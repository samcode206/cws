#include "sock_util.h"
#include "ws.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#define MAX_CONNS 1
#define PORT 9919
#define ADDR "::1"

#define MAX_BUFFERED_BYTES 2048

#define MSG_SIZE 1024 * 1024
char msg[MSG_SIZE];

static_assert(MAX_BUFFERED_BYTES < MSG_SIZE,
              "MAX_BUFFERED_BYTES should be less than MSG_SIZE for this test");

void doFragmentedSend(ws_conn_t *conn) {
  size_t *sent = ws_conn_ctx(conn);

  size_t max_sendable_fragment_size = ws_conn_max_sendable_len(conn);
  bool is_last_fragment = false;
  size_t fragment_size;

  do {
    // if how much we can send is more than what's left this is the last
    // fragment
    is_last_fragment = max_sendable_fragment_size >= MSG_SIZE - *sent;

    // calculate fragment_size
    fragment_size =
        is_last_fragment ? MSG_SIZE - *sent : max_sendable_fragment_size;

    if (*sent == 0) {
      // if first send, we shouldn't be in the sending fragments state
      assert(ws_conn_sending_fragments(conn) == false);
    }

    // send the fragment
    int stat = ws_conn_send_fragment(conn, msg + *sent, fragment_size, 0,
                                     is_last_fragment);
    *sent += fragment_size;

    // with the given fragment size based on ws_conn_max_sendable_len
    // we shouldn't run into these stats below
    assert(stat != WS_SEND_DROPPED_NEEDS_FRAGMENTATION);
    assert(stat != WS_SEND_DROPPED_TOO_LARGE);

    if (stat == WS_SEND_FAILED) {
      exit(EXIT_FAILURE);
      break;
    }

    // pause for now and wait for on drain
    if (stat == WS_SEND_OK_BACKPRESSURE) {
      break;
    }

    if (is_last_fragment) {
      assert(*sent == MSG_SIZE);
      *sent = 0;
      // we sent the final fragment successfully
      // we now should not be in the sending_fragments state
      // meaning we are allowed to send more non fragmented messages
      assert(ws_conn_sending_fragments(conn) == false);

      // we are no longer in a fragmented state
      // those two calls below MUST not return WS_SEND_DROPPED_NOT_ALLOWED
      assert(ws_conn_send(conn, "hi", 2, 0) != WS_SEND_DROPPED_NOT_ALLOWED);
      assert(ws_conn_send_txt(conn, "hi", 2, 0) != WS_SEND_DROPPED_NOT_ALLOWED);
      printf("[SUCCESS]: sent all fragments\n");
      break;
    } else {
      // sending data shouldn't be allowed per Websocket protocol because we are
      // still not done with the fragmented msg
      assert(ws_conn_send(conn, "hi", 2, 0) == WS_SEND_DROPPED_NOT_ALLOWED);
      assert(ws_conn_send_txt(conn, "hi", 2, 0) == WS_SEND_DROPPED_NOT_ALLOWED);
      assert(ws_conn_put_bin(conn, "hi", 2, 0) == WS_SEND_DROPPED_NOT_ALLOWED);
      assert(ws_conn_put_txt(conn, "hi", 2, 0) == WS_SEND_DROPPED_NOT_ALLOWED);

      // not the final fragment
      // we should still be in the sending_fragments state
      assert(ws_conn_sending_fragments(conn) == true);
    }

  } while ((max_sendable_fragment_size = ws_conn_max_sendable_len(conn)));
}

void onDrain(ws_conn_t *conn) {
  // printf("onDrain: resuming fragmented sends\n");
  // we are still sending fragments after draining backpressure
  // continue sending fragments
  if (ws_conn_sending_fragments(conn))
    doFragmentedSend(conn);
}

void onOpen(ws_conn_t *conn) {
  size_t *sent = malloc(sizeof(size_t));
  assert(sent != NULL);
  *sent = 0;
  ws_conn_set_ctx(conn, sent);

  assert(ws_conn_can_put_msg(conn, MSG_SIZE) == 0); // should never fit
  assert(ws_conn_can_put_msg(conn, 1024) == 1);     // should fit

  // is this too large of a message?
  if (!ws_conn_can_put_msg(conn, MSG_SIZE)) {
    doFragmentedSend(conn);
  }
}

void onMsg(ws_conn_t *conn, void *msg, size_t n, bool bin) {}

void onDisconnect(ws_conn_t *conn, int err) {
  assert(ws_conn_ctx(conn));
  free(ws_conn_ctx(conn));
  ws_conn_set_ctx(conn, NULL);
}

void onPong(ws_conn_t *conn, void *msg, size_t n) {
  // printf("pong recv\n");
}

void *server_init(void *_) {
  struct ws_server_params p = {
      .addr = ADDR,
      .port = PORT,
      .on_ws_open = onOpen,
      .on_ws_msg = onMsg,
      .on_ws_disconnect = onDisconnect,
      .max_buffered_bytes = MAX_BUFFERED_BYTES,
      .max_conns = MAX_CONNS,
      .on_ws_drain = onDrain,
      .on_ws_pong = onPong,
  };

  int stat;
  ws_server_t *s = ws_server_create(&p, &stat);
  ws_server_start(s, 8);

  return NULL;
}

void test_client_init() {
  int fd = sock_new_connect(PORT, ADDR);
  sock_upgrade_ws(fd);

  // +10 for header
  char *buf = malloc(MSG_SIZE);
  size_t fragments_len = 0;
  assert(buf != NULL);
  unsigned char header[10];

  // build up the message from the server's sent frames
  while (fragments_len < MSG_SIZE) {
    // read the first two bytes from the header
    assert(sock_recvall(fd, header, 2) == 2);
    unsigned opcode = frame_get_opcode(header);
    unsigned fin = frame_get_fin(header);

    size_t payload_len;
    int needed_header = frame_decode_payload_len(header, 2, &payload_len);
    if (needed_header != 0) {
      // read remaining header bytes to get the payload length
      assert(sock_recvall(fd, header + 2, needed_header - 2) ==
             needed_header - 2);

      // try to get payload_len again
      needed_header =
          frame_decode_payload_len(header, needed_header, &payload_len);
      // we should have the entire header and the payload_len should have been
      // read
      assert(needed_header == 0);
    };

    if (fragments_len == 0) {
      assert(!fin);
      assert(opcode == OP_BIN);
    } else {
      assert(opcode == 0); // expect continuation
    }

    // read the payload portion of the frame into buf
    assert(sock_recvall(fd, buf + fragments_len, payload_len) == payload_len);

    fragments_len += payload_len;

    if (fin) {
      // if fin make sure what was accumulated matches MSG_SIZE
      assert(fragments_len == MSG_SIZE);
    }
  }

  assert(fragments_len == MSG_SIZE);
  if (memcmp(buf, msg, MSG_SIZE) == 0) {
    printf("PASS\n");
  } else {
    printf("FAIL Received Message doesn't match expected\n");
  }
}

int main(void) {

  FILE *f = fopen("/dev/urandom", "r");
  size_t n = fread(msg, 1, MSG_SIZE, f);
  assert(n == MSG_SIZE);
  fclose(f);

  pthread_t server_w;

  if (pthread_create(&server_w, NULL, server_init, NULL) == -1) {
    perror("pthread_create");
    exit(EXIT_FAILURE);
  };

  sleep(1);

  test_client_init();

  return 0;
}

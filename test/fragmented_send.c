#include "ws.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#define MAX_CONNS 1024

size_t msg_sz = 1024*1024*64;
char large_msg[1024*1024*64] = {0};


void doFragmentedSend(ws_conn_t *conn) {
  size_t *sent = ws_conn_ctx(conn);

  size_t max_sendable_fragment_size = ws_conn_max_sendable_len(conn);
  bool is_last_fragment = false;
  size_t fragment_size;

  size_t iterations = 0;

  // alternate sending fragmented messages and pings
  // this is for demonstration only
  do {
    iterations += 1;
    if (iterations % 2 == 0) {
      // this should be allowed to go through
      // interleaved control messages are okay during fragmentation

      // try both styles of pinging
      // this should make no difference but only here to confirm
      if (iterations % 4) {
        // place it in the buffer sending will be async
        assert(ws_conn_put_ping(conn, "ping", 4) !=
               WS_SEND_DROPPED_NOT_ALLOWED);
      } else {
        // place in the buffer and send it right after if possible
        assert(ws_conn_ping(conn, "hi", 2) != WS_SEND_DROPPED_NOT_ALLOWED);
      }

    } else {
      is_last_fragment = max_sendable_fragment_size >= msg_sz - *sent;
      fragment_size =
          is_last_fragment ? msg_sz - *sent : max_sendable_fragment_size;

      // printf("before state *************************\n");
      // printf("fragment_size = %zu\n", fragment_size);
      // printf("send index = %zu\n", sent);
      // printf("is last = %d\n", is_last_fragment);

      if (*sent == 0) {
        // if first send, we shouldn't be in the sending fragments state
        assert(ws_conn_sending_fragments(conn) == false);
      }

      int stat = ws_conn_send_fragment(conn, large_msg + *sent, fragment_size,
                                       0, is_last_fragment);
      *sent += fragment_size;

      assert(stat != WS_SEND_DROPPED_NEEDS_FRAGMENTATION);
      assert(stat != WS_SEND_DROPPED_TOO_LARGE);
      if (stat == WS_SEND_FAILED) {
        exit(EXIT_FAILURE);
        break;
      }

      if (stat == WS_SEND_OK_BACKPRESSURE) {
        break;
      }

      // printf("after state *************************\n");
      // printf("sent index = %zu\n", sent);

      if (is_last_fragment) {
        assert(*sent == msg_sz);
        *sent = 0;
        // we sent the final fragment successfully
        // we now should not be in the sending_fragments state
        // meaning we are allowed to send more non fragmented messages
        assert(ws_conn_sending_fragments(conn) == false);

        // we are no longer in a fragmented state
        // those two calls below MUST not return WS_SEND_DROPPED_NOT_ALLOWED
        assert(ws_conn_send(conn, "hi", 2, 0) != WS_SEND_DROPPED_NOT_ALLOWED);
        assert(ws_conn_send_txt(conn, "hi", 2, 0) !=
               WS_SEND_DROPPED_NOT_ALLOWED);
        printf("[SUCCESS]: sent all fragments\n");
        break;
      } else {
        // sending data
        assert(ws_conn_send(conn, "hi", 2, 0) == WS_SEND_DROPPED_NOT_ALLOWED);
        assert(ws_conn_send_txt(conn, "hi", 2, 0) ==
               WS_SEND_DROPPED_NOT_ALLOWED);

        // not the final fragment
        // we should still be in the sending_fragments state
        assert(ws_conn_sending_fragments(conn) == true);
      }
    }
  } while ((max_sendable_fragment_size = ws_conn_max_sendable_len(conn)));
}

void onDrain(ws_conn_t *conn) {
  // printf("onDrain: resuming fragmented sends\n");
  // we are still sending fragments after draining backpressure
  // continue sending fragments
  doFragmentedSend(conn);
}

void onOpen(ws_conn_t *conn) {
  size_t *sent = malloc(sizeof(size_t));
  assert(sent != NULL);
  *sent = 0;
  ws_conn_set_ctx(conn, sent);

  assert(ws_conn_can_put_msg(conn, msg_sz) == 0); // should never fit
  assert(ws_conn_can_put_msg(conn, 1024) == 1);   // should fit

  // is this too large of a message?
  if (!ws_conn_can_put_msg(conn, msg_sz)) {
    doFragmentedSend(conn);
  }
}

void onMsg(ws_conn_t *conn, void *msg, size_t n, bool bin) {
  if (ws_conn_sending_fragments(conn)) {
    assert(ws_conn_send(conn, msg, n, 0) == WS_SEND_DROPPED_NOT_ALLOWED);
  } else {
    // you can fail if you want just never with WS_SEND_DROPPED_NOT_ALLOWED
    assert(ws_conn_send(conn, msg, n, 0) != WS_SEND_DROPPED_NOT_ALLOWED);
  }

  // printf("msg recv\n");
}

void onDisconnect(ws_conn_t *conn, int err) {
  assert(ws_conn_ctx(conn));
  free(ws_conn_ctx(conn));
  ws_conn_set_ctx(conn, NULL);
}

void onPong(ws_conn_t *conn, void *msg, size_t n) {
  // printf("pong recv\n");
}

int main(void) {
  printf("sending fragmented message example starting on 9919\n");

  struct ws_server_params p = {
      .addr = "::1",
      .port = 9919,
      .on_ws_open = onOpen,
      .on_ws_msg = onMsg,
      .on_ws_disconnect = onDisconnect,
      .max_buffered_bytes = 2048,
      .max_conns = MAX_CONNS,
      .on_ws_drain = onDrain,
      .on_ws_pong = onPong,
  };

  int stat;
  ws_server_t *s = ws_server_create(&p, &stat);

  ws_server_start(s, 1024);
  return 0;
}

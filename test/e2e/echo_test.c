#include "../../src/ws.h"
#include "../test.h"
#include "sock_util.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__linux__)
#include <sys/eventfd.h>

#else
#include <sys/event.h>
#endif

#define PORT 9919
#define ADDR "::1"

ws_server_t *s;
int done;

void server_on_open(ws_conn_t *conn) {}

void server_on_msg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  ws_conn_send_msg(conn, msg, n, OP_BIN, 0);
}

void server_on_disconnect(ws_conn_t *conn, unsigned long err) {}

void *server_init(void *_) {
  struct ws_server_params p = {
      .addr = ADDR,
      .port = PORT,
      .on_ws_open = server_on_open,
      .on_ws_msg = server_on_msg,
      .on_ws_disconnect = server_on_disconnect,
      .max_buffered_bytes = 2048,
      .max_conns = 2,
  };

  s = ws_server_create(&p);
  assert(s != NULL);

  int ret = ws_server_start(s, 1024);
  assert(ret == 0);

  assert(ws_server_destroy(s) == 0);

  return NULL;
}

void async_shutdown(ws_server_t *s, void *ctx) {
  ws_server_shutdown(s);
#if defined(__linux__)
  assert(eventfd_write(done, 1) == 0);
#else
  struct kevent ev;
  EV_SET(&ev, 0, EVFILT_USER, EV_ADD | EV_ONESHOT, NOTE_TRIGGER, 0, NULL);
  kevent(done, &ev, 1, NULL, 0, NULL);
#endif
}

int ECHO_TEST(const char *name) {
  pthread_t server_w;

  if (pthread_create(&server_w, NULL, server_init, NULL) == -1) {
    perror("pthread_create");
    exit(EXIT_FAILURE);
  };

  sleep(1);

  int fd = sock_new_connect(PORT, ADDR);
  sock_upgrade_ws(fd);

  int runs = 300;

  char out_buf[256];
  char in_buf[256];

  unsigned frame_cfg = OP_BIN | 0x80;

  while (runs--) {
    if (runs < 200 && runs > 100) {
      frame_cfg = OP_TXT | 0x80;
    } else if (runs < 100) {
      frame_cfg = OP_PING | 0x80;
    }

    int msg_len = sprintf(out_buf, "count %d", runs);
    unsigned char *frame = new_frame(out_buf, msg_len, frame_cfg);
    ssize_t sent = sock_sendall(fd, frame, msg_len + 6);
    assert(sent == msg_len + 6);
    ssize_t read = sock_recvall(fd, in_buf, msg_len + 2);
    assert(read == msg_len + 2);

    if (memcmp(in_buf + 2, out_buf, msg_len) != 0) {
      fprintf(stderr, "mismatched data received expected: %s got %.*s\n",
              out_buf, msg_len, in_buf);
    }

    free(frame);
  }

#if defined(__linux__)
  done = eventfd(0, EFD_CLOEXEC);
  assert(done != -1);

  assert(ws_server_sched_callback(s, async_shutdown, NULL) == 0);

  eventfd_t value;
  eventfd_read(done, &value);

  (void)value;
#else
  done = kqueue();
  assert(done != -1);
  assert(ws_server_sched_callback(s, async_shutdown, NULL) == 0);

  struct kevent ev;
  kevent(done, NULL, 0, &ev, 1, NULL);
#endif

  return EXIT_SUCCESS;
}

#define NUN_TESTS 1

struct test_table tests[NUN_TESTS] = {
    {"ECHO_TEST", ECHO_TEST},
};

int main(void) { RUN_TESTS("Echo", tests, NUN_TESTS); }

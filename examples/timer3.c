#include "ws.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/signal.h>
#include <time.h>

#define MAX_CONNS 1024

#define MS *1000000
#define US *1000

void onOpen(ws_conn_t *conn) {}

void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  ws_conn_put_msg(conn, msg, n, opcode, 0);
}

void on_timeout(ws_server_t *s, void *ctx) {
  fprintf(stderr, "Cancellation Failed\n");
  exit(EXIT_FAILURE);
}

void onDisconnect(ws_conn_t *conn, unsigned long err) {}

int main(void) {
  signal(SIGPIPE, SIG_IGN);

  struct ws_server_params p = {
      .addr = "::1",
      .port = 9919,
      .on_ws_open = onOpen,
      .on_ws_msg = onMsg,
      .on_ws_disconnect = onDisconnect,
      .max_buffered_bytes = 1024 * 512,
      .max_conns = MAX_CONNS,
      .silent = 0,
  };

  ws_server_t *s = ws_server_create(&p);

  struct timespec timeout1 = {.tv_sec = 3, .tv_nsec = 0};

  uint64_t tid1 = ws_server_set_timeout(s, &timeout1, NULL, on_timeout);
  
  struct timespec timeout2 = {.tv_sec = 10, .tv_nsec = 0};

  uint64_t tid2 = ws_server_set_timeout(s, &timeout2, NULL, on_timeout);

  ws_server_cancel_timeout(s, tid1);
  ws_server_cancel_timeout(s, tid2);


    ws_server_cancel_timeout(s, 321412412); // invalid tid

  ws_server_start(s, 1024);

  return 0;
}

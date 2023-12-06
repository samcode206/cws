#include "../src/ws.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#define MAX_CONNS 1024

void onOpen(ws_conn_t *conn) {}

void onMsg(ws_conn_t *conn, void *msg, size_t n, bool bin) {
  printf("msg %zu\n", n);
  ws_conn_send(conn, msg, n, 0);
}

void onDisconnect(ws_conn_t *conn, int err) {
  printf("%s\n", ws_conn_strerror(conn));
}

int main(void) {
  printf("echo example starting on 9919\n");

  struct ws_server_params p = {
      .addr = "::1",
      .port = 9919,
      .on_ws_open = onOpen,
      .on_ws_msg = onMsg,
      .on_ws_disconnect = onDisconnect,
      .max_buffered_bytes = 2048,
      .max_conns = MAX_CONNS,
  };

  int stat;
  ws_server_t *s = ws_server_create(&p, &stat);

  ws_server_start(s, 1024);
  return 0;
}

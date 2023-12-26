#include "ws.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signal.h>

#define MAX_CONNS 1024

void onOpen(ws_conn_t *conn) {}

void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  if (ws_conn_msg_ready(conn)) {
    ws_conn_put_msg(conn, msg, n, OP_BIN, 0);
  } else {
    ws_conn_send_msg(conn, msg, n, OP_BIN, 0);
  }
}

void onDisconnect(ws_conn_t *conn, int err) {}

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

  ws_server_start(s, 1024);
  return 0;
}

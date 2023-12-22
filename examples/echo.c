#include "ws.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signal.h>
#include <sys/timerfd.h>

#define MAX_CONNS 1024

size_t total = 0;

void onOpen(ws_conn_t *conn) {}

void onUpgrade(ws_conn_t *conn, struct ws_conn_handshake *hs) {
  struct http_header headers[5] = {
      {
          "Upgrade",
          "websocket",
      },
      {
          "Connection",
          "Upgrade",
      },
      {
          "Server",
          "cws",
      },
      {
        "Access-Control-Allow-Origin",
        "*",
      },
      {
          "Sec-WebSocket-Accept",
          hs->sec_websocket_accept,
      },
  };

  struct ws_conn_handshake_response res = {
      .upgrade = 1,
      .body = NULL,
      .header_count = 5,
      .per_msg_deflate = 1,
      .status = "101 Switching Protocols",
      .headers = headers,
  };

 ws_conn_handshake_reply(conn, &res);


}

void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  // printf("msg %zu\n", n);
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
      .on_ws_handshake = onUpgrade,
      .verbose = 1,
  };

  ws_server_t *s = ws_server_create(&p);

  ws_server_start(s, 1024);
  return 0;
}

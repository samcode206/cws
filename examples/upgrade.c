#include "ws.h"
#include <assert.h>
#include <stdio.h>
#include <sys/signal.h>

#define MAX_CONNS 1024

void onOpen(ws_conn_t *conn) {
  int status = ws_conn_send_msg(conn, "welcome", 7, OP_TXT, 0);
  assert(status == WS_SEND_OK);
}

void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  ws_conn_put_msg(conn, msg, n, opcode, 0);
}

void onHandshakeRequest(ws_conn_t *c, struct ws_conn_handshake *hs) {
  struct http_header hdrs[] = {{"Access-Control-Allow-Origin", "*"},
                               {
                                   "Sec-Websocket-Accept",
                                   hs->sec_websocket_accept,
                               }};

  struct ws_conn_handshake_response resp = {
      .status = WS_HANDSHAKE_STATUS_101,
      .body = NULL,
      .header_count = 2,
      .headers = hdrs,

  };
  ws_conn_handshake_reply(c, &resp);
}

void onDisconnect(ws_conn_t *conn, unsigned long err) {}

int main(void) {

  signal(SIGPIPE, SIG_IGN);
  printf("starting server on ws://localhost:9919/\n");
  struct ws_server_params p = {
      .addr = "::1",
      .port = 9919,
      .on_ws_open = onOpen,
      .on_ws_msg = onMsg,
      .on_ws_handshake = onHandshakeRequest,
      .on_ws_disconnect = onDisconnect,
      .max_conns = MAX_CONNS,
  };

  ws_server_t *s = ws_server_create(&p);

  ws_server_start(s, 1024);
  return 0;
}

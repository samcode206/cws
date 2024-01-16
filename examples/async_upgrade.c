#include "ws.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/signal.h>

#define MAX_CONNS 1024

void onOpen(ws_conn_t *conn) {}

void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  ws_conn_put_msg(conn, msg, n, opcode, 0);
}

typedef struct {
  ws_conn_t *connection;
  char *origin;
  char acceptKey[];
} AsyncUpgrade;

void onWsUpgradeTime(ws_server_t *s, AsyncUpgrade *ctx) {
  printf("Origin: %s\n", ctx->origin);

  struct http_header hdrs[] = {{"Access-Control-Allow-Origin", "*"},
                               {
                                   "Sec-Websocket-Accept",
                                   ctx->acceptKey,
                               }};

  struct ws_conn_handshake_response resp = {
      .status = WS_HANDSHAKE_STATUS_101,
      .header_count = 2,
      .headers = hdrs,
      .body = NULL,
  };

  int ret = ws_conn_handshake_reply(ctx->connection, &resp);
  assert(ret == WS_SEND_OK);

  free(ws_conn_ctx(ctx->connection));
  ws_conn_set_ctx(ctx->connection, NULL);
  free(ctx->origin);
  free(ctx);
}

void onHandshakeRequest(ws_conn_t *c, struct ws_conn_handshake *hs) {
  size_t keyLen = strlen(hs->sec_websocket_accept);
  AsyncUpgrade *au = malloc(sizeof(AsyncUpgrade) + keyLen);
  if (au == NULL) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }

  au->connection = c;

  const struct http_header *hdr = ws_conn_handshake_header_find(hs, "Origin");
  au->origin = hdr ? strdup(hdr->val) : NULL;
  memcpy(au->acceptKey, hs->sec_websocket_accept, keyLen);

  struct timespec ts = {
      .tv_sec = 1,
      .tv_nsec = 0,
  };

  uint64_t timeoutHandle = ws_server_set_timeout(ws_conn_server(c), &ts, au,
                                                 (ws_timeout_cb_t)onWsUpgradeTime);

  if (timeoutHandle) {
    ws_conn_set_ctx(c, malloc(sizeof(uint64_t)));
    assert(ws_conn_ctx(c) != NULL);
    *(uint64_t *)ws_conn_ctx(c) = timeoutHandle;
  }
}

void onDisconnect(ws_conn_t *conn, unsigned long err) {
  if (ws_conn_ctx(conn)) {
    ws_server_cancel_timeout(ws_conn_server(conn),
                             *(uint64_t *)ws_conn_ctx(conn));
    free(ws_conn_ctx(conn));
  }
}

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
      .log_params = 1,
  };

  ws_server_t *s = ws_server_create(&p);

  if (s)
    ws_server_start(s, 1024);
  else
    return EXIT_FAILURE;

  return EXIT_SUCCESS;
}

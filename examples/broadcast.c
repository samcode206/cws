#include "ws.h"
#include <assert.h>
#include <stdio.h>

#define MAX_CONNS 1024

typedef struct {
  size_t numConnections;
  ws_conn_t *conns[MAX_CONNS];
} App;

static void appBroadcast(App *state, char *msg, size_t len) {
  while (state->numConnections) {
    size_t i = state->numConnections;
    while (i--) {
      if (!ws_conn_can_put_msg(state->conns[i], len)) {
        ws_conn_flush_pending(state->conns[i]);
      }

      ws_conn_put_txt(state->conns[i], msg, len, 0);

      // ws_conn_send_txt(state->conns[i], msg, len, 0);
    }

    break;
  }
}

static void appNewConnection(App *state, ws_conn_t *conn) {
  state->conns[state->numConnections++] = conn;
}

static void appDisconnect(App *state, ws_conn_t *conn) {

  while (state->numConnections) {
    size_t i = state->numConnections;
    while (i--) {
      if (state->conns[i] == conn) {
        ws_conn_t *tmp = state->conns[state->numConnections - 1];
        state->numConnections--;
        state->conns[i] = tmp;
        break;
      }
    }

    break;
  }
}

void onOpen(ws_conn_t *conn) {
  // printf("on Open\n");

  appNewConnection(ws_server_ctx(ws_conn_server(conn)), conn);
}

void onMsg(ws_conn_t *conn, void *msg, size_t n, bool bin) {
  appBroadcast(ws_server_ctx(ws_conn_server(conn)), msg, n);
}

void onDisconnect(ws_conn_t *conn, int err) {
  // printf("on Disconnect\n");
  appDisconnect(ws_server_ctx(ws_conn_server(conn)), conn);
}

int main(void) {
  printf("broadcast example starting on 9919\n");

  App *state = calloc(1, sizeof *state);
  assert(state != NULL);

  struct ws_server_params p = {
      .addr = "::1",
      .port = 9919,
      .on_ws_open = onOpen,
      .on_ws_msg = onMsg,
      .on_ws_disconnect = onDisconnect,
      .max_buffered_bytes = 1024 * 512,
      .max_conns = MAX_CONNS,
      .ctx = state,
  };

  ws_server_t *s = ws_server_create(&p);
  ws_server_start(s, 1024);
  return 0;
}

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
      if (ws_conn_max_sendable_len(state->conns[i]) < len + 10) {
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

static App *state;

void onOpen(ws_conn_t *conn) {
  // printf("on Open\n");
  appNewConnection(state, conn);
}

void onMsg(ws_conn_t *conn, void *msg, size_t n, bool bin) {
  appBroadcast(state, msg, n);
}

void onDisconnect(ws_conn_t *conn, int err) {
  // printf("on Disconnect\n");
  appDisconnect(state, conn);
}

int main(void) {
  printf("broadcast example starting on 9919\n");

  state = calloc(1, sizeof *state);
  assert(state != NULL);

  struct ws_server_params p = {
      .addr = "::1",
      .port = 9919,
      .on_ws_open = onOpen,
      .on_ws_msg = onMsg,
      .on_ws_disconnect = onDisconnect,
      .max_buffered_bytes = 1024 * 1024 * 32,
      .max_conns = MAX_CONNS,
  };

  
  ws_server_t *s = ws_server_create(&p);
  ws_server_start(s, 1024);
  return 0;
}

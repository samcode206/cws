#include "../src/ws.h"
#include <assert.h>
#include <stdio.h>

#define MAX_CONNS 1024

typedef struct {
  size_t numConnections;
  ws_conn_t *conns[MAX_CONNS];
} AppState;

static void appStateAnnounceChange(AppState *state) {
  char msg[256];
  int count =
      sprintf(msg, "New Connection Count: %zu\n", state->numConnections);

  assert(count > 0);

  while (state->numConnections) {
    size_t i = state->numConnections;
    while (i--) {
      ws_conn_send_txt(state->conns[i], msg, count, 0);
    }
    break;
  }
}

static void appStateNewConnection(AppState *state, ws_conn_t *conn) {
  state->conns[state->numConnections++] = conn;
  appStateAnnounceChange(state);
}

static void appStateDisconnect(AppState *state, ws_conn_t *conn) {

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

  appStateAnnounceChange(state);
}

static AppState *state;

void onOpen(ws_conn_t *conn) {
  printf("on Open\n");
  appStateNewConnection(state, conn);
}

void onMsg(ws_conn_t *conn, void *msg, size_t n, bool bin) {
  fprintf(stdout, "got message: %.*s\n", (int)n, (char *)msg);
}

void onDisconnect(ws_conn_t *conn, int err) {
  printf("on Disconnect\n");
  appStateDisconnect(state, conn);
}

int main(void) {
  printf("online example starting on 9919\n");
  
  state = calloc(1, sizeof *state);
  assert(state != NULL);

  struct ws_server_params p = {
      .addr = "::1",
      .port = 9919,
      .on_ws_open = onOpen,
      .on_ws_msg = onMsg,
      .on_ws_disconnect = onDisconnect,
      .max_buffered_bytes = 1024,
      .max_conns = MAX_CONNS,
  };

  int stat;
  ws_server_t *s = ws_server_create(&p, &stat);
  ws_server_start(s, 1024);
  return 0;
}

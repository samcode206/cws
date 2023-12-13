#include "../src/ws.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_CONNS 1024
#define NUM_SERVERS 4

typedef struct {
  ws_server_t *servers[NUM_SERVERS];
} App;

typedef struct {
  unsigned long server_idx;
  size_t numConnections;
  ws_conn_t *conns[MAX_CONNS];
  App *app;
} Slice;

typedef struct {
  size_t msg_len;
  void *msg;
  size_t refs;
} BroadcastRequest;

void onOpen(ws_conn_t *conn) {
  ws_server_t *s = ws_conn_server(conn);
  Slice *ctx = ws_server_ctx(s);
  ctx->conns[ctx->numConnections++] = conn;
}

void broadcast(ws_server_t *s, async_cb_ctx_t *ctx) {
  Slice *slc = ws_server_ctx(s);
  BroadcastRequest *req = ctx->ctx;

  for (size_t i = 0; i < slc->numConnections; i++) {
    ws_conn_send(slc->conns[i], req->msg, req->msg_len, 0);
  }

  req->refs--;
  
  assert(req->refs <= NUM_SERVERS);

  if (!req->refs) {
    free(req->msg);
    free(req);
    free(ctx);
  }
}

void onMsg(ws_conn_t *conn, void *msg, size_t n, bool bin) {
  ws_server_t *s = ws_conn_server(conn);
  Slice *ctx = ws_server_ctx(s);

  struct async_cb_ctx *cbinfo = malloc(sizeof(struct async_cb_ctx));
  BroadcastRequest *req = malloc(sizeof(BroadcastRequest));

  req->msg = malloc(n);

  memcpy(req->msg, msg, n);
  req->msg_len = n;
  req->refs = NUM_SERVERS;

  cbinfo->cb = broadcast;
  cbinfo->ctx = req;

  for (size_t i = 0; i < NUM_SERVERS; i++) {
    ws_server_sched_async(ctx->app->servers[i], cbinfo);
  }
}

void onDisconnect(ws_conn_t *conn, int err) {
  ws_server_t *s = ws_conn_server(conn);
  Slice *ctx = ws_server_ctx(s);
  if (ctx->numConnections) {
    size_t i = ctx->numConnections;
    while (i--) {
      if (ctx->conns[i] == conn) {
        ws_conn_t *tmp = ctx->conns[ctx->numConnections - 1];
        ctx->numConnections--;
        ctx->conns[i] = tmp;
        break;
      }
    }
  }
}

void *server_init(void *s) {
  printf("broadcast example starting on 9919\n");

  ws_server_start(s, 1024);

  return NULL;
}

int main(void) {

  App *state = calloc(1, sizeof *state);
  assert(state != NULL);

  for (size_t i = 0; i < NUM_SERVERS; i++) {
    Slice *slc = calloc(1, sizeof(Slice));
    slc->app = state;
    slc->server_idx = i;

    struct ws_server_params p = {
        .addr = "::1",
        .port = 9919,
        .on_ws_open = onOpen,
        .on_ws_msg = onMsg,
        .on_ws_disconnect = onDisconnect,
        .max_buffered_bytes = 1024 * 512,
        .max_conns = MAX_CONNS,
        .ctx = slc,
    };

    state->servers[i] = ws_server_create(&p);
  }

  pthread_t server_threads[NUM_SERVERS];

  for (size_t i = 0; i < NUM_SERVERS; i++) {
    if (pthread_create(&server_threads[i], NULL, server_init,
                       state->servers[i]) == -1) {
      perror("pthread_create");
      exit(EXIT_FAILURE);
    }
  }

  pthread_join(server_threads[0], NULL);

  return 0;
}

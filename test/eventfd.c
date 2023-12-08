#include "../src/ws.h"
#include <pthread.h>
#include <stdio.h>
#include <assert.h>

#define PORT 9919
#define ADDR "::1"

ws_server_t *srv;

void server_on_open(ws_conn_t *conn) {}

void server_on_msg(ws_conn_t *conn, void *msg, size_t n, bool bin) {
  // printf("msg %zu\n", n);
  ws_conn_send(conn, msg, n, 0);
}

void server_on_disconnect(ws_conn_t *conn, int err) {
  printf("%s\n", ws_conn_strerror(conn));
}

void *server_init(void *_) {
  struct ws_server_params p = {
      .addr = ADDR,
      .port = PORT,
      .on_ws_open = server_on_open,
      .on_ws_msg = server_on_msg,
      .on_ws_disconnect = server_on_disconnect,
      .max_buffered_bytes = 512,
      .max_conns = 2,
  };

  int stat;
  srv = ws_server_create(&p, &stat);

  ws_server_start(srv, 1024);

  return NULL;
}

void server_async_task(ws_server_t *rs, async_cb_ctx_t *ctx) { 
    printf("Hello I am running on %p\n", (void*)rs);
    assert(srv == rs);

    ws_server_sched_async(rs, ctx);
 }

int main() {
  pthread_t server_w;

  if (pthread_create(&server_w, NULL, server_init, NULL) == -1) {
    perror("pthread_create");
    exit(EXIT_FAILURE);
  };

  sleep(1);

  struct async_cb_ctx *task_info = malloc(sizeof (struct async_cb_ctx));
  task_info->ctx = NULL;
  task_info->cb = server_async_task;

  ws_server_sched_async(srv, task_info);
  


  sleep(10);
}

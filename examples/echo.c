#include "ws.h"
#include <stdio.h>
#include <sys/signal.h>


#define MAX_CONNS 1024

void onOpen(ws_conn_t *conn) {}

void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  ws_conn_put_msg(conn, msg, n, opcode, 0);
}


void on_timeout(ws_server_t *s, ws_timer_t *ctx){
  printf("on timeout\n");

  if (ctx->timeout_ms > 1){
    ctx->timeout_ms -= 1;
    ws_server_set_timeout(s, ctx);
    return;
  }

  // ws_server_set_timeout(s, ctx);
  free(ctx);
}

void onDisconnect(ws_conn_t *conn, unsigned long err) {}

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

  ws_timer_t *t = malloc(sizeof(ws_timer_t));
  t->timeout_ms = 10;
  t->cb = on_timeout;

  ws_server_set_timeout(s, t);

  ws_server_start(s, 1024);
  

  
  return 0;
}

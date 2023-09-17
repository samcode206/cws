#include "ws.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>

ws_server_t *s;

void on_open(ws_conn_t *c) {
  printf("websocket connection open %p\n", (void *)c);
}

void on_ping(ws_conn_t *c, void *msg, size_t n, bool bin) {

  ws_conn_pong(s, c, msg, n, bin);



  printf("on_ping: %s\n", (char *)msg);

}

void on_msg(ws_conn_t *c, void *msg, uint8_t *mask, size_t n, bool bin) {
  frame_payload_unmask(msg, msg, mask, n);
  printf("on_msg: %s\n", (char *)msg);
}

void on_close(ws_conn_t *ws_conn, int reason) { printf("on_close\n"); }

void on_destroy(ws_conn_t *ws_conn) { printf("on_destroy\n"); }

void on_drain(ws_conn_t *ws_conn) { printf("on_drain\n"); }

int main(void) {
  struct ws_server_params sp = {
      .addr = INADDR_ANY,
      .port = 9919,
      .max_events = 1024,
      .on_ws_open = on_open,
      .on_ws_msg = on_msg,
      .on_ws_ping = on_ping,
      .on_ws_drain = on_drain,
      .on_ws_close = on_close,
      .on_ws_destroyed = on_destroy,
  };

  int ret = 0;
  s = ws_server_create(&sp, &ret);

  if (ret < 0) {
    ws_write_err(STDERR_FILENO, ret);
    exit(ret);
  }

  printf("websocket server starting on port : %d\n", sp.port);

  ret = ws_server_start(s, 1024);

  exit(ret);
}

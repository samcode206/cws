#include "ws.h"

void on_open(ws_conn_t *c) { ws_conn_ping(ws_conn_server(c), c, "welcome", 8); }

void on_ping(ws_conn_t *c, void *msg, size_t n) {
  printf("on_ping: %s\n", (char *)msg);
  int stat = ws_conn_pong(ws_conn_server(c), c, msg, n);
  if (stat == 1) {
    printf("pong sent\n");
  } else {
    printf("partial pong sent or an error occurred waiting for <on_ws_drain | "
           "on_ws_disconnect>\n");
  }
}

void on_pong(ws_conn_t *c, void *msg, size_t n) {
  printf("on_pong: %s\n", (char *)msg);
}

void on_msg(ws_conn_t *c, void *msg, size_t n, bool bin) {
  msg_unmask(msg, msg, n);
  printf("on_msg: %s\n", (char *)msg);
  int stat = ws_conn_send_txt(ws_conn_server(c), c, msg, n);
  if (stat == 1) {
    printf("msg sent\n");
  } else {
    printf("partial send or an error occurred waiting for <on_ws_drain | "
           "on_ws_disconnect>\n");
  }
}

void on_close(ws_conn_t *ws_conn, int code, const void *reason) { 
  printf("on_close, code: %d reason: %s\n", code, (char *)reason);
  ws_conn_close(ws_conn_server(ws_conn), ws_conn, (void *)reason, strlen(reason), code);
}

void on_disconnect(ws_conn_t *ws_conn, int err) { printf("on_disconnect\n"); }

void on_drain(ws_conn_t *ws_conn) { printf("on_drain\n"); }

void on_server_err(ws_server_t *s, int err) {
  fprintf(stderr, "on_server_err: %s\n", strerror(err));
}

int main(void) {
  const size_t max_events = 1024;
  const uint16_t port = 9919;
  const int backlog = 1024;
  struct ws_server_params sp = {.addr = INADDR_ANY,
                                .port = port,
                                .max_events = max_events,
                                .on_ws_open = on_open,
                                .on_ws_msg = on_msg,
                                .on_ws_ping = on_ping,
                                .on_ws_pong = on_pong,
                                .on_ws_drain = on_drain,
                                .on_ws_close = on_close,
                                .on_ws_disconnect = on_disconnect,
                                .on_ws_err = on_server_err};

  int ret = 0;
  ws_server_t *s = ws_server_create(&sp, &ret);

  if (ret < 0) {
    fprintf(stderr, "ws_server_create: %d\n", ret);
    exit(1);
  }

  printf("websocket server starting on port : %d\n", sp.port);

  ret = ws_server_start(s, backlog);

  exit(ret);
}

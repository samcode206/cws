# cws


### Example Sever

```c

#include "ws.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

void on_open(ws_conn_t *c) { ws_conn_ping(ws_conn_server(c), c, "welcome", 8); }

void on_ping(ws_conn_t *c, void *msg, size_t n) {
  printf("on_ping: %s\n", (char *)msg);
  int stat = ws_conn_pong(ws_conn_server(c), c, msg, n);
  if (stat == 1) {
    printf("pong sent\n");
  } else {
    printf("partial send or an error occurred waiting for <on_ws_drain | "
           "on_ws_destroyed>\n");
  }
}

void on_msg(ws_conn_t *c, void *msg, size_t n, bool bin) {
  msg_unmask(msg, msg, n);
  printf("on_msg: %s\n", (char *)msg);
  int stat = ws_conn_send_txt(ws_conn_server(c), c, msg, n);
  if (stat == 1) {
    printf("msg sent\n");
  } else {
    printf("partial send or an error occurred waiting for <on_ws_drain | "
           "on_ws_destroyed>\n");
  }
}

void on_close(ws_conn_t *ws_conn, int reason) { printf("on_close\n"); }

void on_disconnect(ws_conn_t *ws_conn, int err) { printf("on_disconnect\n"); }

void on_drain(ws_conn_t *ws_conn) { printf("on_drain\n"); }

void on_server_err(ws_server_t *s, int err) {
  fprintf(stderr, "on_server_err: %s\n", strerror(err));
}

int main(void) {
  struct ws_server_params sp = {.addr = INADDR_ANY,
                                .port = 9919,
                                .max_events = 1024,
                                .on_ws_open = on_open,
                                .on_ws_msg = on_msg,
                                .on_ws_ping = on_ping,
                                .on_ws_drain = on_drain,
                                .on_ws_close = on_close,
                                .on_ws_disconnect = on_disconnect,
                                .on_ws_err = on_server_err};

  int ret = 0;
  ws_server_t *s = ws_server_create(&sp, &ret);

  if (ret < 0) {
    ws_write_err(STDERR_FILENO, ret);
    exit(ret);
  }

  printf("websocket server starting on port : %d\n", sp.port);

  ret = ws_server_start(s, 1024);

  exit(ret);
}


```

#include "ws.h"


#include <stdio.h>
#include <unistd.h>

void on_open(ws_conn_t *c) {}

void on_ping(ws_conn_t *c, void *msg, size_t n) {
  printf("on_ping:");
  fwrite(msg, sizeof(char), n, stdout);
  fwrite("\n", 1, 1, stdout);
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
  // printf("on_msg: ");
  // fwrite(msg, sizeof(char), n, stdout);
  // fwrite("\n", 1, 1, stdout);
  int stat;
  if (bin) {
    stat = ws_conn_send(ws_conn_server(c), c, msg, n);
  } else {
    stat = ws_conn_send_txt(ws_conn_server(c), c, msg, n);
  }

  if (stat == 1) {
    // printf("msg sent\n");
  } else {
    printf("partial send or an error occurred waiting for <on_ws_drain | "
           "on_ws_disconnect>\n");
  }
}

void on_close(ws_conn_t *ws_conn, int code, const void *reason) {
  printf("on_close, code: %d reason: %s\n", code, (char *)reason);
  if ((code == WS_CLOSE_ABNORM) | (code == WS_CLOSE_NOSTAT)) {
    ws_conn_close(ws_conn_server(ws_conn), ws_conn, (void *)reason, 0,
                  WS_CLOSE_NORMAL);
  } else {
    ws_conn_close(ws_conn_server(ws_conn), ws_conn, (void *)reason, 0, code);
  }
}

void on_disconnect(ws_conn_t *ws_conn, int err) {
  // printf("on_disconnect %d\n", err);
}

void on_drain(ws_conn_t *ws_conn) { printf("on_drain\n"); }

void on_server_err(ws_server_t *s, int err) {
  fprintf(stderr, "on_server_err: %s\n", strerror(err));
}

void *start_server() {
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

  return NULL;
}

#define NUM_THREADS 1
int main(void) {
  printf("%d\n", getpid());
  start_server();
  // pthread_t threads[NUM_THREADS];
  //   int rc;
  //   long t;
  //   for (t = 0; t < NUM_THREADS; t++) {
  //       printf("In main: creating thread %ld\n", t);
  //       rc = pthread_create(&threads[t], NULL, start_server, (void *)t);
  //       if (rc) {
  //           printf("ERROR; return code from pthread_create() is %d\n", rc);
  //           exit(-1);
  //       }
  //   }

  //   // Wait for all threads to complete
  //   for (t = 0; t < NUM_THREADS; t++) {
  //       pthread_join(threads[t], NULL);
  //   }

  //   printf("Main: program exiting.\n");
  //   pthread_exit(NULL);
}

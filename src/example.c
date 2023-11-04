#include "ws.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// fragment accumulator
typedef struct {
  size_t len;
  size_t cap;
  uint8_t data[];
} frag_t;

void on_open(ws_conn_t *c) {}

// void on_ping(ws_conn_t *c, void *msg, size_t n) {
//   printf("on_ping:");
//   fwrite(msg, sizeof(char), n, stdout);
//   fwrite("\n", 1, 1, stdout);
//   int stat = ws_conn_pong(c, msg, n);
//   if (stat == 1) {
//     printf("pong sent\n");
//   } else {
//     printf("partial pong sent or an error occurred waiting for <on_ws_drain |
//     "
//            "on_ws_disconnect>\n");
//   }
// }

void on_msg_fragment(ws_conn_t *c, void *fragment, size_t n, bool fin) {

  frag_t *ctx = ws_conn_ctx(c);
  if (!ctx) {
    ws_conn_set_ctx(c, malloc(sizeof(frag_t) + 512));
    assert((ctx = ws_conn_ctx(c)) != NULL);
    memset(ctx, 0, 16);
  }

  if (ctx->len + n > ctx->cap) {
    size_t new_cap =
        ctx->cap + ctx->cap - ctx->len > n ? ctx->cap + ctx->cap : n + ctx->cap;
    ws_conn_set_ctx(c, realloc(ctx, 16 + new_cap));
    assert((ctx = ws_conn_ctx(c)) != NULL);
    ctx->cap = new_cap;
  }

  memcpy(ctx->data + ctx->len, fragment, n);
  ctx->len += n;

  if (!fin) {
    // printf("received fragment: %.*s\n", (int)n, (char *)fragment);
  } else {
    if (!ws_conn_msg_bin(c)) {
      if (!utf8_is_valid(ctx->data, ctx->len)) {
        ws_conn_close(c, NULL, 0, WS_CLOSE_INVALID);
        return;
      }
      ws_conn_send_txt(c, ctx->data, ctx->len);
    } else {
      ws_conn_send(c, ctx->data, ctx->len);
    }

    // printf("received final fragment: %.*s\n", (int)n, (char *)fragment);
    // printf("full message: %.*s\n", (int)ctx->len, ctx->data);

    free(ws_conn_ctx(c));
    ws_conn_set_ctx(c, NULL);
  }
}

void on_msg(ws_conn_t *c, void *msg, size_t n, bool bin) {
  // printf("on_msg: ");
  // fwrite(msg, sizeof(char), n, stdout);
  // fwrite("\n", 1, 1, stdout);
  if (bin) {
    ws_conn_send(c, msg, n);
  } else {
    ws_conn_send_txt(c, msg, n);
  }

  // if (stat == 1) {
  //   // printf("msg sent\n");
  // } else {
  //   printf("partial send or an error occurred waiting for <on_ws_drain | "
  //          "on_ws_disconnect>\n");
  // }
}

void on_close(ws_conn_t *ws_conn, int code, const void *reason) {
  printf("on_close, code: %d \n", code);
  if ((code == WS_CLOSE_ABNORMAL) | (code == WS_CLOSE_NO_STATUS)) {
    ws_conn_close(ws_conn, (void *)reason, 0, WS_CLOSE_NORMAL);
  } else {
    ws_conn_close(ws_conn, (void *)reason, 0, code);
  }
}

void on_disconnect(ws_conn_t *ws_conn, int err) {
  if (ws_conn_ctx(ws_conn)) {
    free(ws_conn_ctx(ws_conn));
    ws_conn_set_ctx(ws_conn, NULL);
  };

  // printf("on_disconnect fd=%d\n", ws_conn_fd(ws_conn));
}

void on_drain(ws_conn_t *ws_conn) { printf("on_drain\n"); }

void on_server_err(ws_server_t *s, int err) {
  fprintf(stderr, "on_server_err: %s\n", strerror(err));
}

void on_accept_err(ws_server_t *s, int err) {
  printf("accept4(): %s\n", strerror(err));
  printf("open_conns = %zu \n", ws_server_open_conns(s));
  printf("is_paused=%d\n", ws_server_accept_paused(s));
}

void *start_server() {
  const uint16_t port = 9919;
  const int backlog = 1024;
  struct ws_server_params sp = {
      .addr = "::1",
      .port = port,
      .on_ws_open = on_open,
      .on_ws_msg = on_msg,
      .on_ws_disconnect = on_disconnect,
      .max_buffered_bytes = 512,
      .on_ws_accept_err = on_accept_err,
      .max_conns = 1000,
          // .on_ws_msg_fragment = on_msg_fragment,
  };

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

#include "ws.h"
#include <stdio.h>
#include <sys/signal.h>
#include <time.h>

#define MAX_CONNS 1024

#define MS *1000000
#define US *1000

void onOpen(ws_conn_t *conn) {}

void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  ws_conn_put_msg(conn, msg, n, opcode, 0);
}

struct timer_metrics {
  uint64_t counter;
  clock_t start;
};

void on_timeout(ws_server_t *s, void *ctx) {
  struct timer_metrics *metrics = (struct timer_metrics *)ctx;
  metrics->counter++;

  if (metrics->counter < 1000) {
    struct timespec timeout = {
        .tv_sec = 0,
        .tv_nsec = 1 MS,
    };

    ws_server_set_timeout(s, &timeout, metrics, on_timeout);
    // printf("ret: %d\n", ret);
  } else {
    struct timespec t = {
        .tv_sec = 0,
        .tv_nsec = 0,
    };

    clock_gettime(CLOCK_BOOTTIME, &t);
    uint64_t end = t.tv_nsec + (t.tv_sec * 1000000000);


    printf("time taken: %zu ms callback called %zu times\n", (end - metrics->start )/ 1000000, metrics->counter);
    free(metrics);
  }
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

  struct timespec timeout = {
      .tv_sec = 0,
      .tv_nsec = 1 MS,
  };

  struct timer_metrics *metrics = malloc(sizeof(struct timer_metrics));
  metrics->counter = 0;

  struct timespec start = {
      .tv_sec = 0,
      .tv_nsec = 0,
  };

  clock_gettime(CLOCK_BOOTTIME, &start);
  metrics->start = start.tv_nsec + (start.tv_sec * 1000000000);
  ws_server_set_timeout(s, &timeout, metrics, on_timeout);

  ws_server_start(s, 1024);

  return 0;
}

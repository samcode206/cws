#include "ws.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/signal.h>
#include <time.h>

#define MAX_CONNS 1024

#define MS *1000000
#define US *1000

void onOpen(ws_conn_t *conn) {}

void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  ws_conn_put_msg(conn, msg, n, opcode, 0);
}

uint64_t get_ns_time() {
  struct timespec now = {
      .tv_sec = 0,
      .tv_nsec = 0,
  };

  clock_gettime(CLOCK_BOOTTIME, &now);
  uint64_t n = now.tv_nsec + (now.tv_sec * 1000000000);
  assert(n != 0);
  return n;
}

uint64_t gid = 0;
uint64_t pending_timeouts = 0;

struct timer_metrics {
  uint64_t expected_timeout;
  uint64_t scheduled_at;
  uint64_t id;
  uint64_t timeout_ms;
};


static inline uint64_t get_random_duration(){
  return (rand() % 10) + 1;
}

struct timer_metrics *new_timer_metrics(uint64_t t) {
  struct timer_metrics *metrics = malloc(sizeof(struct timer_metrics));
  metrics->scheduled_at = get_ns_time();
  metrics->expected_timeout = metrics->scheduled_at + t;
  metrics->id = gid++;
  metrics->timeout_ms = t / 1000000;

  return metrics;
}




void timer_metrics_free(void *ctx) {
  struct timer_metrics *metrics = (struct timer_metrics *)ctx;
  free(metrics);
}

void set_metric_timer(ws_server_t *s, uint64_t d, timeout_cb_t cb) {

  struct timespec timeout = {
      .tv_sec = d > 1000 ? d / 1000 : 0,
      .tv_nsec = d > 1000 ? ((d  % 1000) MS): (d) MS,
  };
  struct timer_metrics *metrics = new_timer_metrics((d) MS);
  pending_timeouts++;
  int ret = ws_server_set_timeout(s, &timeout, metrics, cb);
  printf("[TIMER_INFO] SCHED id: %zu scheduled: %zu expires: %zu duration: %zu ms now %zu\n", metrics->id,
         metrics->scheduled_at, metrics->expected_timeout, metrics->timeout_ms, metrics->scheduled_at);
}

void on_timeout(ws_server_t *s, void *ctx) {
  struct timer_metrics *m = (struct timer_metrics *)ctx;
  uint64_t now = get_ns_time();
  uint64_t diff = now - m->expected_timeout;
  pending_timeouts--;

  assert(now > m->expected_timeout);


  printf("[TIMER_INFO] EXPIR id: %zu scheduled: %zu expires: %zu late: %zuus duration: %zu ms now: %zu\n", m->id,
         m->scheduled_at, m->expected_timeout, (now - m->expected_timeout) / 1000, m->timeout_ms, now);


  timer_metrics_free(ctx);

  if (gid < 1000) {
    set_metric_timer(s, get_random_duration(), on_timeout);

  }

  if (pending_timeouts == 0) {
    printf("done\n");
    exit(EXIT_SUCCESS);
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

  set_metric_timer(s, 1, on_timeout);

  set_metric_timer(s, 999, on_timeout);
  ws_server_start(s, 1024);

  return 0;
}

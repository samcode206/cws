#include "../../src/ws.c"

#include "../test.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

static void onOpen(ws_conn_t *conn) {}

static void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {}

static void onDisconnect(ws_conn_t *conn, unsigned long err) {}

static struct ws_server_params p = {
    .addr = "::1",
    .port = 9919,
    .on_ws_open = onOpen,
    .on_ws_msg = onMsg,
    .on_ws_disconnect = onDisconnect,
    .max_buffered_bytes = 64,
    .max_conns = 1,
    .silent = 1,
};

// *************** TEST SETUP ***************

#define MS *1000000

#define TEST_ITERATIONS 1000

static uint64_t test_id = 0;
static uint64_t pending_timeouts = 0;

static uint64_t get_ns_time() {
  struct timespec now = {
      .tv_sec = 0,
      .tv_nsec = 0,
  };

  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t n = now.tv_nsec + (now.tv_sec * 1000000000);
  assert(n != 0);
  return n;
}

struct timer_metrics {
  uint64_t expected_timeout;
  uint64_t scheduled_at;
  uint64_t id;
  uint64_t timeout_ms;
};

static inline uint64_t get_random_duration() { return (rand() % 10) + 1; }

static struct timer_metrics *new_timer_metrics(uint64_t t) {
  struct timer_metrics *metrics = malloc(sizeof(struct timer_metrics));
  metrics->scheduled_at = get_ns_time();
  metrics->expected_timeout = metrics->scheduled_at + t;
  metrics->id = test_id++;
  metrics->timeout_ms = t / 1000000;

  return metrics;
}

static void timer_metrics_free(void *ctx) {
  struct timer_metrics *metrics = (struct timer_metrics *)ctx;
  free(metrics);
}

static void set_metric_timer(ws_server_t *s, uint64_t d, timeout_cb_t cb) {

  struct timespec timeout = {
      .tv_sec = d > 1000 ? d / 1000 : 0,
      .tv_nsec = d > 1000 ? ((d % 1000) MS) : (d)MS,
  };
  struct timer_metrics *metrics = new_timer_metrics((d)MS);
  pending_timeouts++;
  ws_server_set_timeout(s, &timeout, metrics, cb);
  printf("[TIMER_INFO] SCHED id: %zu scheduled: %zu expires: %zu duration: %zu "
         "ms now %zu\n",
         metrics->id, metrics->scheduled_at, metrics->expected_timeout,
         metrics->timeout_ms, metrics->scheduled_at);
}

static void TEST_TIMERS_CONSEQ_RANDOM_ON_TIMEOUT(ws_server_t *s, void *ctx) {
  struct timer_metrics *m = (struct timer_metrics *)ctx;
  uint64_t now = get_ns_time();

  pending_timeouts--;

  assert(now > m->expected_timeout);

  printf("[TIMER_INFO] EXPIR id: %zu scheduled: %zu expires: %zu late: %zuus "
         "duration: %zu ms now: %zu\n",
         m->id, m->scheduled_at, m->expected_timeout,
         (now - m->expected_timeout) / 1000, m->timeout_ms, now);

  timer_metrics_free(ctx);

  if (test_id < TEST_ITERATIONS) {
    set_metric_timer(s, get_random_duration(),
                     TEST_TIMERS_CONSEQ_RANDOM_ON_TIMEOUT);
  }

  if (pending_timeouts == 0) {
    printf("done\n");
    ws_server_shutdown(s);
  }
}

// *************** TESTS ***************

static int TEST_TIMERS_CONSEQ_RANDOM(const char *name) {

  ws_server_t *s = ws_server_create(&p);

  set_metric_timer(s, 1, TEST_TIMERS_CONSEQ_RANDOM_ON_TIMEOUT);

  set_metric_timer(s, 999, TEST_TIMERS_CONSEQ_RANDOM_ON_TIMEOUT);
  int ret = ws_server_start(s, 1024);
  if (ret != 0) {
    return EXIT_FAILURE;
  }

  ws_server_destroy(s);

  return EXIT_SUCCESS;
}

void TEST_TIMERS_CANCELLATION_ON_TIMEOUT(ws_server_t *s, void *ctx) {
  if (ctx == (void *)1) {
    printf("ontimeout: Only Got the correct uncancelled callback\n");
    ws_server_shutdown(s);
    return;
  } else {
    fprintf(stderr, "Cancellation Failed\n");
    exit(EXIT_FAILURE);
  }
}

static int TEST_TIMERS_CANCELLATION(const char *name) {
  ws_server_t *s = ws_server_create(&p);

  struct timespec timeout1 = {.tv_sec = 3, .tv_nsec = 50000000}; // 50ms

  uint64_t tid1 = ws_server_set_timeout(s, &timeout1, NULL,
                                        TEST_TIMERS_CANCELLATION_ON_TIMEOUT);

  struct timespec timeout2 = {.tv_sec = 10, .tv_nsec = 500000000}; // 500ms

  uint64_t tid2 = ws_server_set_timeout(s, &timeout2, NULL,
                                        TEST_TIMERS_CANCELLATION_ON_TIMEOUT);

  ws_server_cancel_timeout(s, tid1);
  ws_server_cancel_timeout(s, tid2);

  ws_server_cancel_timeout(s, 321412412); // invalid tid

  struct timespec timeout3 = {
      .tv_sec = 2, .tv_nsec = 0}; // 2 second (this one ends the test)

  ws_server_set_timeout(s, &timeout3, (void *)1,
                        TEST_TIMERS_CANCELLATION_ON_TIMEOUT);

  ws_server_start(s, 1024);
  return EXIT_SUCCESS;
}

void TEST_BATCH_OF_TIMERS_SCHEDULED_TOGETHER_ON_TIMEOUT(ws_server_t *s,
                                                        void *ctx) {
  struct timer_metrics *m = (struct timer_metrics *)ctx;
  uint64_t now = get_ns_time();
  uint64_t diff = now - m->expected_timeout;
  pending_timeouts--;

  assert(now > m->expected_timeout);

  printf("[TIMER_INFO] EXPIR id: %zu scheduled: %zu expires: %zu late: %zuus "
         "duration: %zu ms now: %zu\n",
         m->id, m->scheduled_at, m->expected_timeout,
         (now - m->expected_timeout) / 1000, m->timeout_ms, now);

  timer_metrics_free(ctx);

  if (pending_timeouts == 0) {
    printf("done\n");
    ws_server_shutdown(s);
  }
}

static int TEST_BATCH_OF_TIMERS_SCHEDULED_TOGETHER_1MS_APART(const char *name) {

  ws_server_t *s = ws_server_create(&p);

  set_metric_timer(
      s, 3000,
      TEST_BATCH_OF_TIMERS_SCHEDULED_TOGETHER_ON_TIMEOUT); // one
                                                           // longer
                                                           // runnning
                                                           // timer

  // timers scheduled together spaced 1ms apart
  for (size_t i = 1; i < 1000; i++) {
    set_metric_timer(s, i, TEST_BATCH_OF_TIMERS_SCHEDULED_TOGETHER_ON_TIMEOUT);
  }

  ws_server_start(s, 1024);
  ws_server_destroy(s);

  return EXIT_SUCCESS;
}

static int TEST_BATCH_OF_TIMERS_SCHEDULED_TOGETHER(const char *name) {

  ws_server_t *s = ws_server_create(&p);

  set_metric_timer(
      s, 3000,
      TEST_BATCH_OF_TIMERS_SCHEDULED_TOGETHER_ON_TIMEOUT); // one
                                                           // longer
                                                           // runnning
                                                           // timer

  // all timers expire in 100 ms
  for (size_t i = 1; i < 1000; i++) {
    set_metric_timer(s, 100,
                     TEST_BATCH_OF_TIMERS_SCHEDULED_TOGETHER_ON_TIMEOUT);
  }

  ws_server_start(s, 1024);
  ws_server_destroy(s);

  return EXIT_SUCCESS;
}

#define NUM_TESTS 4

struct test_table CONN_POOL_TESTSUITE[NUM_TESTS] = {
    {"random consecutive timeouts", TEST_TIMERS_CONSEQ_RANDOM},
    {"Time Cancellation", TEST_TIMERS_CANCELLATION},
    {"Batch of timers scheduled together 1ms apart",
     TEST_BATCH_OF_TIMERS_SCHEDULED_TOGETHER_1MS_APART},
    {"Batch of timers scheduled together",
     TEST_BATCH_OF_TIMERS_SCHEDULED_TOGETHER},
};

int main(void) { RUN_TESTS("timers", CONN_POOL_TESTSUITE, NUM_TESTS); }

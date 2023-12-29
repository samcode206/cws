#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

typedef struct ws_timeout_t ws_timeout_t;
typedef void (*timeout_cb_t)(ws_timeout_t *ctx);

typedef struct ws_timeout_t {
  uint64_t id;
  time_t timeout_ms;
  void *ctx;
  timeout_cb_t cb;
} ws_timeout_t;

typedef ws_timeout_t *timer_queue;

#define P
#define T timer_queue
#include "pqu.h"

static int timer_queue_cmp(timer_queue *a, timer_queue *b) {
  return (*a)->timeout_ms < (*b)->timeout_ms;
}

struct timer_queue {
  uint64_t next_id;        // next timer id when adding a new timer
  int timer_fd;            // timer fd for epoll
  pqu_timer_queue *pqu_tq; // min heap of soonest expiring timers
};

static struct timer_queue *timer_queue_init(struct timer_queue *tq) {
  pqu_timer_queue timer_queue = pqu_timer_queue_init(timer_queue_cmp);
  pqu_timer_queue *p = calloc(1, sizeof(timer_queue));
  memcpy(p, &timer_queue, sizeof(timer_queue));
  tq->pqu_tq = p;

  tq->next_id = 0;

  tq->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (tq->timer_fd == -1) {
    pqu_timer_queue_free(tq->pqu_tq);
    free(p);
    tq->pqu_tq = NULL;
    perror("timerfd_create");
    exit(EXIT_FAILURE);
  }

  return tq;
}

static void timer_queue_destroy(struct timer_queue *tq) {
 
 while (!pqu_timer_queue_empty(tq->pqu_tq)) {
    ws_timeout_t **p = pqu_timer_queue_top(tq->pqu_tq);
    ws_timeout_t *t = *p;
    pqu_timer_queue_pop(tq->pqu_tq);
    free(t);
  }
 

  pqu_timer_queue_free(tq->pqu_tq);
  free(tq->pqu_tq);
  close(tq->timer_fd);
  memset(tq, 0, sizeof(*tq));
}

static time_t timer_queue_get_soonest_expiration(struct timer_queue *tq) {
  if (!pqu_timer_queue_empty(tq->pqu_tq)) {
    ws_timeout_t **p = pqu_timer_queue_top(tq->pqu_tq);
    return (*p)->timeout_ms;
  } else {
    return 0;
  }
}

static void timer_queue_set_soonest_expiration(struct timer_queue *tq,
                                               time_t soonest) {

  if (soonest > 0) {
    printf("now soonest %zu\n", soonest);
    struct itimerspec utmr = {.it_value =
                                  {
                                      0,
                                      soonest * 1000000,
                                  },
                              .it_interval = {
                                  0,
                                  0,
                              }};

    int _ = timerfd_settime(tq->timer_fd, 0, &utmr, NULL);
    assert(_ == 0);
    (void)_;
  }
}

static void timer_queue_run_expired_callbacks(struct timer_queue *tq,
                                              time_t now_ms) {
  time_t soonest = timer_queue_get_soonest_expiration(tq);

  while (!pqu_timer_queue_empty(tq->pqu_tq)) {
    ws_timeout_t **p = pqu_timer_queue_top(tq->pqu_tq);
    if ((*p)->timeout_ms > now_ms) {
      ws_timeout_t *t = *p;
      pqu_timer_queue_pop(tq->pqu_tq);
      t->cb(t);
      free(t);
    } else {
      break;
    }
  }

  // update timer with new soonest expiration
  time_t new_soonest = timer_queue_get_soonest_expiration(tq);
  if (soonest != new_soonest) {
    timer_queue_set_soonest_expiration(tq, soonest);
  }
}

static uint64_t timer_queue_add(struct timer_queue *tq, ws_timeout_t *t) {
  if (t->timeout_ms == 0) {
    // zero does nothing
    return 0;
  }

  // get soonest expiration
  time_t soonest = timer_queue_get_soonest_expiration(tq);

  uint64_t id = tq->next_id++;
  t->id = id;
  pqu_timer_queue_push(tq->pqu_tq, t);

  if ((soonest == 0) | (t->timeout_ms < soonest)) {
    timer_queue_set_soonest_expiration(tq, t->timeout_ms);
  }

  return id;
}

static void on_timeout(ws_timeout_t *ctx) {
  printf("timer id %zu expired\n", ctx->id);
}

int main(void) {
  struct timer_queue tq;
  timer_queue_init(&tq);

  for (long i = 1; i < 100; i++) {
    ws_timeout_t *t = malloc(sizeof(ws_timeout_t));
    t->timeout_ms = i;
    t->cb = on_timeout;

    timer_queue_add(&tq, t);
  }

  time_t soonest = timer_queue_get_soonest_expiration(&tq);

  printf("soonest: %lu\n", soonest);

  timer_queue_run_expired_callbacks(&tq, 50);

  soonest = timer_queue_get_soonest_expiration(&tq);
  printf("soonest: %lu\n", soonest);

  timer_queue_destroy(&tq);
}

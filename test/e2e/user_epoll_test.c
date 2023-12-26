#include "../../src/ws.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#define MAX_CONNS 1024

int myTimerFdPollCBMaxCalls = 5;

void myTimerFdPollCB(ws_server_t *s, ws_poll_cb_ctx_t *ctx, unsigned int ev) {
  myTimerFdPollCBMaxCalls--;

  int *tfd = ctx->ctx;

  printf("myTimerFdPollCB calls left: %d\n", myTimerFdPollCBMaxCalls);

  if (!myTimerFdPollCBMaxCalls) {
    printf("stopping interval\n");
    close(*tfd);
    ws_epoll_ctl_del(s, *tfd);
    free(tfd);
    free(ctx);
    printf("PASS\n");
    exit(EXIT_SUCCESS);
  } else {
    // read from the timerfd to drain it
    // otherwise it will keep triggering
    uint64_t _;
    assert(read(*tfd, &_, 8) == 8);
    (void)_;
  }
}

void createPollableIntervalTimer(ws_server_t *s) {
  int *tfd = malloc(sizeof(int));
  assert(tfd != NULL);
  *tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

  ws_poll_cb_ctx_t *ctx = calloc(1, sizeof(ws_poll_cb_ctx_t));
  assert(ctx != NULL);

  ctx->cb = myTimerFdPollCB;
  ctx->ctx = tfd;

  struct itimerspec timer = {.it_interval =
                                 {
                                     .tv_nsec = 100000,
                                     .tv_sec = 0,
                                 },
                             .it_value = {
                                 .tv_nsec = 100000,
                                 .tv_sec = 0,
                             }};

  assert(timerfd_settime(*tfd, 0, &timer, NULL) != -1);

  ws_epoll_ctl_add(s, *tfd, ctx, EPOLLIN);
}

void onOpen(ws_conn_t *conn) {}

void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {}

void onDisconnect(ws_conn_t *conn, int err) {}

int main(void) {
  printf("interval example starting on 9919\n");

  struct ws_server_params p = {
      .addr = "::1",
      .port = 9919,
      .on_ws_open = onOpen,
      .on_ws_msg = onMsg,
      .on_ws_disconnect = onDisconnect,
      .max_buffered_bytes = 1024 * 1024,
      .max_conns = MAX_CONNS,
  };

  ws_server_t *s = ws_server_create(&p);

  ws_epoll_create1(s); // register user's epoll

  createPollableIntervalTimer(
      s); // create a pollable fd (this case it's timer fd)

  ws_server_start(s, 1024);
  return 0;
}

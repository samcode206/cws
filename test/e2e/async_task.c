#include "../../src/ws.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(__linux__)
#include <sys/eventfd.h>
#else
#include <sys/event.h>
#endif

#define PORT 9919
#define ADDR "::1"

ws_server_t *srv;

_Atomic unsigned long done = 0;

void server_on_open(ws_conn_t *conn) {}

void server_on_msg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  // printf("msg %zu\n", n);
  ws_conn_send_msg(conn, msg, n, OP_BIN, 0);
}

void server_on_disconnect(ws_conn_t *conn, unsigned long err) {
  printf("%s\n", ws_conn_strerror(conn));
}

void *server_init(void *_) {
  struct ws_server_params p = {
      .addr = ADDR,
      .port = PORT,
      .on_ws_open = server_on_open,
      .on_ws_msg = server_on_msg,
      .on_ws_disconnect = server_on_disconnect,
      .max_buffered_bytes = 512,
      .max_conns = 2,
      .silent = 1,
  };

  srv = ws_server_create(&p);

  int ret = ws_server_start(srv, 1024);

  printf("ws_server_start = %s\n", !ret ? "ok" : "failed");

  ret = ws_server_destroy(srv);
  printf("ws_server_destroy = %s\n", !ret ? "ok" : "failed");

  return NULL;
}

void on_tasks_done(ws_server_t *s, void *ctx) {
  int *chanid = ctx;

  printf("id: #%d done\n", *(int *)ctx);
#if defined(__linux__)
  uint64_t v = 1;
  assert(write(*chanid, &v, 8) == 8);
#else
  struct kevent ev;
  // trigger
  EV_SET(&ev, *chanid, EVFILT_USER, EV_ENABLE, NOTE_TRIGGER, 0, NULL);
  kevent(*chanid, &ev, 1, NULL, 0, NULL);
#endif
}

struct timespec rand_duration(long atleast, long mrand) {
  struct timespec ts = {
      .tv_nsec = atleast + (rand() % 10 * mrand),
      .tv_sec = 0,
  };
  return ts;
}

void server_async_task3(ws_server_t *rs, void *ctx) {
  struct timespec ts = rand_duration(25000000, 50000000);

  // schedule a timeout that ends this thread's work
  printf("id: #%d task: #%d set_timeout: %zu\n", *(int *)ctx, 3,
         (size_t)ws_server_set_timeout(rs, &ts, ctx, on_tasks_done));
}

void server_async_task2(ws_server_t *rs, void *ctx) {
  ws_server_sched_callback(rs, server_async_task3, ctx);
}

void server_async_task(ws_server_t *rs, void *ctx) {
  ws_server_sched_callback(rs, server_async_task2, ctx);
}

void *test_init(void *_) {
  int *evfd = calloc(1, sizeof(int));

#if defined(__linux__)
  // will use a blocking eventfd to know when all tasks are run
  *evfd = eventfd(0, 0);
#else
  *evfd = kqueue();
#endif

  ws_server_sched_callback(srv, server_async_task, evfd);

  uint64_t val;
  // once read is done we know we are done because write to eventfd happens in
  // the final task

#if defined(__linux__)
  assert(read(*evfd, &val, 8) == 8);
#else
  struct kevent ev;
  // add
  EV_SET(&ev, *evfd, EVFILT_USER, EV_ADD, 0, 0, NULL);
  kevent(*evfd, &ev, 1, NULL, 0, NULL);

  // wait
  kevent(*evfd, NULL, 0, &ev, 1, NULL);
#endif

  done++;
  printf("id: #%d scheduled And Ran All tasks freeing: %p\n", *evfd, evfd);
  close(*evfd);
  free(evfd);
  return NULL;
}

int main() {
  printf("async task test starting...\n");
  pthread_t server_w;

  if (pthread_create(&server_w, NULL, server_init, NULL) == -1) {
    perror("pthread_create");
    exit(EXIT_FAILURE);
  };

  sleep(2);

#define NUM_TEST_THREADS 32
  pthread_t client_threads[NUM_TEST_THREADS];

  for (size_t i = 0; i < NUM_TEST_THREADS; i++) {
    if (pthread_create(&client_threads[i], NULL, test_init, (void *)(long)i) ==
        -1) {
      perror("pthread_create");
      exit(EXIT_FAILURE);
    }
  }

  printf("waiting for tasks to finish...\n");
  // wait for the tests to complete
  for (size_t i = 0; i < NUM_TEST_THREADS; i++) {
    pthread_join(client_threads[i], NULL);
  }

  printf("shutting down...\n");
  int ret = ws_server_shutdown(srv);
  printf("ws_server_shutdown task complete: %s\n", !ret ? "true" : "false");
  printf("ws_server_shuttin_down: %s\n",
         ws_server_shutting_down(srv) ? "true" : "false");
  pthread_join(server_w, NULL);

  printf("tasks done:  %zu/%zu\n", done, (unsigned long)NUM_TEST_THREADS);

  if (done == NUM_TEST_THREADS) {
    exit(EXIT_SUCCESS);
  } else {
    exit(EXIT_FAILURE);
  }
}

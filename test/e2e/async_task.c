#include "../../src/ws.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/eventfd.h>

#define PORT 9919
#define ADDR "::1"

ws_server_t *srv;


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
  };

  srv = ws_server_create(&p);

  ws_server_start(srv, 1024);


  return NULL;
}

void server_async_task4(ws_server_t *rs, void *ctx) {
  int *chanid = ctx;
  printf("Final Task 4 running for %d\n", *chanid);
  uint64_t v = 1;
  assert(write(*chanid, &v, 8) == 8);
}

void server_async_task3(ws_server_t *rs, void *ctx) {
  int *chanid = ctx;
  printf("Task 4 running for %d\n", *chanid);
  assert(srv == rs);

  ws_server_sched_callback(rs, server_async_task4, ctx);
}

void server_async_task2(ws_server_t *rs, void *ctx) {
  int *chanid = ctx;
  printf("Task 2 running for %d\n", *chanid);
  assert(srv == rs);

  ws_server_sched_callback(rs, server_async_task3, ctx);
}

void server_async_task(ws_server_t *rs, void *ctx) {
  int *chanid = ctx;
  printf("Task 1 running for %d\n", *chanid);
  assert(srv == rs);

  ws_server_sched_callback(rs, server_async_task2, ctx);
}



void *test_init(void *_) {

  // will use a blocking eventfd to know when all tasks are run
  int evfd = eventfd(0, 0);

  ws_server_sched_callback(srv, server_async_task, &evfd);

  uint64_t val;
  // once read is done we know we are done because write to eventfd happens in
  // the final task
  assert(read(evfd, &val, 8) == 8);
  printf("thread %d scheduled And Ran All tasks\n", gettid());


  return NULL;
}

int main() {
  pthread_t server_w;

  if (pthread_create(&server_w, NULL, server_init, NULL) == -1) {
    perror("pthread_create");
    exit(EXIT_FAILURE);
  };

  sleep(1);

#define NUM_TEST_THREADS 128
  pthread_t client_threads[NUM_TEST_THREADS];

  for (size_t i = 0; i < NUM_TEST_THREADS; i++) {
    if (pthread_create(&client_threads[i], NULL, test_init, (void *)(long)i) ==
        -1) {
      perror("pthread_create");
      exit(EXIT_FAILURE);
    }
  }

  // wait for the tests to complete
  for (size_t i = 0; i < NUM_TEST_THREADS; i++) {
    pthread_join(client_threads[i], NULL);
  }




  ws_server_destroy(srv);

}

#include "ws.h"
#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signal.h>
#include <sys/timerfd.h>

#define MAX_CONNS 4000
#define NUM_SERVERS 4

size_t total = 0;

void onOpen(ws_conn_t *conn) {}


void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  if (ws_conn_msg_ready(conn)) {
    ws_conn_put_msg(conn, msg, n, opcode, 0);
  } else {
    ws_conn_send_msg(conn, msg, n, opcode, 0);
  }
}

void onDisconnect(ws_conn_t *conn, unsigned long err) {}

void *server_init(void *s) {
  printf("broadcast example starting on 9919\n");

  ws_server_start(s, 1024);

  return NULL;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);

  ws_server_t *servers[NUM_SERVERS] = {0};

  for (size_t i = 0; i < NUM_SERVERS; i++) {
    struct ws_server_params p = {
        .addr = "::1",
        .port = 9919,
        .on_ws_open = onOpen,
        .on_ws_msg = onMsg,
        .on_ws_disconnect = onDisconnect,
        .max_buffered_bytes = 1024 * 512,
        .max_conns = MAX_CONNS / NUM_SERVERS,
        .silent = 0,
    };

    servers[i] = ws_server_create(&p);
  }

  pthread_t server_threads[NUM_SERVERS];

  for (size_t i = 0; i < NUM_SERVERS; i++) {
    if (pthread_create(&server_threads[i], NULL, server_init, servers[i]) ==
        -1) {
      perror("pthread_create");
      exit(EXIT_FAILURE);
    }
  }

  for (size_t i = 0; i < NUM_SERVERS; i++) {
    pthread_join(server_threads[i], NULL);
  }

  return 0;
}

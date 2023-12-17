#include "sock_util.h"
#include "ws.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define PORT 9919
#define ADDR "::1"

void server_on_open(ws_conn_t *conn) {}

void server_on_msg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  // printf("msg %zu\n", n);
  ws_conn_send_msg(conn, msg, n, OP_BIN, 0);
}

void server_on_disconnect(ws_conn_t *conn, int err) {
  printf("%s\n", ws_conn_strerror(conn));
}

void *server_init(void *_) {
  struct ws_server_params p = {
      .addr = ADDR,
      .port = PORT,
      .on_ws_open = server_on_open,
      .on_ws_msg = server_on_msg,
      .on_ws_disconnect = server_on_disconnect,
      .max_buffered_bytes = 2048,
      .max_conns = 2,
  };

  ws_server_t *s = ws_server_create(&p);

  ws_server_start(s, 1024);

  return NULL;
}

void do_handshake_test() {

  int fd = sock_new_connect(PORT, ADDR);

  ssize_t sent = sock_sendall(fd, EXAMPLE_REQUEST, sizeof EXAMPLE_REQUEST - 1);
  if (sent != sizeof EXAMPLE_REQUEST - 1) {
    fprintf(stderr, "failed to send upgrade request\n");
    exit(EXIT_FAILURE);
  }

  char buf[4096] = {0};

  ssize_t read = sock_recv(fd, buf, 4096);
  if (read == 0) {
    fprintf(stderr, "connection dropped before receiving upgrade response\n");
    exit(EXIT_FAILURE);
  } else if (read == -1) {
    perror("recv");
    exit(EXIT_FAILURE);
  }

  if (strstr(buf, EXAMPLE_REQUEST_EXPECTED_ACCEPT_KEY) != NULL) {
    printf("PASS");
  } else {
    fprintf(stderr, "unexpected response\n");
  }

  printf("-------------------------------\n");
  printf("%s\n", buf);
  printf("-------------------------------\n");
}

int main(void) {
  pthread_t server_w;

  if (pthread_create(&server_w, NULL, server_init, NULL) == -1) {
    perror("pthread_create");
    exit(EXIT_FAILURE);
  };

  sleep(1);
  signal(SIGPIPE, SIG_IGN);
  do_handshake_test();
  return EXIT_SUCCESS;
}

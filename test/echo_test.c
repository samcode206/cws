#include "ws.h"
#include "sock_util.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define PORT 9919
#define ADDR "::1"

void server_on_open(ws_conn_t *conn) {}

void server_on_msg(ws_conn_t *conn, void *msg, size_t n, bool bin) {
  // printf("msg %zu\n", n);
  ws_conn_send(conn, msg, n, 0);
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

  int stat;
  ws_server_t *s = ws_server_create(&p, &stat);

  ws_server_start(s, 1024);

  return NULL;
}



int main(void) {
  pthread_t server_w;

  if (pthread_create(&server_w, NULL, server_init, NULL) == -1) {
    perror("pthread_create");
    exit(EXIT_FAILURE);
  };

  sleep(1);

  int fd = sock_new_connect(PORT, ADDR);
  sock_upgrade_ws(fd);

  int runs = 300;

  char out_buf[256];
  char in_buf[256];

  unsigned frame_cfg = OP_BIN | 0x80;

  while (runs--) {
    if (runs < 200 && runs > 100) {
      frame_cfg = OP_TXT | 0x80;
    } else if (runs < 100) {
      frame_cfg = OP_PING | 0x80;
    }

    int msg_len = sprintf(out_buf, "count %d", runs);
    unsigned char *frame = new_frame(out_buf, msg_len, frame_cfg);
    ssize_t sent = sock_sendall(fd, frame, msg_len + 6);
    assert(sent == msg_len + 6);
    ssize_t read = sock_recvall(fd, in_buf, msg_len + 2);
    assert(read == msg_len + 2);

    if (memcmp(in_buf + 2, out_buf, msg_len) != 0) {
      fprintf(stderr, "mismatched data received expected: %s got %.*s\n",
              out_buf, msg_len, in_buf);
    }

    free(frame);
  }

  printf("PASS\n");
}

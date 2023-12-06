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

static const char msg[] =
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit.  "
    "Vestibulum elementum venenatis nibh, in accumsan justo  quis. ";

static const size_t msg_len = sizeof msg - 1;

void test1(int fd) {
  unsigned char *first_frame = new_frame(msg, 4, OP_TXT);
  sock_sendall(fd, first_frame, 10);

  for (size_t i = 4; i < msg_len - 4; i += 4) {
    unsigned char *frame = new_frame(msg + i, 4, 0);
    sock_sendall(fd, frame, 10);
  }

  unsigned char *last_frame = new_frame(msg + (msg_len - 4), 4, 0X80);
  sock_sendall(fd, last_frame, 10);

  char buf[512];

  ssize_t read = sock_recvall(fd, buf, 122);
  buf[read] = '\0';

  if (read == 122 && !strcmp(msg, buf + 2)) {
    printf("[SUCCESS] Received the fragmented message\n");
  } else {
    printf("[FAIL] fragmented message handling failure\n");
  }
}

void do_fragmented_msg_test1() {
  printf("do_fragmented_msg_test1.........\n");
  int fd = sock_new(1);
  sock_connect(fd, PORT, ADDR, 1);
  sock_upgrade_ws(fd);
  test1(fd);
}

void test2(int fd) {
  size_t nvecs = msg_len / 4;
  struct iovec *vecs = malloc(sizeof(struct iovec) * nvecs);
  size_t total_size = nvecs * 10;

  vecs[0].iov_len = 10;
  vecs[0].iov_base = new_frame(msg, 4, OP_TXT);
  size_t iov_idx = 1;
  for (size_t i = 4; i < msg_len - 4; i += 4) {
    vecs[iov_idx].iov_base = new_frame(msg + i, 4, 0);
    vecs[iov_idx].iov_len = 10;
    iov_idx++;
  }
  vecs[nvecs - 1].iov_len = 10;
  vecs[nvecs - 1].iov_base = new_frame(msg + (msg_len - 4), 4, 0X80);

  ssize_t n = writev(fd, vecs, nvecs);

  char buf[512];

  ssize_t read = sock_recvall(fd, buf, 122);
  buf[read] = '\0';

  if (read == 122 && !strcmp(msg, buf + 2)) {
    printf("[SUCCESS] Received the fragmented message\n");
  } else {
    printf("[FAIL] fragmented message handling failure\n");
  }
}

void do_fragmented_msg_test2() {
  printf("do_fragmented_msg_test2.........\n");
  int fd = sock_new(1);
  sock_connect(fd, PORT, ADDR, 1);
  sock_upgrade_ws(fd);
}

void test3(int fd) {
  size_t nvecs = (msg_len / 4) + 2;
  struct iovec *vecs = malloc(sizeof(struct iovec) * nvecs);
  size_t total_size = (nvecs - 2) * 10;

  vecs[0].iov_len = 10;
  vecs[0].iov_base = new_frame(msg, 4, OP_TXT);
  size_t iov_idx = 1;
  for (size_t i = 4; i < msg_len - 4; i += 4) {
    vecs[iov_idx].iov_base = new_frame(msg + i, 4, 0);
    vecs[iov_idx].iov_len = 10;
    iov_idx++;
  }
  vecs[iov_idx].iov_len = 10;
  vecs[iov_idx].iov_base = new_frame(msg + (msg_len - 4), 4, 0X80);

  vecs[nvecs - 2].iov_len = 126;
  vecs[nvecs - 2].iov_base = new_frame(msg, msg_len, 0x80 | OP_TXT);
  total_size += 126;

  vecs[nvecs - 1].iov_len = 126;
  vecs[nvecs - 1].iov_base = new_frame(msg, msg_len, 0x80 | OP_TXT);
  total_size += 126;

  ssize_t n = writev(fd, vecs, nvecs);
  printf("%zu %zu\n", n, total_size);

  char buf[512];
  for (size_t i = 0; i < 3; i++) {

    ssize_t read = sock_recvall(fd, buf, 122);
    buf[read] = '\0';

    if (read == 122 && !strcmp(msg, buf + 2)) {
      printf("[SUCCESS] Received the fragmented message %zu\n", i);
    } else {
      printf("[FAIL] fragmented message handling failure %zu\n", i);
    }
  }
}

void test4(int fd) {
  size_t nvecs = (msg_len / 4) + 2;
  struct iovec *vecs = malloc(sizeof(struct iovec) * nvecs);
  size_t total_size = (nvecs - 2) * 10;

  vecs[0].iov_len = 10;
  vecs[0].iov_base = new_frame(msg, 4, OP_TXT);
  size_t iov_idx = 1;
  for (size_t i = 4; i < msg_len - 4; i += 4) {
    vecs[iov_idx].iov_base = new_frame(msg + i, 4, 0);
    vecs[iov_idx].iov_len = 10;
    iov_idx++;
  }
  vecs[iov_idx].iov_len = 10;
  vecs[iov_idx].iov_base = new_frame(msg + (msg_len - 4), 4, 0X80);

  vecs[nvecs - 2].iov_len = 126;
  vecs[nvecs - 2].iov_base = new_frame(msg, msg_len, 0x80 | OP_TXT);
  total_size += 126;

  vecs[nvecs - 1].iov_len = 126;
  vecs[nvecs - 1].iov_base = new_frame(msg, msg_len, 0x80 | OP_TXT);
  total_size += 126;

  for (size_t i = 0; i < nvecs; i++) {
    for (size_t j = 0; j < vecs[i].iov_len; j++) {
      ssize_t n = send(fd, vecs[i].iov_base + j, 1, 0);
      assert(n == 1);
    }
  }

  char buf[512];
  for (size_t i = 0; i < 3; i++) {

    ssize_t read = sock_recvall(fd, buf, 122);
    buf[read] = '\0';

    if (read == 122 && !strcmp(msg, buf + 2)) {
      printf("[SUCCESS] Received the fragmented message %zu\n", i);
    } else {
      printf("[FAIL] fragmented message handling failure %zu\n", i);
    }
  }
}

void do_fragmented_msg_test3() {
  printf("do_fragmented_msg_test3.........\n");
  // one one write
  // send the same message 3 times first is fragmented then twice un fragmented
  // expect the message to be echoed back 3 times
  int fd = sock_new(1);
  sock_connect(fd, PORT, ADDR, 1);
  sock_upgrade_ws(fd);

  test3(fd);
}

void test5(int fd) {

  struct iovec vecs[] = {
      {
          .iov_base = new_frame("hello ", 6, OP_TXT),
          .iov_len = 12,
      },
      {
          .iov_base = new_frame("ping", 4, OP_PING | 0x80),
          .iov_len = 10,
      },
      {
          .iov_base = new_frame("World", 5, 0),
          .iov_len = 11,
      },
      {
          .iov_base = new_frame("ping", 4, OP_PING | 0x80),
          .iov_len = 10,
      },
      {
          .iov_base = new_frame(".", 1, 0X80),
          .iov_len = 7,
      },
      {
          .iov_base = new_frame("ping", 4, OP_PING | 0x80),
          .iov_len = 10,
      },
      {
          .iov_base = new_frame("How Are you?", 12, 0X80 | OP_TXT),
          .iov_len = 18,
      },
      {
          .iov_base = new_frame("ping", 4, OP_PING | 0x80),
          .iov_len = 10,
      },
  };

  writev(fd, vecs, 8);

  char buf[512];

  int read = sock_recvall(fd, buf, 6);
  assert(read == 6);
  assert(memcmp("ping", buf + 2, 4) == 0);

  read = sock_recvall(fd, buf, 6);
  assert(read == 6);
  assert(memcmp("ping", buf + 2, 4) == 0);

  read = sock_recvall(fd, buf, 14);
  assert(read == 14);

  assert(memcmp("hello World.", buf + 2, 12) == 0);

  read = sock_recvall(fd, buf, 6);
  assert(read == 6);
  assert(memcmp("ping", buf + 2, 4) == 0);

  read = sock_recvall(fd, buf, 14);
  assert(read == 14);

  assert(memcmp("How Are you?", buf + 2, 12) == 0);

  read = sock_recvall(fd, buf, 6);
  assert(read == 6);
  assert(memcmp("ping", buf + 2, 4) == 0);

  printf("[Success] Received all messages and interleaved pongs\n");
}

void test6(int fd) {

  struct iovec vecs[] = {
      {
          .iov_base = new_frame("hello ", 6, OP_TXT),
          .iov_len = 12,
      },
      {
          .iov_base = new_frame("ping", 4, OP_PING | 0x80),
          .iov_len = 10,
      },
      {
          .iov_base = new_frame("World", 5, 0),
          .iov_len = 11,
      },
      {
          .iov_base = new_frame("ping", 4, OP_PING | 0x80),
          .iov_len = 10,
      },
      {
          .iov_base = new_frame(".", 1, 0X80),
          .iov_len = 7,
      },
      {
          .iov_base = new_frame("How Are you?", 12, 0X80 | OP_TXT),
          .iov_len = 18,
      },
      {
          .iov_base = new_frame("ping", 4, OP_PING | 0x80),
          .iov_len = 10,
      },
  };

  for (size_t i = 0; i < 7; i++) {
    for (size_t j = 0; j < vecs[i].iov_len; j++) {
      assert(send(fd, vecs[i].iov_base + j, 1, 0) == 1);
    }
  }

  char buf[512];

  int read = sock_recvall(fd, buf, 6);
  assert(read == 6);
  assert(memcmp("ping", buf + 2, 4) == 0);

  read = sock_recvall(fd, buf, 6);
  assert(read == 6);
  assert(memcmp("ping", buf + 2, 4) == 0);

  read = sock_recvall(fd, buf, 14);
  assert(read == 14);

  assert(memcmp("hello World.", buf + 2, 12) == 0);

  read = sock_recvall(fd, buf, 14);
  assert(read == 14);

  assert(memcmp("How Are you?", buf + 2, 12) == 0);

  read = sock_recvall(fd, buf, 6);
  assert(read == 6);
  assert(memcmp("ping", buf + 2, 4) == 0);

  printf("[Success] Received all messages and interleaved pongs\n");
}

int main(void) {
  assert(!(msg_len % 4));
  pthread_t server_w;

  if (pthread_create(&server_w, NULL, server_init, NULL) == -1) {
    perror("pthread_create");
    exit(EXIT_FAILURE);
  };

  sleep(1);

  int fd = sock_new(1);
  sock_connect(fd, PORT, ADDR, 1);
  sock_upgrade_ws(fd);

  printf("test6....................\n");
  test6(fd);

  printf("test5....................\n");
  test5(fd);

  printf("test4....................\n");
  test4(fd);

  printf("test3....................\n");
  test3(fd);

  printf("test2....................\n");
  test2(fd);

  printf("test1....................\n");
  test1(fd);

  do_fragmented_msg_test1();
  do_fragmented_msg_test2();
  do_fragmented_msg_test3();
}

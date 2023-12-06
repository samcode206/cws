#include "sock_util.h"
#include <assert.h>
#include <stdio.h>

static const char msg[] =
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit.  "
    "Vestibulum elementum venenatis nibh, in accumsan justo  quis. ";

static const size_t msg_len = sizeof msg - 1;

unsigned char *new_frame(const char *src, size_t len, unsigned frame_cfg) {
  // only handle sending small frames
  if (len > 125) {
    return NULL;
  }
  // 2 byte header + 4 byte mask key
  unsigned char *dst = malloc(len + 6);
  if (!dst) {
    return NULL;
  }

  dst[0] = frame_cfg;
  dst[1] = (uint8_t)len;
  dst[1] |= 0x80; // masked frame

  unsigned char *mask = dst + 2;
  unsigned char *payload = dst + 6;

  for (unsigned i = 0; i < 4; ++i) {
    mask[i] = rand() % 256;
  }

  memcpy(dst + 6, src, len);

  for (size_t i = 0; i < len; ++i) {
    payload[i] ^= mask[i % 4];
  }

  return dst;
}

void do_fragmented_msg_test1() {
  printf("do_fragmented_msg_test1.........\n");
  int fd = sock_new(1);
  sock_connect(fd, 9919, "::1", 1);
  sock_upgrade_ws(fd);

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

void do_fragmented_msg_test2() {
  printf("do_fragmented_msg_test2.........\n");
  int fd = sock_new(1);
  sock_connect(fd, 9919, "::1", 1);
  sock_upgrade_ws(fd);

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




void do_fragmented_msg_test3() {
  // one one write
  // send the same message 3 times first is fragmented then twice un fragmented
  // expect the message to be echoed back 3 times
  int fd = sock_new(1);
  sock_connect(fd, 9919, "::1", 1);
  sock_upgrade_ws(fd);

  size_t nvecs = (msg_len / 4) + 2;
  struct iovec *vecs = malloc(sizeof(struct iovec) * nvecs);
  size_t total_size = (nvecs-2) * 10;

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

  vecs[nvecs-2].iov_len = 122;
  vecs[nvecs-2].iov_base = new_frame(msg, msg_len, 0x80 | OP_TXT);
  total_size += 122;

  vecs[nvecs-1].iov_len = 122;
  vecs[nvecs-1].iov_base = new_frame(msg, msg_len, 0x80 | OP_TXT);
  total_size += 122;

  ssize_t n = writev(fd, vecs, nvecs);
  printf("%zu %zu\n", n, total_size);

}


int main(void) {
  assert(!(msg_len % 4));
//   do_fragmented_msg_test1();
//   do_fragmented_msg_test2();
  do_fragmented_msg_test3();
}

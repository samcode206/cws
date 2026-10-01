// Regression tests for defects found in review:
//  1. a handshake header line without a colon must get a 400, not a crash
//  2. a frame whose header straddles a read boundary right after a fragmented
//     message must still be parsed (receive buffer compaction was skipped)
//  3. the IO timeout sweep must fire for the earliest deadline, not the last
//     connection's deadline
#include "../../src/ws.h"
#include "../wsockutil.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 9933
static size_t opened = 0;

static void onOpen(ws_conn_t *conn) {
  // first connection of the timeout test gets a long deadline and sits at
  // index 0 of pending_timers; the second gets a short one. The old sweep
  // rescheduled itself from index 0's deadline and missed the short one.
  opened++;
  ws_conn_set_read_timeout(conn, opened == 3 ? 2 : 50);
}

static void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  ws_conn_send_msg(conn, msg, n, opcode == OP_PING ? OP_PONG : opcode, 0);
}

static void onDisconnect(ws_conn_t *conn, unsigned long err) {
  (void)conn; (void)err;
}

static void *server_init(void *_) {
  (void)_;
  struct ws_server_params p = {
      .addr = "::1", .port = PORT, .on_ws_open = onOpen, .on_ws_msg = onMsg,
      .on_ws_disconnect = onDisconnect, .max_buffered_bytes = 4096, .max_conns = 8,
  };
  ws_server_t *s = ws_server_create(&p);
  assert(s != NULL);
  ws_server_start(s, 16);
  return NULL;
}

static int test_header_without_colon(void) {
  int fd = sock_new_connect(PORT, "::1");
  const char *req = "GET / HTTP/1.1\r\nHost: x\r\nThisLineHasNoColon\r\n\r\n";
  sock_sendall(fd, req, strlen(req));
  char buf[512] = {0};
  ssize_t n = sock_recv(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0 || strncmp(buf, "HTTP/1.1 400", 12) != 0) {
    fprintf(stderr, "[FAIL] expected 400 for header without colon, got %zd bytes: %.40s\n", n, buf);
    return -1;
  }
  printf("[PASS] header line without colon -> 400\n");
  return 0;
}

// build a masked frame; caller frees
static unsigned char *mk_frame(const char *src, size_t len, unsigned cfg, size_t *out) {
  size_t hl = len > 125 ? 4 : 2;
  unsigned char *f = malloc(hl + 4 + len);
  f[0] = (unsigned char)cfg;
  if (hl == 2) f[1] = 0x80 | (unsigned char)len;
  else { f[1] = 0x80 | 126; f[2] = (unsigned char)(len >> 8); f[3] = (unsigned char)len; }
  unsigned char mask[4] = {1, 2, 3, 4};
  memcpy(f + hl, mask, 4);
  for (size_t i = 0; i < len; i++) f[hl + 4 + i] = (unsigned char)src[i] ^ mask[i & 3];
  *out = hl + 4 + len;
  return f;
}

// read one unmasked server frame (text, len <= 65535) into out; returns len
static ssize_t read_echo(int fd, char *out, size_t cap) {
  unsigned char h[4];
  if (sock_recvall(fd, h, 2) != 2) return -1;
  size_t len = h[1] & 0x7f;
  if (len == 126) { if (sock_recvall(fd, h + 2, 2) != 2) return -1; len = ((size_t)h[2] << 8) | h[3]; }
  if (len > cap) return -1;
  if (sock_recvall(fd, out, len) != (ssize_t)len) return -1;
  return (ssize_t)len;
}

static int test_straddling_frame_after_fragments(void) {
  int fd = sock_new_connect(PORT, "::1");
  sock_upgrade_ws(fd);

  size_t l1, l2, l3;
  unsigned char *f1 = mk_frame("hello ", 6, OP_TXT, &l1);          // fin=0
  unsigned char *f2 = mk_frame("world", 5, 0x80 | 0x0, &l2);       // fin=1 cont
  char big[300]; memset(big, 'x', sizeof big);
  unsigned char *f3 = mk_frame(big, sizeof big, 0x80 | OP_TXT, &l3); // 4-byte header

  // one write: both fragments plus only the first 3 bytes of the next frame's
  // header, so the server sees an incomplete header with the socket drained
  unsigned char *w = malloc(l1 + l2 + 3);
  memcpy(w, f1, l1); memcpy(w + l1, f2, l2); memcpy(w + l1 + l2, f3, 3);
  sock_sendall(fd, w, l1 + l2 + 3);
  usleep(100 * 1000);
  sock_sendall(fd, f3 + 3, l3 - 3);

  char out[512]; int rc = 0;
  ssize_t n = read_echo(fd, out, sizeof out);
  if (n != 11 || memcmp(out, "hello world", 11) != 0) {
    fprintf(stderr, "[FAIL] fragmented message echo wrong (%zd)\n", n); rc = -1;
  }
  n = read_echo(fd, out, sizeof out);
  if (n != (ssize_t)sizeof big || memcmp(out, big, sizeof big) != 0) {
    fprintf(stderr, "[FAIL] frame after fragments not echoed correctly (%zd)\n", n); rc = -1;
  }
  if (rc == 0) printf("[PASS] straddling frame after fragmented message\n");
  free(f1); free(f2); free(f3); free(w);
  close(fd);
  return rc;
}

static int test_earliest_timeout_fires(void) {
  int a = sock_new_connect(PORT, "::1"); // opened 2nd overall -> 50 s, index 0
  sock_upgrade_ws(a);
  int b = sock_new_connect(PORT, "::1"); // opened 3rd -> 2 s, index 1
  sock_upgrade_ws(b);
  struct timeval tv = {.tv_sec = 8, .tv_usec = 0};
  setsockopt(b, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  char buf[16];
  ssize_t n;
  for (;;) {
    // the sweep sends a keepalive ping shortly before the deadline; skip
    // frames until EOF (server closed b) or the 8 s receive timeout
    n = recv(b, buf, sizeof buf, 0);
    if (n <= 0) break;
  }
  int rc = 0;
  if (n != 0) {
    fprintf(stderr, "[FAIL] 2s read timeout did not close the connection within 8s (recv=%zd)\n", n); rc = -1;
  } else {
    // the long-deadline connection must still be open: a ping gets a pong
    size_t pl; unsigned char *ping = mk_frame("", 0, 0x80 | OP_PING, &pl);
    sock_sendall(a, ping, pl); free(ping);
    unsigned char h[2];
    if (sock_recvall(a, h, 2) != 2 || (h[0] & 0x0f) != OP_PONG) {
      fprintf(stderr, "[FAIL] long-deadline connection was closed too\n"); rc = -1;
    }
  }
  if (rc == 0) printf("[PASS] earliest IO timeout fires on time\n");
  close(a); close(b);
  return rc;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  pthread_t t;
  pthread_create(&t, NULL, server_init, NULL);
  sleep(1);
  int rc = 0;
  rc |= test_header_without_colon();            // never upgraded, onOpen not called
  rc |= test_straddling_frame_after_fragments(); // opened #1 -> 50 s, closed again
  rc |= test_earliest_timeout_fires();           // opened #2 -> 50 s, #3 -> 2 s
  printf(rc == 0 ? "PASS regressions_test\n" : "FAIL regressions_test\n");
  exit(rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

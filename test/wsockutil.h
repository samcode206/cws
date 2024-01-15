
#ifndef WS_SOCK_UTIL_1235412X
#define WS_SOCK_UTIL_1235412X

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <signal.h>
// utils for socket testing

#define OP_TXT 0x1
#define OP_BIN 0x2
#define OP_CLOSE 0x8
#define OP_PING 0x9
#define OP_PONG 0xA

static int sock_new_connect(short port, char *addr) {
  struct sockaddr_in _;
  bool ipv6 = 0;
  if (inet_pton(AF_INET, addr, &_) != 1) {
    struct sockaddr_in6 _;
    if (inet_pton(AF_INET6, addr, &_) != 1) {
      fprintf(stderr, "invalid address %s\n", addr);
      exit(EXIT_FAILURE);
    } else {
      ipv6 = 1;
    }
  }

  int fd;

  if (ipv6) {
    fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (fd == -1) {
      perror("socket");
      exit(EXIT_FAILURE);
    }
  } else {
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == -1) {
      perror("socket");
      exit(EXIT_FAILURE);
    }
  }

  int on = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on) == -1) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  };

  if (ipv6) {
    struct sockaddr_in6 peerAddr = {0};
    peerAddr.sin6_family = AF_INET6;
    peerAddr.sin6_port = htons(port);
    inet_pton(AF_INET6, addr, &peerAddr.sin6_addr);

    if (connect(fd, (struct sockaddr *)&peerAddr, sizeof peerAddr) == -1) {
      perror("connect");
      exit(EXIT_FAILURE);
    };

  } else {
    struct sockaddr_in peerAddr = {0};
    peerAddr.sin_family = AF_INET;
    peerAddr.sin_port = htons(port);
    inet_pton(AF_INET, addr, &peerAddr.sin_addr);
    if (connect(fd, (struct sockaddr *)&peerAddr, sizeof peerAddr) == -1) {
      perror("connect");
      exit(EXIT_FAILURE);
    };
  }

  return fd;
}

static ssize_t sock_sendall(int fd, const void *data, size_t len) {
  size_t sent = 0;
  ssize_t n;

  while (sent < len) {
    n = send(fd, (char *)data + sent, len - sent, 0);
    if (n == -1) {
      if (n == EINTR) {
        continue;
      }
      return -1;
    } else if (n == 0) {
      return sent;
    } else {
      sent += n;
    }
  }

  return sent;
}

static ssize_t sock_recvall(int fd, void *data, size_t len) {
  size_t read = 0;
  ssize_t n;

  while (read < len) {
    n = recv(fd, (char *)data + read, len - read, MSG_WAITALL);
    if (n == -1) {
      if (n == EINTR) {
        continue;
      } else {
        return -1;
      }
    } else if (n == 0) {
      return read;
    } else {
      read += n;
    }
  }

  return read;
}

static ssize_t sock_recv(int fd, void *data, size_t len) {
  ssize_t n;

  for (;;) {
    n = recv(fd, (char *)data, len, 0);
    if (n == -1) {
      if (n == EINTR) {
        continue;
      } else {
        return -1;
      }
    } else if (n == 0) {
      return 0;
    } else {
      return n;
    }
  }

  return 0;
}

// Frame Parsing Utils
static inline uint8_t frame_get_fin(const unsigned char *buf) {
  return (buf[0] >> 7) & 0x01;
}

static inline uint8_t frame_get_opcode(const unsigned char *buf) {
  return buf[0] & 0x0F;
}

static inline size_t frame_payload_get_len126(const unsigned char *buf) {
  return (buf[2] << 8) | buf[3];
}

static inline size_t frame_payload_get_len127(const unsigned char *buf) {
  return ((uint64_t)buf[2] << 56) | ((uint64_t)buf[3] << 48) |
         ((uint64_t)buf[4] << 40) | ((uint64_t)buf[5] << 32) |
         ((uint64_t)buf[6] << 24) | ((uint64_t)buf[7] << 16) |
         ((uint64_t)buf[8] << 8) | (uint64_t)buf[9];
}

static inline size_t frame_payload_get_raw_len(const unsigned char *buf) {
  return buf[1] & 0X7F;
}

static int frame_decode_payload_len(uint8_t *buf, size_t rbuf_len,
                                    size_t *res) {
  size_t raw_len = frame_payload_get_raw_len(buf);
  *res = raw_len;
  if (raw_len == 126) {
    if (rbuf_len > 3) {
      *res = frame_payload_get_len126(buf);
    } else {
      return 4;
    }
  } else if (raw_len == 127) {
    if (rbuf_len > 9) {
      *res = frame_payload_get_len127(buf);
    } else {
      return 10;
    }
  }

  return 0;
}

static unsigned char *new_frame(const char *src, size_t len,
                                unsigned frame_cfg) {
  // only handle sending small frames
  if (len > 125) {
    return NULL;
  }
  // 2 byte header + 4 byte mask key
  unsigned char *dst = (unsigned char *)malloc(len + 6);
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

#define EXAMPLE_REQUEST                                                        \
  "GET /chat HTTP/1.1"                                                         \
  "\r\n"                                                                       \
  "Host: example.com:8000"                                                     \
  "\r\n"                                                                       \
  "Upgrade: websocket"                                                         \
  "\r\n"                                                                       \
  "Connection: Upgrade"                                                        \
  "\r\n"                                                                       \
  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=="                                \
  "\r\n"                                                                       \
  "Sec-WebSocket-Version: 13"                                                  \
  "\r\n"                                                                       \
  "\r\n"

#define EXAMPLE_REQUEST_EXPECTED_ACCEPT_KEY                                    \
  "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

static int sock_upgrade_ws(int fd) {
  ssize_t sent = sock_sendall(fd, EXAMPLE_REQUEST, sizeof EXAMPLE_REQUEST - 1);
  if (sent != sizeof EXAMPLE_REQUEST - 1) {
    fprintf(stderr, "failed to send upgrade request\n");
    exit(EXIT_FAILURE);
  }

  char buf[4096] = {0};

  // peek because we don't want to miss any frames
  // after upgrade we only want to consume the upgrade response
  // this keeps the function stateless which is more useful for testing

  ssize_t read = recv(fd, buf, 4096, MSG_PEEK);
  if (read == 0) {
    fprintf(stderr, "connection dropped before receiving upgrade response\n");
    exit(EXIT_FAILURE);
  } else if (read == -1) {
    perror("recv");
    exit(EXIT_FAILURE);
  }

  char *upgrade_end = strstr(buf, "\r\n\r\n");
  assert(upgrade_end != NULL);
  upgrade_end += 4;

  read = recv(fd, buf, upgrade_end - buf, 0);
  assert(read == upgrade_end - buf);

  return 0;
}

#endif /* WS_SOCK_UTIL_1235412X */

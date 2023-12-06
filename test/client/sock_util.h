
#ifndef WS_SOCK_UTIL_1235412X
#define WS_SOCK_UTIL_1235412X

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <time.h>



#define OP_TXT 0x1
#define OP_BIN 0x2
#define OP_CLOSE 0x8
#define OP_PING 0x9
#define OP_PONG 0xA


#define HDR_END "\r\n"

#define HDRS_END "\r\n\r\n"

#define EXAMPLE_REQUEST                                                        \
  "GET /chat HTTP/1.1" HDR_END "Host: example.com:8000" HDR_END                \
  "Upgrade: websocket" HDR_END "Connection: Upgrade" HDR_END                   \
  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" HDR_END                        \
  "Sec-WebSocket-Version: 13" HDRS_END

#define EXAMPLE_REQUEST_EXPECTED_ACCEPT_KEY                                    \
  "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

static int sock_new(int ipv6) {
  int fd = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  int on = 1;
  if (setsockopt(fd, SOL_TCP, TCP_NODELAY, &on, sizeof on) == -1) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  };

  return fd;
}

static void sock_connect(int fd, short port, char *addr, int ipv6) {
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
}

// send all data for non blocking sockets
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

static int sock_upgrade_ws(int fd) {
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


  return 0;
}

static inline uint8_t frame_get_opcode(const unsigned char *buf) {
  return buf[0] & 0x0F;
}

#endif /* WS_SOCK_UTIL_1235412X */

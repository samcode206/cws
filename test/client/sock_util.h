
#ifndef WS_SOCK_UTIL_1235412X
#define WS_SOCK_UTIL_1235412X

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
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

#define HDR_END "\r\n"

#define HDRS_END "\r\n\r\n"

#define EXAMPLE_REQUEST                                                        \
  "GET /chat HTTP/1.1" HDR_END "Host: example.com:8000" HDR_END                \
  "Upgrade: websocket" HDR_END "Connection: Upgrade" HDR_END                   \
  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" HDR_END                        \
  "Sec-WebSocket-Version: 13" HDRS_END


#define EXAMPLE_REQUEST_EXPECTED_ACCEPT_KEY "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="


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

#endif /* WS_SOCK_UTIL_1235412X */
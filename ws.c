#define _GNU_SOURCE

#include "handshake.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/signal.h>
#include <sys/socket.h>

#define DEFAULT_PORT 9919
#define LISTEN_BACKLOG (1 << 12) /* 4k */
#define MAX_EVENTS 1024
#define BUF_SIZE (1 << 13) /* 8kb */

typedef struct {
  struct epoll_event events[MAX_EVENTS]; /* event list */
  struct epoll_event ev;                 /* ctl mod event */
  int epoll_fd;
  unsigned char sbuf[BUF_SIZE];                  /* hot buffer */
  unsigned char conn_bufs[MAX_EVENTS][BUF_SIZE]; /* connection specific
                                                        buffers (slow path) */
} server_t;

server_t *server_init(int server_fd);
void server_shutdown(server_t *s, int sfd);
int socket_bind_listen(uint16_t port, uint16_t addr, int backlog);

typedef uint64_t event_ctx_t;

static inline int ev_ctx_get_fd(event_ctx_t ctx);
static inline event_ctx_t ev_ctx_set_fd(event_ctx_t ctx, int fd);
static inline uint32_t ev_ctx_get_buf_offset(event_ctx_t ctx);
static inline event_ctx_t ev_ctx_set_buf_offset(event_ctx_t ctx,
                                                uint32_t offset);

int handle_conn(server_t *s, event_ctx_t ctx, int nops);

int main(void) {
  printf("pid: %d\n", getpid());
  signal(SIGPIPE, SIG_IGN);
  struct sockaddr_storage client_sockaddr;
  socklen_t client_socklen;
  client_socklen = sizeof client_sockaddr;

  int server_fd = socket_bind_listen(DEFAULT_PORT, INADDR_ANY, LISTEN_BACKLOG);
  server_t *server = server_init(server_fd);

  for (;;) {

    int n_evs = epoll_wait(server->epoll_fd, server->events, MAX_EVENTS, -1);
    if (n_evs < 0) {
      perror("epoll_wait");
      return EXIT_FAILURE;
    }

    // loop over events
    for (int i = 0; i < n_evs; ++i) {
      if (ev_ctx_get_fd(server->events[i].data.u64) == server_fd) {
        for (;;) {
          int client_fd =
              accept4(server_fd, (struct sockaddr *)&client_sockaddr,
                      &client_socklen, O_NONBLOCK);
          if ((client_fd < 0)) {
            if (!(errno == EAGAIN)) {
              perror("accept");
            }
            break;
          }

          if (client_fd > MAX_EVENTS - 5) {
            printf("can't index fd: %d\n", client_fd);
            close(client_fd);
            continue;
          }

          server->ev.events = EPOLLIN | EPOLLRDHUP;
          server->ev.data.u64 = ev_ctx_set_fd(0, client_fd);

          assert(epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, client_fd,
                           &server->ev) == 0);
        }

      } else {
        if (server->events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
          int fd = ev_ctx_get_fd(server->events[i].data.u64);
          assert(epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, fd, &server->ev) ==
                 0);
          assert(close(fd) == 0);
        } else {
          if (server->events[i].events & EPOLLOUT) {

          } else if (server->events[i].events & EPOLLIN) {
            int ret = handle_conn(server, server->events[i].data.u64, 8);
            if (ret == -1) {
              int fd = ev_ctx_get_fd(server->events[i].data.u64);
              // ev.data.fd = fd;
              assert(epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, fd,
                               &server->ev) == 0);
              assert(close(fd) == 0);
            }
          }
        }
      }
    }
  }

  server_shutdown(server, server_fd);

  return EXIT_SUCCESS;
}

server_t *server_init(int server_fd) {
  // create a vm mapping, and mlock the hot portion of the server (back it up by
  // RAM and keep it there) the connection specific buffers will page fault on a
  // per needed bases (slow path buffers)
  server_t *server = mmap(NULL, sizeof *server, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
  assert(server != MAP_FAILED);
  if (mlock2(server, offsetof(server_t, conn_bufs), 0) != 0) {
    fprintf(stdout, "[warning]: mlock failed %s\n", strerror(errno));
    errno = 0;
  };

  printf("listening on port:%d\n", DEFAULT_PORT);

  // set up epoll
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    perror("epoll_create1");
    exit(EXIT_FAILURE);
  }
  server->epoll_fd = epoll_fd;

  server->ev.events = EPOLLIN;
  server->ev.data.u64 = ev_ctx_set_fd(0, server_fd);

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &server->ev) < 0) {
    perror("epoll_ctl");
    exit(EXIT_FAILURE);
  };

  return server;
}

int socket_bind_listen(uint16_t port, uint16_t addr, int backlog) {
  int server_fd;
  struct sockaddr_in srv_addr;
  int ret;

  server_fd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (server_fd < 0) {
    return server_fd;
  }

  int on = 1;
  ret = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int));
  if (ret < 0) {
    return ret;
  }

  memset(&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_port = htons(port);
  srv_addr.sin_addr.s_addr = htons(addr);

  ret = bind(server_fd, (const struct sockaddr *)&srv_addr, sizeof(srv_addr));
  if (ret < 0) {
    return ret;
  }

  ret = listen(server_fd, backlog);
  if (listen(server_fd, backlog)) {
    perror("listen");
    exit(1);
  }

  return server_fd;
}

void server_shutdown(server_t *s, int sfd) {
  // end of event loop
  close(s->epoll_fd);
  close(sfd);
  munlockall();
  munmap(s, sizeof *s);
}

#define would_block(n) (n == -1) & ((errno == EAGAIN) | (errno == EWOULDBLOCK))

ssize_t handle_upgrade(const char *buf, char *res_hdrs, size_t n) {
  int ret = get_header(buf, SEC_WS_KEY_HDR, res_hdrs, n);
  if (ret < 0) {
    printf("error parsing http headers: %d\n", ret);
    return -1;
  }

  char accept_key[64];
  int len = ws_derive_accept_hdr(res_hdrs, accept_key, ret-1);

  return ws_build_upgrade_headers(accept_key, len, res_hdrs);
}

int handle_conn(server_t *s, event_ctx_t ctx, int nops) {
  int fd = ev_ctx_get_fd(ctx);
  ssize_t n = recv(fd, s->sbuf, BUF_SIZE, 0);
  if (n == -1) {
    if ((errno == EAGAIN || errno == EWOULDBLOCK)) {
      return 0;
    }
    return -1;
  } else if (n == 0) {
    return -1;
  }

  if (!strncmp((char *)s->sbuf, GET_RQ, sizeof GET_RQ - 1)) {
    printf("Req --------------------------------\n");

    printf("%s", s->sbuf);

    char res_hdrs[1024] = {0};
    ssize_t ret = handle_upgrade(s->sbuf, res_hdrs, sizeof res_hdrs);

    send(fd, res_hdrs, ret-1, 0);
    printf("Res --------------------------------\n");
    printf("%s\n", res_hdrs);

  } else {
    printf("other: %s\n", s->sbuf);
  }

  return 0;
}

static inline int ev_ctx_get_fd(event_ctx_t ctx) {
  return ctx & ((1ULL << 32) - 1);
}

static inline event_ctx_t ev_ctx_set_fd(event_ctx_t ctx, int fd) {
  return (ctx & ~((1ULL << 32) - 1)) | (event_ctx_t)fd;
}

static inline uint32_t ev_ctx_get_buf_offset(event_ctx_t ctx) {
  return (ctx >> 32) & ((1ULL << 32) - 1);
}

static inline event_ctx_t ev_ctx_set_buf_offset(event_ctx_t ctx,
                                                uint32_t offset) {
  return (ctx & ~(((1ULL << 32) - 1) << 32)) | ((event_ctx_t)offset << 32);
}

// -----------------------------------------------------------------------------------
// -------------------------------  WebSocket Utils
// ----------------------------------

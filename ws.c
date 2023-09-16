#define _GNU_SOURCE

#include "frame.h"
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

struct conn {
  int fd;
  size_t buf_in_len;
  size_t buf_out_len;
  uint8_t buf_in[BUF_SIZE];
  uint8_t buf_out[BUF_SIZE];
};

void on_msg(int fd, int binary, size_t len, unsigned char *msg, uint8_t *mask);

void on_msg(int fd, int binary, size_t len, unsigned char *msg, uint8_t *mask) {
  frame_payload_unmask(msg, msg, mask, len);
  printf("msg: %s\n", msg);
}

typedef struct {
  struct epoll_event events[MAX_EVENTS];
  struct epoll_event ev;
  int epoll_fd;
} server_t;

server_t *server_init(int server_fd);
void server_shutdown(server_t *s, int sfd);
int socket_bind_listen(uint16_t port, uint16_t addr, int backlog);

int handle_conn(server_t *s, struct conn *conn, int nops);

void conn_destroy(server_t *server, struct conn *conn) {
  assert(epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, conn->fd, &server->ev) ==
         0);
  assert(close(conn->fd) == 0);
  free(conn);
}

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
      if (server->events[i].data.ptr == server) {
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

          server->ev.events = EPOLLIN | EPOLLRDHUP;
          struct conn *conn = calloc(1, sizeof(struct conn));
          assert(conn != NULL);
          conn->fd = client_fd;
          server->ev.data.ptr = conn;
          assert(epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, client_fd,
                           &server->ev) == 0);
        }

      } else {
        if (server->events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
          conn_destroy(server, server->events[i].data.ptr);
          server->events[i].data.ptr = NULL;
        } else {
          if (server->events[i].events & EPOLLOUT) {

          } else if (server->events[i].events & EPOLLIN) {
            int ret = handle_conn(server, server->events[i].data.ptr, 8);
            if (ret == -1) {
              conn_destroy(server, server->events[i].data.ptr);
              server->events[i].data.ptr = NULL;
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
  server_t *server = mmap(NULL, sizeof *server, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
  assert(server != MAP_FAILED);

  printf("listening on port:%d\n", DEFAULT_PORT);

  // set up epoll
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    perror("epoll_create1");
    exit(EXIT_FAILURE);
  }
  server->epoll_fd = epoll_fd;

  server->ev.events = EPOLLIN;
  server->ev.data.ptr = server;

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
  int len = ws_derive_accept_hdr(res_hdrs, accept_key, ret - 1);

  return ws_build_upgrade_headers(accept_key, len, res_hdrs);
}

int handle_conn(server_t *s, struct conn *conn, int nops) {

  ssize_t n = recv(conn->fd, conn->buf_in + conn->buf_in_len,
                   BUF_SIZE - 1 - conn->buf_in_len, 0);
  if (n == -1) {
    if ((errno == EAGAIN || errno == EWOULDBLOCK)) {
      return 0;
    }
    return -1;
  } else if (n == 0) {
    return -1;
  }

  conn->buf_in_len += n;
  conn->buf_in[conn->buf_in_len] = '\0';

  if (!strncmp((char *)conn->buf_in, GET_RQ, sizeof GET_RQ - 1)) {
    printf("Req --------------------------------\n");
    printf("%s", conn->buf_in);
    char res_hdrs[1024] = {0};
    ssize_t ret =
        handle_upgrade((char *)conn->buf_in, res_hdrs, sizeof res_hdrs);

    send(conn->fd, res_hdrs, ret - 1, 0);
    printf("Res --------------------------------\n");
    printf("%s\n", res_hdrs);

    conn->buf_in_len = 0;
  } else {

    // need at least 2 bytes to read anything
    if (conn->buf_in_len >= 2) {
      uint8_t fin = frame_get_fin(conn->buf_in);
      int masked = frame_is_masked(conn->buf_in);
      // if mask bit isn't set close the connection
      if (!masked) {
        printf("received unmasked client data\n");
        return -1;
      }

      uint8_t opcode = frame_get_opcode(conn->buf_in);
      size_t len = frame_payload_get_len(conn->buf_in);
      printf("fin: %d\n", fin);
      printf("opcode: %d\n", opcode);
      printf("len: %zu\n", len);
      if ((len == PAYLOAD_LEN_16) & (conn->buf_in_len > 3)) {
        len = frame_payload_get_len126(conn->buf_in);
      } else if ((len == PAYLOAD_LEN_64) & (conn->buf_in_len > 9)) {
        len = frame_payload_get_len127(conn->buf_in);
      }

      if ((opcode == OP_BIN) | (opcode == OP_TXT)) {
        size_t mask_offset = frame_get_mask_offset(conn->buf_in, len);
        size_t frame_len = len + mask_offset + 4;
        if (conn->buf_in_len >= frame_len) {
          on_msg(conn->fd, opcode == OP_BIN, len,
                 conn->buf_in + mask_offset + 4, conn->buf_in + mask_offset);
          conn->buf_in_len -= frame_len;
          if (conn->buf_in_len) {
            memmove(conn, conn + frame_len,
                    conn->buf_in_len); // move the remainder to the front
          }
          printf("processed msg, remaining: %zu\n", conn->buf_in_len);
        }
        // msg
      } else if ((opcode == OP_PING) | (opcode == OP_PONG)) {
        // handle ping pong stuff
        printf("PING/PONG\n");
      } else if (opcode == OP_CLOSE) {
        // handle close stuff
      } else if (opcode == OP_CONT) {
        return -1; // unsupported
      }
    }
  }

  return 0;
}

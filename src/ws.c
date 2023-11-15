/* The MIT License

   Copyright (c) 2008, 2009, 2011 by Sam H

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

#define _GNU_SOURCE
#include "ws.h"
#include "buffer.h"
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

#define TIMER_GRANULARITY 5
#define READ_TIMEOUT 60

#define FIN 0x80

#define OP_CONT 0x0
#define OP_TXT 0x1
#define OP_BIN 0x2
#define OP_PING 0x9
#define OP_PONG 0xA
#define OP_CLOSE 0x8

#define PAYLOAD_LEN_16 126
#define PAYLOAD_LEN_64 127

struct ws_conn_t {
  int fd;               // socket fd
  unsigned int flags;   // state flags
  size_t fragments_len; // size of the data portion of the frames across
                        // fragmentation

  size_t needed_bytes; // bytes needed before we can do something with the frame
  buf_t *read_buf;     // recv buffer structure
  buf_t *write_buf;
  ws_server_t *base;          // server ptr
  void *ctx;                  // user data pointer
  unsigned int read_timeout;  // seconds
  unsigned int write_timeout; // seconds
};

struct ws_conn_pool {
  ws_conn_t *base;
  struct buf_node *head;
  struct buf_node _buf_nodes[];
};

struct ws_conn_pool *ws_conn_pool_create(size_t nmemb);
struct ws_conn_t *ws_conn_alloc(struct ws_conn_pool *p);

void ws_conn_free(struct ws_conn_pool *p, struct ws_conn_t *c);

#define CONN_CLOSE_QUEUED (1u << 0)
#define CONN_UPGRADED (1u << 1)

#define CONN_RX_BIN (1u << 2)
#define CONN_RX_FRAGMENTED (1u << 3)
#define CONN_RX_GET_REQUEST (1u << 4)

#define CONN_TX_WRITEABLE (1u << 5)
#define CONN_TX_USING_SHARED (1u << 6)
#define CONN_TX_WRITE_QUEUED (1u << 7)
#define CONN_TX_DISPOSING (1u << 8)

#define CONN_RX_USING_OWN_BUF                                                  \
  (1 << 9) // we are using the connection read buffer
#define CONN_TX_USING_OWN_BUF                                                  \
  (1 << 10) // we are using the connection write buffer

// general purpose dynamic array
// that is used to hold a list of connections
// used for tracking connections that need closing
// and connections that need writing
struct conn_list {
  size_t len;
  size_t cap;
  ws_conn_t **conns;
};

typedef struct server {
  size_t max_msg_len; // max allowed msg length
  ws_msg_cb_t on_ws_msg;
  buf_t *shared_recv_buffer;
  ws_msg_fragment_cb_t on_ws_msg_fragment;
  ws_ping_cb_t on_ws_ping;
  ws_conn_t *shared_send_buffer_owner;
  buf_t *shared_send_buffer;
  ws_pong_cb_t on_ws_pong;

  ws_drain_cb_t on_ws_drain;
  ws_disconnect_cb_t on_ws_disconnect;
  ws_open_cb_t on_ws_open;
  size_t open_conns; // open websocket connections
  size_t max_conns;  // max connections allowed
  ws_accept_cb_t on_ws_accept;
  ws_on_upgrade_req_cb_t on_ws_upgrade_req;
  ws_close_cb_t on_ws_close;

  ws_on_timeout_t on_ws_conn_timeout;
  ws_err_cb_t on_ws_err;
  ws_err_accept_cb_t on_ws_accept_err;
  struct mbuf_pool *buffer_pool;
  struct ws_conn_pool *conn_pool;
  int fd; // server file descriptor
  int epoll_fd;
  bool accept_paused; // are we paused on accepting new connections
  struct epoll_event ev;
  struct epoll_event events[1024];
  struct conn_list writeable_conns;
  struct conn_list closeable_conns;
} ws_server_t;

// connection state utils

static inline bool is_closing(unsigned int const flags) {
  return (flags & CONN_CLOSE_QUEUED) != 0;
}

static inline void mark_closing(ws_conn_t *c) { c->flags |= CONN_CLOSE_QUEUED; }

static inline bool is_upgraded(ws_conn_t *c) {
  return (c->flags & CONN_UPGRADED) != 0;
}

static inline void set_upgraded(ws_conn_t *c) { c->flags |= CONN_UPGRADED; }

static inline void clear_upgraded(ws_conn_t *c) { c->flags &= ~CONN_UPGRADED; }

static inline bool is_bin(ws_conn_t *c) {
  return (c->flags & CONN_RX_BIN) != 0;
}

static inline void set_bin(ws_conn_t *c) { c->flags |= CONN_RX_BIN; }

static inline void clear_bin(ws_conn_t *c) { c->flags &= ~CONN_RX_BIN; }

static inline bool is_fragmented(ws_conn_t *c) {
  return (c->flags & CONN_RX_FRAGMENTED) != 0;
}

static inline void set_fragmented(ws_conn_t *c) {
  c->flags |= CONN_RX_FRAGMENTED;
}

static inline void clear_fragmented(ws_conn_t *c) {
  c->flags &= ~CONN_RX_FRAGMENTED;
}

static inline bool is_http_get_request(ws_conn_t *c) {
  return (c->flags & CONN_RX_GET_REQUEST) != 0;
}

static inline void set_http_get_request(ws_conn_t *c) {
  c->flags |= CONN_RX_GET_REQUEST;
}

static inline void clear_http_get_request(ws_conn_t *c) {
  c->flags &= ~CONN_RX_GET_REQUEST;
}

static inline bool is_writeable(ws_conn_t *c) {
  return (c->flags & CONN_TX_WRITEABLE) != 0;
}

static inline void set_writeable(ws_conn_t *c) {
  c->flags |= CONN_TX_WRITEABLE;
}

static inline void clear_writeable(ws_conn_t *c) {
  c->flags &= ~CONN_TX_WRITEABLE;
}

static inline bool is_write_queued(ws_conn_t *c) {
  return (c->flags & CONN_TX_WRITE_QUEUED) != 0;
}

static inline void set_write_queued(ws_conn_t *c) {
  c->flags |= CONN_TX_WRITE_QUEUED;
}

static inline void clear_write_queued(ws_conn_t *c) {
  c->flags &= ~CONN_TX_WRITE_QUEUED;
}

static inline bool is_using_shared(ws_conn_t *c) {
  return (c->flags & CONN_TX_USING_SHARED) != 0;
}

static inline void set_using_shared(ws_conn_t *c) {
  c->flags |= CONN_TX_USING_SHARED;
}

static inline void clear_using_shared(ws_conn_t *c) {
  c->flags &= ~CONN_TX_USING_SHARED;
}

static inline bool is_write_shutdown(ws_conn_t *c) {
  return (c->flags & CONN_TX_DISPOSING) != 0;
}

static inline void set_write_shutdown(ws_conn_t *c) {
  c->flags |= CONN_TX_DISPOSING;
}

static inline bool is_using_own_recv_buf(ws_conn_t *c) {
  return (c->flags & CONN_RX_USING_OWN_BUF) != 0;
}

static inline void set_using_own_recv_buf(ws_conn_t *c) {
  c->flags |= CONN_RX_USING_OWN_BUF;
}

static inline void clear_using_own_recv_buf(ws_conn_t *c) {
  c->flags &= ~CONN_RX_USING_OWN_BUF;
}

static inline bool is_using_own_write_buf(ws_conn_t *c) {
  return (c->flags & CONN_TX_USING_OWN_BUF) != 0;
}

static inline void set_using_own_write_buf(ws_conn_t *c) {
  c->flags |= CONN_TX_USING_OWN_BUF;
}

static inline void clear_using_own_write_buf(ws_conn_t *c) {
  c->flags &= ~CONN_TX_USING_OWN_BUF;
}

// Frame Utils
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
  if (raw_len == PAYLOAD_LEN_16) {
    if (rbuf_len > 3) {
      *res = frame_payload_get_len126(buf);
    } else {
      return 4;
    }
  } else if (raw_len == PAYLOAD_LEN_64) {
    if (rbuf_len > 9) {
      *res = frame_payload_get_len127(buf);
    } else {
      return 10;
    }
  }

  return 0;
}

static inline int frame_has_reserved_bits_set(uint8_t const *buf) {
  return (buf[0] & 0x70) != 0;
}

static inline uint32_t frame_is_masked(const unsigned char *buf) {
  return (buf[1] >> 7) & 0x01;
}

static inline size_t frame_get_header_len(size_t const n) {
  return 2 + ((n > 125) * 2) + ((n > 0xFFFF) * 6);
}

static void msg_unmask(uint8_t *src, uint8_t const *mask, size_t const n) {

  size_t i = 0;
  size_t left_over = n & 3;

  for (; i < left_over; ++i) {
    src[i] = src[i] ^ mask[i & 3];
  }

  while (i < n) {
    src[i] = src[i] ^ mask[i & 3];
    src[i + 1] = src[i + 1] ^ mask[(i + 1) & 3];
    src[i + 2] = src[i + 2] ^ mask[(i + 2) & 3];
    src[i + 3] = src[i + 3] ^ mask[(i + 3) & 3];
    i += 4;
  }
}

static void ws_server_epoll_ctl(ws_server_t *s, int op, int fd);

static void ws_conn_handle(ws_conn_t *conn);

static void handle_upgrade(ws_conn_t *conn);
static inline int conn_read(ws_conn_t *conn, buf_t *buf);

static int conn_drain_write_buf(ws_conn_t *conn, buf_t *wbuf);

static int conn_write_frame(ws_conn_t *conn, void *data, size_t len,
                            uint8_t op);

static void conn_list_append(struct conn_list *cl, ws_conn_t *conn) {
  if (cl->len + 1 < cl->cap) {
    cl->conns[cl->len++] = conn;
  } else {
    fprintf(stderr, "%s: would overflow\n", "conn_list_append");
    exit(EXIT_FAILURE);
  }
}

static void server_writeable_conns_append(ws_conn_t *c) {
  // to be added to the list:
  // a connection must not already be queued for writing
  // a connection must be in a writeable state
  if (is_writeable(c) && !is_write_queued(c)) {
    conn_list_append(&c->base->writeable_conns, c);
    set_write_queued(c);
  }
}

static void server_closeable_conns_append(ws_conn_t *c) {
  // clear the shared buffer first if owned by this connection
  if (is_using_shared(c)) {
    buf_reset(c->base->shared_send_buffer);
    clear_using_shared(c);
    c->base->shared_send_buffer_owner = NULL;
  }
  c->base->ev.data.ptr = c;
  ws_server_epoll_ctl(c->base, EPOLL_CTL_DEL, c->fd);
  conn_list_append(&c->base->closeable_conns, c);
  mark_closing(c);
}

static void server_writeable_conns_drain(ws_server_t *s) {
  if (s->shared_send_buffer_owner) {
    ws_conn_t *c = s->shared_send_buffer_owner;
    if (!is_closing(c->flags) &&
        conn_drain_write_buf(c, s->shared_send_buffer) == -1) {
      // buf_reset(s->shared_send_buffer);
      ws_conn_destroy(c);
    };
    clear_using_shared(c);
    s->shared_send_buffer_owner = NULL;
  }
  if (s->writeable_conns.len) {
    printf("draining %zu connections\n", s->writeable_conns.len);
  }

  for (size_t i = 0; i < s->writeable_conns.len; ++i) {
    ws_conn_t *c = s->writeable_conns.conns[i];
    if (!is_closing(c->flags)) {
      if (conn_drain_write_buf(c, c->write_buf) == -1) {
        ws_conn_destroy(c);
      };
      clear_write_queued(c);
    }
  }

  // looping from the back while decrementing len might be faster, just have to
  // keep FIFO
  s->writeable_conns.len = 0;
}

static void server_closeable_conns_close(ws_server_t *s) {
  if (s->closeable_conns.len) {
    // printf("closing %zu connections\n", s->closeable_conns.len);
    size_t n = s->closeable_conns.len;
    while (n--) {
      ws_conn_t *c = s->closeable_conns.conns[n];
      assert(close(c->fd) == 0);
      s->on_ws_disconnect(c, 0);
      mbuf_put(s->buffer_pool, c->write_buf);
      mbuf_put(s->buffer_pool, c->read_buf);

      if (s->shared_send_buffer_owner == c) {
        s->shared_send_buffer_owner = NULL;
      }
      ws_conn_free(s->conn_pool, c);
      --s->open_conns;
    }

    s->closeable_conns.len = 0;
  }
}

ws_server_t *ws_server_create(struct ws_server_params *params, int *ret) {
  if (ret == NULL) {
    return NULL;
  };

  if (params->port <= 0) {
    *ret = WS_CREAT_EBAD_PORT;
    return NULL;
  }

  if (!params->on_ws_open || !params->on_ws_msg || !params->on_ws_disconnect) {
    *ret = WS_CREAT_ENO_CB;
    return NULL;
  }

  ws_server_t *s;

  // allocate memory for server and events for epoll
  s = (ws_server_t *)calloc(1, (sizeof *s));
  if (s == NULL) {
    *ret = WS_ESYS;
    return NULL;
  }

  // socket init
  struct sockaddr_in6 srv_addr;

  s->fd =
      socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  if (s->fd < 0) {
    *ret = WS_ESYS;
    free(s);
    return NULL;
  }

  // socket config
  int on = 1;
  *ret = setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &on,
                    sizeof(int));
  if (*ret < 0) {
    *ret = WS_ESYS;
    free(s);
    return NULL;
  }

  int off = 0;
  *ret = setsockopt(s->fd, SOL_IPV6, IPV6_V6ONLY, &off, sizeof(int));
  if (*ret < 0) {
    *ret = WS_ESYS;
    free(s);
    return NULL;
  }

  // fill in addr info
  memset(&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin6_family = AF_INET6;
  srv_addr.sin6_port = htons(params->port);
  inet_pton(AF_INET6, params->addr, &srv_addr.sin6_addr);

  // bind server socket
  *ret = bind(s->fd, (const struct sockaddr *)&srv_addr, sizeof(srv_addr));
  if (*ret < 0) {
    *ret = WS_ESYS;
    close(s->fd);
    free(s);
    return NULL;
  }

  // init epoll
  s->epoll_fd = epoll_create1(0);
  if (s->epoll_fd < 0) {
    *ret = WS_ESYS;
    close(s->fd);
    free(s);
    return NULL;
  }

  // mandatory callbacks
  s->on_ws_open = params->on_ws_open;
  s->on_ws_msg = params->on_ws_msg;
  s->on_ws_disconnect = params->on_ws_disconnect;

  if (params->on_ws_pong) {
    s->on_ws_pong = params->on_ws_pong;
  }

  if (params->on_ws_err) {
    s->on_ws_err = params->on_ws_err;
  }

  if (params->on_ws_ping) {
    s->on_ws_ping = params->on_ws_ping;
  }

  if (params->on_ws_close) {
    s->on_ws_close = params->on_ws_close;
  }

  if (params->on_ws_drain) {
    s->on_ws_drain = params->on_ws_drain;
  }

  if (params->on_ws_msg_fragment) {
    s->on_ws_msg_fragment = params->on_ws_msg_fragment;
  }

  if (params->on_ws_accept) {
    s->on_ws_accept = params->on_ws_accept;
  }

  if (params->on_ws_accept_err) {
    s->on_ws_accept_err = params->on_ws_accept_err;
  }

  if (params->on_ws_upgrade_req) {
    s->on_ws_upgrade_req = params->on_ws_upgrade_req;
  }

  if (params->on_ws_conn_timeout) {
    s->on_ws_conn_timeout = params->on_ws_conn_timeout;
  }

  size_t max_backpressure =
      params->max_buffered_bytes ? params->max_buffered_bytes : 16000;
  size_t page_size = getpagesize();

  // account for an interleaved control msg during fragmentation
  // since we never dynamically allocate more buffers
  size_t buffer_size =
      (max_backpressure + 132 + page_size - 1) & ~(page_size - 1);

  printf("buffer size = %zu\n", buffer_size);
  printf("max_backpressure = %zu\n", max_backpressure);

  struct rlimit rlim = {0};
  assert(getrlimit(RLIMIT_NOFILE, &rlim) == 0);

  if (!params->max_conns) {
    // account for already open files
    s->max_conns = rlim.rlim_cur - 16;
  } else if (params->max_conns < 16) {
    s->max_conns = 16;
  } else if (params->max_conns < rlim.rlim_cur) {
    s->max_conns = params->max_conns;

    if (s->max_conns > rlim.rlim_cur - 8) {
      fprintf(stderr,
              "[WARN] params->max_conns-%zu may be too high. RLIMIT_NOFILE=%zu "
              "only %zu open files would remain\n",
              s->max_conns, rlim.rlim_cur, rlim.rlim_cur - s->max_conns);
    }

  } else if (params->max_conns >= rlim.rlim_cur) {
    s->max_conns = params->max_conns;
    fprintf(stderr, "[WARN] params->max_conns %zu exceeds RLIMIT_NOFILE %zu\n",
            s->max_conns, rlim.rlim_cur);
  }

  printf("max_conns = %zu\n", s->max_conns);
  s->buffer_pool =
      mbuf_pool_create(s->max_conns + s->max_conns + 2, buffer_size);

  s->conn_pool = ws_conn_pool_create(s->max_conns);

  assert(s->conn_pool != NULL);
  assert(s->buffer_pool != NULL);

  s->max_msg_len = max_backpressure;

  s->shared_send_buffer_owner = NULL;
  assert(s->buffer_pool != NULL);

  // allocate the list (dynamic array of pointers to ws_conn_t) to track
  // writeable connections
  s->writeable_conns.conns =
      calloc(s->max_conns, sizeof s->writeable_conns.conns);
  s->writeable_conns.len = 0;
  s->writeable_conns.cap = s->max_conns;

  // allocate the list (dynamic array of pointers to ws_conn_t) to track
  // closeable connections
  s->closeable_conns.conns =
      calloc(s->max_conns, sizeof s->closeable_conns.conns);
  s->closeable_conns.len = 0;
  s->closeable_conns.cap = s->max_conns;

  // make sure we got the mem needed
  assert(s->writeable_conns.conns != NULL && s->closeable_conns.conns != NULL);
  // server resources all ready
  return s;
}

static void ws_server_epoll_ctl(ws_server_t *s, int op, int fd) {
  if (epoll_ctl(s->epoll_fd, op, fd, &s->ev) == -1) {
    if (s->on_ws_err) {
      int err = errno;
      s->on_ws_err(s, err);
      exit(EXIT_FAILURE);
    } else {
      perror("epoll_ctl");
      exit(EXIT_FAILURE);
    }
  };
}

// shutting down our write end of the socket
// must be called AFTER the final write has completed
static int conn_shutdown_wr(ws_conn_t *c) {
  if (shutdown(c->fd, SHUT_WR) == -1) {
    ws_conn_destroy(c);
    return -1;
  }

  return 0;
}

static void ws_server_conns_establish(ws_server_t *s, int fd,
                                      struct sockaddr *sockaddr,
                                      socklen_t *socklen) {
  unsigned int now = (unsigned int)time(NULL);
  // how many conns should we try to accept in total
  size_t accepts = (s->accept_paused == 0) * (s->max_conns - s->open_conns);
  // if we can do atleast one, then let's get started...

  int sockopt_on = 1;

  if (accepts) {
    while (accepts--) {
      int client_fd =
          accept4(fd, sockaddr, socklen, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (client_fd != -1) {
        if (s->on_ws_accept &&
            s->on_ws_accept(s, (struct sockaddr_storage *)sockaddr,
                            client_fd) == -1) {
          if (close(client_fd) == -1) {
            if (s->on_ws_err) {
              int err = errno;
              s->on_ws_err(s, err);
            } else {
              perror("close()");
              exit(EXIT_FAILURE);
            }
          };
          continue;
        };

        // disable Nagle's algorithm
        if (setsockopt(client_fd, SOL_TCP, TCP_NODELAY, &sockopt_on,
                       sizeof(sockopt_on)) == -1) {
          if (s->on_ws_err) {
            int err = errno;
            s->on_ws_err(s, err);
            exit(EXIT_FAILURE);
          } else {
            perror("setsockopt");
            exit(EXIT_FAILURE);
          }
          return;
        }

        ws_conn_t *conn = ws_conn_alloc(s->conn_pool);
        assert(conn != NULL); // TODO(sah): remove this
        s->ev.events = EPOLLIN | EPOLLRDHUP;
        conn->fd = client_fd;
        conn->base = s;
        conn->needed_bytes = 12;
        set_writeable(conn);
        conn->fd = client_fd;
        conn->base = s;

        s->ev.data.ptr = conn;

        conn->read_buf = mbuf_get(s->buffer_pool);
        conn->write_buf = mbuf_get(s->buffer_pool);

        assert(conn->read_buf != NULL);
        assert(conn->write_buf != NULL);

        conn->read_timeout = now + READ_TIMEOUT;
        ws_server_epoll_ctl(s, EPOLL_CTL_ADD, client_fd);
        ++s->open_conns;

      } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return; // done
        } else if (errno == EMFILE || errno == ENFILE) {
          // too many open files in either the proccess or entire system
          // remove the server from epoll, it must be re added when atleast one
          // fd closes
          ws_server_epoll_ctl(s, EPOLL_CTL_DEL, fd);
          s->accept_paused = 1;
          if (s->on_ws_accept_err) {
            int err = errno;
            s->on_ws_accept_err(s, err);
          }
          return; // done
        } else if (errno == ENONET || errno == EPROTO || errno == ENOPROTOOPT ||
                   errno == EOPNOTSUPP || errno == ENETDOWN ||
                   errno == ENETUNREACH || errno == EHOSTDOWN ||
                   errno == EHOSTUNREACH || errno == ECONNABORTED ||
                   errno == EINTR) {
          if (s->on_ws_accept_err) {
            int err = errno;
            s->on_ws_accept_err(s, err);
          }
          // call the accept error callback if registered
          // and continue on to the next connection in the accept queue
        } else {
          // this is non recoverable, report to the internal error callback
          if (s->on_ws_err) {
            int err = errno;
            s->on_ws_err(s, err);
            exit(EXIT_FAILURE);
          } else {
            perror("accept");
            exit(EXIT_FAILURE);
          }
          return; // done
        }
      }
    }

    if (s->max_conns == s->open_conns) {
      // remove the server from epoll, it must be re added when atleast one
      // fd closes
      ws_server_epoll_ctl(s, EPOLL_CTL_DEL, fd);
      s->accept_paused = 1;
      if (s->on_ws_accept_err) {
        s->on_ws_accept_err(s, 0);
      }
    }
  }
}

int ws_server_start(ws_server_t *s, int backlog) {
  int ret = listen(s->fd, backlog);
  if (ret < 0) {
    return ret;
  }

  buf_t *shared_rxb = mbuf_get(s->buffer_pool);
  buf_t *shared_txb = mbuf_get(s->buffer_pool);

  s->shared_recv_buffer = shared_rxb;
  s->shared_send_buffer = shared_txb;

  int fd = s->fd;
  int epfd = s->epoll_fd;

  struct sockaddr_storage client_sockaddr;
  socklen_t client_socklen;
  client_socklen = sizeof client_sockaddr;
  s->ev.data.ptr = s;
  s->ev.events = EPOLLIN;

  ws_server_epoll_ctl(s, EPOLL_CTL_ADD, fd);

  int tfd;
  assert((tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)) !=
         -1);

  struct itimerspec timer = {.it_interval =
                                 {
                                     .tv_nsec = 0,
                                     .tv_sec = TIMER_GRANULARITY,
                                 },
                             .it_value = {
                                 .tv_nsec = 0,
                                 .tv_sec = TIMER_GRANULARITY,
                             }};

  assert(timerfd_settime(tfd, 0, &timer, NULL) != -1);

  s->ev.data.ptr = &tfd;
  s->ev.events = EPOLLIN;
  ws_server_epoll_ctl(s, EPOLL_CTL_ADD, tfd);

  bool do_timers_sweep = false;

  for (;;) {
    int n_evs = epoll_wait(epfd, s->events, 1024, -1);
    if (n_evs < 0) {
      if (s->on_ws_err) {
        int err = errno;
        s->on_ws_err(s, err);
      } else {
        perror("epoll_wait");
        exit(EXIT_FAILURE);
      }
      return -1;
    }

    // loop over events
    for (int i = 0; i < n_evs; ++i) {
      if (s->events[i].data.ptr == &tfd) {
        uint64_t _;
        assert(read(tfd, &_, 8) == 8);
        do_timers_sweep = true;
        (void)_;
      } else if (s->events[i].data.ptr == s) {
        ws_server_conns_establish(s, fd, (struct sockaddr *)&client_sockaddr,
                                  &client_socklen);
      } else {
        ws_conn_t *c = s->events[i].data.ptr;
        if (s->events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
          ws_conn_destroy(s->events[i].data.ptr);
        } else {
          if (s->events[i].events & EPOLLOUT) {
            if (!is_closing(c->flags)) {
              int ret = conn_drain_write_buf(c, c->write_buf);
              if (ret == 1) {
                if (!is_write_shutdown(c)) {
                  if (s->on_ws_drain) {
                    s->on_ws_drain(c);
                  }

                  if (!is_upgraded(c)) {
                    set_upgraded(c);
                    c->needed_bytes = 2;
                    c->read_timeout = time(NULL) + READ_TIMEOUT;
                    s->on_ws_open(c);
                  }
                } else {
                  // we wrote everything we needed to write
                  // we can shutdown our write end
                  if (conn_shutdown_wr(c) == -1) {
                    continue; // skip arming EPOLLIN if this fails
                  };
                }

                s->ev.data.ptr = c;
                s->ev.events = EPOLLIN | EPOLLRDHUP;
                if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &s->ev) == -1) {
                  if (s->on_ws_err) {
                    int err = errno;
                    s->on_ws_err(s, err);
                  } else {
                    perror("epoll_ctl");
                    exit(EXIT_FAILURE);
                  }
                };
              } else if (ret == -1) {
                ws_conn_destroy(c);
              }
            }
          }
          if (s->events[i].events & EPOLLIN) {
            if (!is_closing(c->flags)) {
              if (is_upgraded(c)) {
                ws_conn_handle(c);
              } else {
                handle_upgrade(c);
              }
              assert(buf_len(s->shared_recv_buffer) == 0);
            }
          }
        }
      }
    }

    server_writeable_conns_drain(s); // drain writes

    if (do_timers_sweep) {
      unsigned int now = (unsigned int)time(NULL);

      int timeout_kind = 0;
      size_t n = s->max_conns;

      ws_on_timeout_t cb = s->on_ws_conn_timeout;

      while (n--) {
        ws_conn_t *c = &s->conn_pool->base[n];

        timeout_kind += c->read_timeout != 0 && c->read_timeout < now;
        timeout_kind += ((c->write_timeout != 0 && c->write_timeout < now) * 2);

        if (timeout_kind) {
          c->read_timeout = 0;
          c->write_timeout = 0;

          if (cb) {
            cb(c, timeout_kind);
          } else {
            ws_conn_destroy(c);
          }

          timeout_kind = 0;
          ws_conn_destroy(c);
        }
      }

      do_timers_sweep = false;
    }

    bool accept_resumable =
        s->accept_paused &&
        s->open_conns - s->closeable_conns.len <= s->max_conns;
    server_closeable_conns_close(s); // close connections
    if (accept_resumable) {
      s->ev.events = EPOLLIN;
      s->ev.data.ptr = s;
      ws_server_epoll_ctl(s, EPOLL_CTL_ADD, fd);
      s->accept_paused = 0;
    }
  }

  return 0;
}

static int conn_read(ws_conn_t *conn, buf_t *buf) {
  ssize_t n = buf_recv(buf, conn->fd, 0);
  if (n == -1) {
    if ((errno == EAGAIN || errno == EINTR)) {
      return 0;
    }
    return -1;
  } else if (n == 0) {
    return -1;
  }

  return 0;
}

// takes a nul terminated string and finds an http request header
static inline int get_header(const char *headers, const char *key, char *val,
                             size_t n) {
  char *header_start = strcasestr(headers, key);
  if (header_start) {
    header_start =
        strchr(header_start,
               ':'); // skip colon symbol, if not found header is malformed
    if (header_start == NULL) {
      return ERR_HDR_MALFORMED;
    }

    ++header_start; // skipping colon symbol happens here after validating it's
                    // existence in the buffer
    // skip spaces
    while (*header_start == SPACE) {
      ++header_start;
    }

    const char *header_end =
        strstr(header_start, CRLF); // move to the end of the header value
    if (header_end) {
      // if string is larger than n, return to caller with ERR_HDR_TOO_LARGE
      if ((header_end - header_start) + 1 > n) {
        return ERR_HDR_TOO_LARGE;
      }
      memcpy(val, header_start, (header_end - header_start));
      val[header_end - header_start + 1] =
          '\0'; // nul terminate the header value
      return header_end - header_start +
             1; // we only add one here because of adding '\0'
    } else {
      return ERR_HDR_MALFORMED; // if no CRLF is found the headers is malformed
    }
  }

  return ERR_HDR_NOT_FOUND; // header isn't found
}

static const char magic_str[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

#define MAGIC_STR_LEN 36

static inline int ws_derive_accept_hdr(const char *akhdr_val, char *derived_val,
                                       size_t len) {
  unsigned char buf[64] = {0};
  memcpy(buf, akhdr_val, strlen(akhdr_val));
  strcat((char *)buf, magic_str);
  len += MAGIC_STR_LEN;

  unsigned char hash[20] = {0};
  SHA1(buf, len, hash);

  return Base64encode(derived_val, (const char *)hash, sizeof hash);
}

static void handle_upgrade(ws_conn_t *conn) {
  ws_server_t *s = conn->base;
  buf_t *request_buf;
  buf_t *response_buf = NULL;
  size_t resp_len = 0;

  // pick the recv buffer
  if (is_using_own_recv_buf(conn)) {
    request_buf = conn->read_buf;
  } else {
    request_buf = s->shared_recv_buffer;
  }

  // read from the socket
  if (conn_read(conn, request_buf) == -1) {
    ws_conn_destroy(conn);
    buf_reset(s->shared_recv_buffer);
    return;
  };

  // get how much has accumulated so far
  size_t request_buf_len = buf_len(request_buf);

  // if we are disposing the connection
  // it means that we received a bad request or an internal server error ocurred
  if (is_write_shutdown(conn)) {
    conn->fragments_len += request_buf_len;
    // client sending too much data after shutting down our write end
    if (conn->fragments_len > 8192) {
      ws_conn_destroy(conn);
    }

    // reset the buffer, we discard all data after socket is marked disposing
    buf_reset(request_buf);
    return;
  }

  // if we still have less than needed bytes
  // stop and wait for more
  if (request_buf_len < conn->needed_bytes) {
    if (request_buf != conn->read_buf) {
      set_using_own_recv_buf(conn);
      buf_move(s->shared_recv_buffer, conn->read_buf,
               buf_len(s->shared_recv_buffer));
    }

    return;
  }

  // if false by the time we write then call shutdown
  bool ok = false;

  uint8_t *headers = buf_peek(request_buf);

  headers[request_buf_len] = '\0';

  // make sure it's a GET request
  bool is_get_request = is_http_get_request(conn);
  if (!is_get_request) {
    // this is the first read
    // validate that it's a GET request and increase the needed_bytes
    if (memcmp((char *)headers, GET_RQ, GET_RQ_LEN) == 0) {
      set_http_get_request(conn);
      is_get_request = true;
      // Sec-WebSocket-Accept:s3pPLMBiTxaQ9kYGzzhZRbK+xOo= is 49 bytes and
      // that's the absolute minimum (practically still higher because there
      // will be other headers)
      conn->needed_bytes += 49;
    };
  };

  if (is_get_request) {
    // only start processing when the final header has arrived
    bool header_end_reached =
        memcmp((char *)headers + request_buf_len - CRLF2_LEN, CRLF2,
               CRLF2_LEN) == 0;

    // we reached the request headers end
    // start reading the headers and extract Sec-WebSocket-Key
    if (header_end_reached) {
      char sec_websocket_key[25] = {0};
      int ret = get_header((char *)headers, SEC_WS_KEY_HDR, sec_websocket_key,
                           sizeof sec_websocket_key);

      if (ret == 25) {
        char accept_key[32];
        int accept_key_len =
            ws_derive_accept_hdr(sec_websocket_key, accept_key, ret - 1) - 1;

        if (!s->on_ws_upgrade_req) {
          ok = true;
          bool pmd = false;

          char sec_websocket_extensions[1024];

          int sec_websocket_extensions_ret = get_header((char *)headers, "Sec-WebSocket-Extensions", sec_websocket_extensions, 1024);
          if (sec_websocket_extensions_ret > 0){
            pmd = true;
          }



          response_buf = request_buf;
          buf_reset(response_buf);
          buf_put(response_buf, switching_protocols,
                  SWITCHING_PROTOCOLS_HDRS_LEN);
          buf_put(response_buf, accept_key, accept_key_len);
          if (pmd){
          buf_put(response_buf, "\r\nSec-WebSocket-Extensions: permessage-deflate", 46);
          }

          buf_put(response_buf, CRLF2, CRLF2_LEN);
          resp_len = buf_len(response_buf);
        } else {
          if (!buf_len(conn->write_buf) &&
              s->shared_send_buffer_owner == NULL) {
            s->shared_send_buffer_owner = conn;
            set_using_shared(conn);
            response_buf = s->shared_send_buffer;
          } else {
            response_buf = conn->write_buf;
          }

          size_t max_resp_len = buf_space(response_buf);
          resp_len = s->on_ws_upgrade_req(conn, (char *)headers, accept_key,
                                          max_resp_len,
                                          (char *)buf_peek(response_buf));

          buf_consume(request_buf, request_buf_len);
          if ((resp_len > 0) & (resp_len <= max_resp_len)) {
            response_buf->wpos += resp_len;
            ok = true;
          } else {
            set_write_shutdown(conn);
            buf_put(response_buf, internal_server_error,
                    INTERNAL_SERVER_ERROR_LEN);
          }
        }

      } else {
        set_write_shutdown(conn);
        buf_reset(request_buf);
        response_buf = request_buf;
        buf_put(response_buf, bad_request, BAD_REQUEST_LEN);
        printf("error parsing http headers: %d\n", ret);
      }

    } else {
      // there's still more data to be read from the network to get the full
      // header
      if (request_buf == s->shared_recv_buffer) {
        set_using_own_recv_buf(conn);
        buf_move(s->shared_recv_buffer, conn->read_buf,
                 buf_len(s->shared_recv_buffer));
      }
      return;
    }

  } else {
    set_write_shutdown(conn);
    buf_reset(request_buf);
    response_buf = request_buf;
    buf_put(response_buf, bad_request, BAD_REQUEST_LEN);
  }

  if (is_writeable(conn)) {
    int ret = conn_drain_write_buf(conn, response_buf);
    if (ret == 1) {
      clear_using_own_recv_buf(conn);

      if (ok) {
        clear_http_get_request(conn);
        s->on_ws_open(conn);
        set_upgraded(conn);
        conn->read_timeout = time(NULL) + READ_TIMEOUT;
        conn->needed_bytes =
            2; // reset to the minimum needed to parse a ws header
      } else {
        if (shutdown(conn->fd, SHUT_WR) == -1) {
          if (!s->on_ws_err) {
            int err = errno;
            s->on_ws_err(s, err);
          } else {
            perror("shutdown");
          }
        };
      }

    } else if (ret == -1) {
      ws_conn_destroy(conn);
    } else {
      if (response_buf != conn->write_buf ||
          response_buf != s->shared_send_buffer) {
        buf_move(response_buf, conn->write_buf, buf_len(response_buf));
      }
    }
  } else {
    buf_put(conn->write_buf, bad_request, BAD_REQUEST_LEN);
  }
}

static inline buf_t *ws_conn_choose_read_buf(ws_conn_t *conn) {
  if (!is_using_own_recv_buf(conn)) {
    return conn->base->shared_recv_buffer;
  } else {
    return conn->read_buf;
  }

  // if ((buf_len(conn->read_buf) != 0) &
  //     !(conn->fragments_len + conn->read_buf->rpos ==
  //       conn->read_buf->wpos)) {
  //   // buf_debug(&conn->read_buf, "conn buffer chosen");

  //   return conn->read_buf;
  // } else {
  //   return conn->base->shared_recv_buffer;
  // }
}

static size_t ws_conn_readable_len(ws_conn_t *conn, buf_t *buf) {
  if (buf != conn->read_buf) {
    return buf->wpos - buf->rpos;
  } else {
    return buf->wpos - buf->rpos -
           conn->fragments_len; // do we really need the branch?
  }
}

static inline void ws_conn_handle(ws_conn_t *conn) {
  buf_t *buf = ws_conn_choose_read_buf(conn);
  ws_server_t *s = conn->base;

  unsigned int next_read_timeout = time(NULL) + READ_TIMEOUT;

  // total frame header bytes trimmed
  size_t total_trimmed = 0;
  size_t max_allowed_len = s->max_msg_len;

  if (conn_read(conn, buf) == -1) {
    ws_conn_destroy(conn);
    buf_reset(s->shared_recv_buffer);
    return;
  }

  while (ws_conn_readable_len(conn, buf) - total_trimmed >=
         conn->needed_bytes) {
    // payload start
    uint8_t *frame =
        buf_peek_at(buf, buf->rpos + ((buf == conn->read_buf) *
                                      (conn->fragments_len + total_trimmed)));

    uint8_t fin = frame_get_fin(frame);
    uint8_t opcode = frame_get_opcode(frame);
    // printf("fragments=%zu\n", conn->state.fragments_len);
    // run general validation checks on the header
    if (((fin == 0) & ((opcode > 2) & (opcode != OP_CONT))) |
        (frame_has_reserved_bits_set(frame) == 1) |
        (frame_is_masked(frame) == 0)) {
      buf_reset(s->shared_recv_buffer);
      ws_conn_destroy(conn);
      return;
    }

    // make sure we can get the full msg
    size_t payload_len = 0;
    size_t frame_buf_len = buf_len(buf);
    if (conn->read_buf == buf) {
      frame_buf_len = frame_buf_len - conn->fragments_len - total_trimmed;
    }
    // check if we need to do more reads to get the msg length
    int missing_header_len =
        frame_decode_payload_len(frame, frame_buf_len, &payload_len);
    if (missing_header_len) {
      // wait for atleast remaining of the header
      conn->needed_bytes = missing_header_len;
      goto clean_up_buffer;
    }

    size_t mask_offset = frame_get_header_len(payload_len);
    size_t full_frame_len = payload_len + 4 + mask_offset;

    // validate frame length
    if (payload_len > max_allowed_len) {
      // drop the connection
      ws_conn_close(conn, NULL, 0, WS_CLOSE_TOO_LARGE);
      buf_reset(s->shared_recv_buffer);
      return;
    }

    // check that we have atleast the whole frame, otherwise
    // set needed_bytes and exit waiting for more reads from the socket
    if (frame_buf_len < full_frame_len) {
      conn->needed_bytes = full_frame_len;
      goto clean_up_buffer;
    }

    uint8_t *msg = frame + mask_offset + 4;
    msg_unmask(msg, frame + mask_offset, payload_len);
    // printf("buf_len=%zu frame_len=%zu opcode=%d fin=%d\n",
    // frame_buf_len,
    //        full_frame_len, opcode, fin);
    conn->read_timeout = next_read_timeout;
    switch (opcode) {
    case OP_TXT:
    case OP_BIN:
      conn->flags &= ~CONN_RX_BIN;
      conn->flags |= (opcode == OP_BIN) * CONN_RX_BIN;
      // fin and never fragmented
      // this handles both text and binary hence the fallthrough
      if (fin & (!is_fragmented(conn))) {
        if (!is_bin(conn) && !utf8_is_valid(msg, payload_len)) {
          ws_conn_destroy(conn);
          buf_reset(s->shared_recv_buffer);
          return; // TODO(sah): send a Close frame, & call close callback
        }
        s->on_ws_msg(conn, msg, payload_len, is_bin(conn));
        buf_consume(buf, full_frame_len);
        clear_bin(conn);
        conn->needed_bytes = 2;

        break; /* OP_BIN don't fall through to fragmented msg */
      } else if (fin & (is_fragmented(conn))) {
        // this is invalid because we expect continuation not text or binary
        // opcode
        ws_conn_destroy(conn);
        buf_reset(s->shared_recv_buffer);
        return;
      }

    case OP_CONT:
      // accumulate bytes and increase fragments_len

      // move bytes over
      // call the callback
      // reset
      // can't send cont as first fragment
      if ((opcode == OP_CONT) & (!is_fragmented(conn))) {
        ws_conn_destroy(conn);
        buf_reset(s->shared_recv_buffer);
        return;
      }

      if (conn->fragments_len + payload_len > max_allowed_len) {
        ws_conn_close(conn, NULL, 0, WS_CLOSE_TOO_LARGE);
        buf_reset(s->shared_recv_buffer);
        return;
      }

      // set the state to fragmented after validation
      set_fragmented(conn);

      if (!s->on_ws_msg_fragment) {

        // we are using the shared buffer
        if (buf != conn->read_buf) {
          // trim off the header
          buf_consume(buf, mask_offset + 4);
          buf_move(buf, conn->read_buf, payload_len);
          conn->fragments_len += payload_len;
          conn->needed_bytes = 2;
        } else {
          // place back at the frame start which contains the header & mask
          // we want to get rid of but ensure to subtract by the frame_gap to
          // fill it if it isn't zero
          memmove(frame - total_trimmed, msg, payload_len);
          conn->fragments_len += payload_len;
          total_trimmed += mask_offset + 4;
          conn->needed_bytes = 2;
        }
        if (fin) {
          if (!is_bin(conn) &&
              !utf8_is_valid(buf_peek(conn->read_buf), conn->fragments_len)) {
            ws_conn_destroy(conn);
            buf_reset(s->shared_recv_buffer);
            return; // TODO(sah): send a Close frame, & call close callback
          }
          s->on_ws_msg(conn, buf_peek(conn->read_buf), conn->fragments_len,
                       is_bin(conn));
          buf_consume(conn->read_buf, conn->fragments_len);

          conn->fragments_len = 0;
          clear_fragmented(conn);
          clear_bin(conn);
          conn->needed_bytes = 2;
        }
      } else {
        s->on_ws_msg_fragment(conn, msg, payload_len, fin);
        buf_consume(buf, full_frame_len);
        conn->needed_bytes = 2;
        if (fin) {
          conn->fragments_len = 0;
          conn->needed_bytes = 2;
          clear_fragmented(conn);
          clear_bin(conn);
        }
      }
      break;
    case OP_PING:
      if (payload_len > 125) {
        ws_conn_destroy(conn);
        buf_reset(s->shared_recv_buffer);
        return;
      }
      if (s->on_ws_ping) {
        s->on_ws_ping(conn, msg, payload_len);
      } else {
        // Todo(sah): add some throttling to this
        // a bad client can constantly send pings and we would keep replying
        ws_conn_pong(conn, msg, payload_len);
      }
      if ((conn->fragments_len != 0) & (buf == conn->read_buf)) {
        total_trimmed += full_frame_len;
        conn->needed_bytes = 2;
      } else {
        // printf("here\n");
        buf_consume(buf, full_frame_len);
        conn->needed_bytes = 2;
      }

      break;
    case OP_PONG:
      if (payload_len > 125) {
        ws_conn_destroy(conn);
        buf_reset(s->shared_recv_buffer);
        return;
      }

      if (s->on_ws_pong) {
        // only call the callback if provided
        s->on_ws_pong(conn, msg, payload_len);
      }

      if ((conn->fragments_len != 0) & (buf == conn->read_buf)) {
        total_trimmed += total_trimmed;
        conn->needed_bytes = 2;
      } else {
        buf_consume(buf, full_frame_len);
        conn->needed_bytes = 2;
      }
      break;
    case OP_CLOSE:
      if (!payload_len) {
        if (s->on_ws_close) {
          s->on_ws_close(conn, NULL, 0, WS_CLOSE_NORMAL);
        } else {
          ws_conn_close(conn, NULL, 0, WS_CLOSE_NORMAL);
        }
        buf_reset(s->shared_recv_buffer);
        return;
      } else if (payload_len < 2) {
        if (s->on_ws_close) {
          s->on_ws_close(conn, NULL, 0, WS_CLOSE_PROTOCOL);
        } else {
          ws_conn_close(conn, NULL, 0, WS_CLOSE_PROTOCOL);
        }
        buf_reset(s->shared_recv_buffer);
        return;
      }

      else {
        uint16_t code = WS_CLOSE_NO_STATUS;
        if (payload_len > 125) {
          // close frames can be more but this is the most that will be
          // supported for various reasons close frames generally should
          // contain just the 16bit code and a short string for the reason at
          // most
          ws_conn_destroy(conn);
          buf_reset(s->shared_recv_buffer);
          return;
        }

        if (!utf8_is_valid(msg + 2, payload_len - 2)) {
          ws_conn_destroy(conn);
          buf_reset(s->shared_recv_buffer);
          return;
        };

        code = (msg[0] << 8) | msg[1];
        if (code < 1000 || code == 1004 || code == 1100 || code == 1005 ||
            code == 1006 || code == 1015 || code == 1016 || code == 2000 ||
            code == 2999) {
          if (s->on_ws_close) {
            s->on_ws_close(conn, NULL, 0, code);
          } else {
            ws_conn_close(conn, NULL, 0, WS_CLOSE_PROTOCOL);
          }
          buf_reset(s->shared_recv_buffer);
          return;
        }

        if (s->on_ws_close) {
          s->on_ws_close(conn, msg + 2, payload_len - 2, code);
        } else {
          ws_conn_close(conn, NULL, 0, code);
        }

        buf_reset(s->shared_recv_buffer);
        return;
      }

      break;
    default:
      ws_conn_destroy(conn);
      buf_reset(s->shared_recv_buffer);
      return;
    }
  } /* loop end */

  // if we own the shared buffer drain it right now to allow next conn to
  // reuse it
  if (is_using_shared(conn)) {
    if (conn_drain_write_buf(conn, s->shared_send_buffer) == -1) {
      buf_reset(s->shared_recv_buffer);
      ws_conn_destroy(conn);
    };
    clear_using_shared(conn);
    conn->base->shared_send_buffer_owner = NULL;
  }

clean_up_buffer:
  if (buf != conn->read_buf) {
    // move to connection specific buffer
    if (buf_len(buf)) {
      // printf("moving from shared to socket buffer: %zu\n", buf_len(buf));
      buf_move(buf, conn->read_buf, buf_len(buf));
      set_using_own_recv_buf(conn);
    }
  } else {

    if (buf_len(buf)) {
      memmove(buf->buf + buf->rpos + conn->fragments_len,
              buf->buf + buf->rpos + conn->fragments_len + total_trimmed,
              buf->wpos - buf->rpos + conn->fragments_len + total_trimmed);
      buf->wpos = buf->rpos + conn->fragments_len +
                  (buf->wpos - buf->rpos - conn->fragments_len - total_trimmed);
    } else {
      clear_using_own_recv_buf(conn);
    }
  }
}

static void ws_conn_notify_on_writeable(ws_conn_t *conn) {
  clear_writeable(conn);
  conn->base->ev.data.ptr = conn;
  conn->base->ev.events = EPOLLOUT | EPOLLRDHUP;
  ws_server_epoll_ctl(conn->base, EPOLL_CTL_MOD, conn->fd);
}

static int conn_drain_write_buf(ws_conn_t *conn, buf_t *wbuf) {
  size_t to_write = buf_len(wbuf);
  ssize_t n = 0;

  if ((!to_write)) {
    return 0;
  }

  n = buf_send(wbuf, conn->fd, MSG_NOSIGNAL);
  if ((n == -1 && errno != EAGAIN) | (n == 0)) {
    return -1;
  }

  if (to_write == n) {
    set_writeable(conn);
    return 1;
  } else {
    if (is_using_shared(conn)) {
      // worst case
      clear_using_shared(conn);
      buf_move(conn->base->shared_send_buffer, conn->write_buf,
               buf_len(conn->base->shared_send_buffer));
      conn->base->shared_send_buffer_owner = NULL;
    }
    ws_conn_notify_on_writeable(conn);
  }

  return 0;
}

/**
* returns:
     1 data was completely written

     0 part of the data was written caller should wait for on_drain event to
start sending more data or an error occurred in which the corresponding callback
will be called
*/
inline int ws_conn_pong(ws_conn_t *c, void *msg, size_t n) {
  int stat = conn_write_frame(c, msg, n, OP_PONG);
  if (stat == -1) {
    ws_conn_destroy(c);
  }
  return stat == 1;
}
/**
* returns:
     1 data was completely written

     0 part of the data was written caller should wait for on_drain event to
start sending more data or an error occurred in which the corresponding callback
will be called
*/
inline int ws_conn_ping(ws_conn_t *c, void *msg, size_t n) {
  int stat = conn_write_frame(c, msg, n, OP_PING);
  if (stat == -1) {
    ws_conn_destroy(c);
  }
  return stat == 1;
}

/**
* returns:
     1 data was completely writtena

     0 part of the data was written caller should wait for on_drain event to
start sending more data or an error occurred in which the corresponding callback
will be called
*/
inline int ws_conn_send(ws_conn_t *c, void *msg, size_t n) {
  int stat = conn_write_frame(c, msg, n, OP_BIN);
  if (stat == -1) {
    ws_conn_destroy(c);
  }
  return stat;
}

/**
* returns:
     1 data was completely written

     0 part of the data was written caller should wait for on_drain event to
start sending more data or an error occurred in which the corresponding callback
will be called
*/
inline int ws_conn_send_txt(ws_conn_t *c, void *msg, size_t n) {
  int stat = conn_write_frame(c, msg, n, OP_TXT);
  if (stat == -1) {
    ws_conn_destroy(c);
  }
  return stat == 1;
}

static inline buf_t *conn_choose_send_buf(ws_conn_t *conn, size_t send_len) {
  if (send_len > 65535 || is_using_own_write_buf(conn) || !is_writeable(conn)) {
    set_using_own_write_buf(conn);
    return conn->write_buf;
  } else {
    if (!is_using_shared(conn)) {
      ws_conn_t *owner = conn->base->shared_send_buffer_owner;
      if (owner) {
        if (!is_closing(owner->flags)) {
          if (conn_drain_write_buf(owner, owner->base->shared_send_buffer) ==
              -1) {
            ws_conn_destroy(owner);
          };
        }
        clear_using_shared(owner);
      }
      set_using_shared(conn);
      conn->base->shared_send_buffer_owner = conn;
    }
    return conn->base->shared_send_buffer;
  }
}

void ws_conn_close(ws_conn_t *conn, void *msg, size_t len, uint16_t code) {
  // reason string must be less than 124
  // this isn't a websocket protocol restriction but it will be here for now
  if (len > 124) {
    return;
  }

  // make sure we haven't already done this
  if (is_closing(conn->flags)) {
    return;
  }

  buf_t *wbuf = conn_choose_send_buf(conn, 4 + len);
  uint8_t *buf = buf_peek(wbuf);

  buf[0] = FIN | OP_CLOSE;
  buf[1] = 2 + len;
  buf[2] = (code >> 8) & 0xFF;
  buf[3] = code & 0xFF;
  wbuf->wpos += 4;
  buf_put(wbuf, msg, len);

  conn_drain_write_buf(conn, wbuf);
  ws_conn_destroy(conn);
}

void ws_conn_destroy(ws_conn_t *conn) {
  if (is_closing(conn->flags)) {
    return;
  }
  server_closeable_conns_append(conn);
}

/**
* returns:
     1 data was completely written

     0 part of the data was written, caller should wait for on_drain event to
start sending more data

    -1 an error occurred connection should be closed, check errno
*/

static int conn_write_frame(ws_conn_t *conn, void *data, size_t len,
                            uint8_t op) {

  if (!is_closing(conn->flags)) {
    ws_server_t *s = conn->base;
    size_t hlen = frame_get_header_len(len);
    buf_t *wbuf = conn_choose_send_buf(conn, len);

    size_t flen = len + hlen;
    if (buf_space(wbuf) > flen) {
      // large sends are a bit more complex to handle
      // we attempt to avoid as much copying as possible
      // using vectored I/O

      if (hlen == 4) {
        uint8_t *hbuf =
            wbuf->buf + wbuf->wpos; // place the header in the write buffer
        memset(hbuf, 0, 2);
        hbuf[0] = FIN | op; // Set FIN bit and opcode
        wbuf->wpos += hlen;
        hbuf[1] = PAYLOAD_LEN_16;
        hbuf[2] = (len >> 8) & 0xFF;
        hbuf[3] = len & 0xFF;
        buf_put(wbuf, data, len);
      } else if (hlen == 2) {
        uint8_t *hbuf =
            wbuf->buf + wbuf->wpos; // place the header in the write buffer
        memset(hbuf, 0, 2);
        hbuf[0] = FIN | op; // Set FIN bit and opcode
        wbuf->wpos += hlen;
        hbuf[1] = (uint8_t)len;
        buf_put(wbuf, data, len);
      } else {
        // large send
        uint8_t hbuf[hlen]; // place the header on the stack
        memset(hbuf, 0, 2);
        hbuf[0] = FIN | op; // Set FIN bit and opcode
        hbuf[1] = PAYLOAD_LEN_64;
        hbuf[2] = (len >> 56) & 0xFF;
        hbuf[3] = (len >> 48) & 0xFF;
        hbuf[4] = (len >> 40) & 0xFF;
        hbuf[5] = (len >> 32) & 0xFF;
        hbuf[6] = (len >> 24) & 0xFF;
        hbuf[7] = (len >> 16) & 0xFF;
        hbuf[8] = (len >> 8) & 0xFF;
        hbuf[9] = len & 0xFF;

        if (!is_writeable(conn)) {
          buf_put(wbuf, hbuf, hlen);
          buf_put(wbuf, data, len);
        } else {
          ssize_t n;
          size_t total_write;

          // we have to drain from shared buffer so make it first iovec entry
          if (is_using_shared(conn) && buf_len(s->shared_send_buffer) != 0) {
            assert(buf_len(wbuf) == 0); // TODO: remove
            // here we use 3 iovecs
            // first iovec points to the shared buffer
            // second points to the stack allocated header
            // third points to the payload data
            struct iovec vecs[3];
            vecs[0].iov_len = buf_len(s->shared_send_buffer);
            vecs[0].iov_base =
                s->shared_send_buffer->buf + s->shared_send_buffer->rpos;
            vecs[1].iov_base = hbuf;
            vecs[1].iov_len = hlen;
            vecs[2].iov_base = data;
            vecs[2].iov_len = len;
            total_write = flen + vecs[0].iov_len;

            // send of as much as we can and place the rest in the connection
            // buffer draining the shared buffer and also moving leftover data
            // there into the connection buffer if any
            n = buf_drain_write2v(s->shared_send_buffer, vecs, total_write,
                                  wbuf, conn->fd);

          }
          // no writes need to go ahead
          else if (!buf_len(wbuf)) {
            // here only two iovecs are needed
            // stack allocated header and the payload data
            struct iovec vecs[2];
            vecs[0].iov_base = hbuf;
            vecs[0].iov_len = hlen;
            vecs[1].iov_base = data;
            vecs[1].iov_len = len;
            total_write = flen;
            // write as much as possible and only copy from payload data what
            // couldn't be drained
            n = buf_write2v(wbuf, conn->fd, vecs, flen);
          }

          else {
            // here there is data pending in the connection send buffer
            // first iovec points to the connection buffer
            // second points to the stack allocated header
            // third points to the payload data
            struct iovec vecs[3];
            vecs[0].iov_len = buf_len(wbuf);
            vecs[0].iov_base = wbuf->buf + wbuf->rpos;
            vecs[1].iov_base = hbuf;
            vecs[1].iov_len = hlen;
            vecs[2].iov_base = data;
            vecs[2].iov_len = len;
            total_write = flen + vecs[0].iov_len;

            // send of as much as we can and place the rest in the connection
            // buffer
            n = buf_drain_write2v(wbuf, vecs, total_write, NULL, conn->fd);
          }

          if (n == total_write) {

            if (is_using_own_write_buf(conn)) {
              clear_using_own_write_buf(conn);
            }

            return 1;
          } else if (n == 0 ||
                     ((n == -1) & ((errno != EAGAIN) | (errno != EINTR)))) {
            return -1;
          } else {
            if (is_using_shared(conn)) {
              clear_using_shared(conn);
              set_using_own_write_buf(conn);
              conn->base->shared_send_buffer_owner = NULL;
            }
            ws_conn_notify_on_writeable(conn);
            return 1;
          }
        }
      }

      // queue up for writing if not using shared buffer
      if (wbuf == conn->write_buf) {
        // queue it up for writing
        server_writeable_conns_append(conn);
      }

      return 1;
    }
  }

  return 0;
}

int ws_conn_fd(ws_conn_t *c) { return c->fd; }

inline ws_server_t *ws_conn_server(ws_conn_t *c) { return c->base; }

inline void *ws_conn_ctx(ws_conn_t *c) { return c->ctx; }

inline void ws_conn_set_ctx(ws_conn_t *c, void *ctx) { c->ctx = ctx; }

inline bool ws_server_accept_paused(ws_server_t *s) { return s->accept_paused; }

inline bool ws_conn_msg_bin(ws_conn_t *c) { return is_bin(c); }

inline size_t ws_server_open_conns(ws_server_t *s) { return s->open_conns; }

int utf8_is_valid(uint8_t *s, size_t n) {
  for (uint8_t *e = s + n; s != e;) {
    if (s + 4 <= e) {
      uint32_t tmp;
      memcpy(&tmp, s, 4);
      if ((tmp & 0x80808080) == 0) {
        s += 4;
        continue;
      }
    }

    while (!(*s & 0x80)) {
      if (++s == e) {
        return 1;
      }
    }

    if ((s[0] & 0x60) == 0x40) {
      if (s + 1 >= e || (s[1] & 0xc0) != 0x80 || (s[0] & 0xfe) == 0xc0) {
        return 0;
      }
      s += 2;
    } else if ((s[0] & 0xf0) == 0xe0) {
      if (s + 2 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 ||
          (s[0] == 0xe0 && (s[1] & 0xe0) == 0x80) ||
          (s[0] == 0xed && (s[1] & 0xe0) == 0xa0)) {
        return 0;
      }
      s += 3;
    } else if ((s[0] & 0xf8) == 0xf0) {
      if (s + 3 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 ||
          (s[3] & 0xc0) != 0x80 || (s[0] == 0xf0 && (s[1] & 0xf0) == 0x80) ||
          (s[0] == 0xf4 && s[1] > 0x8f) || s[0] > 0xf4) {
        return 0;
      }
      s += 4;
    } else {
      return 0;
    }
  }
  return 1;
}

struct ws_conn_pool *ws_conn_pool_create(size_t nmemb) {
  size_t conns_size = nmemb * sizeof(ws_conn_t);
  size_t pool_sz = (sizeof(struct ws_conn_pool) +
                    (nmemb * sizeof(struct buf_node)) + 64 - 1) &
                   ~(64 - 1);

  struct ws_conn_pool *pool;

  assert(posix_memalign((void **)&pool, 64, conns_size + pool_sz) == 0);

  memset(pool, 0, conns_size + pool_sz);

  uintptr_t base_ptr = (uintptr_t)pool + pool_sz;

  pool->base = (ws_conn_t *)base_ptr;

  ws_conn_t *cur = pool->base;

  for (size_t i = 0; i < nmemb; ++i) {
    pool->_buf_nodes[i].b = cur;
    cur += 1;
  }

  for (size_t i = 0; i < nmemb - 1; i++) {
    pool->_buf_nodes[i].next = &pool->_buf_nodes[i + 1];
  }

  pool->_buf_nodes[nmemb - 1].next = NULL;

  pool->head = &pool->_buf_nodes[0];

  return pool;
}

struct ws_conn_t *ws_conn_alloc(struct ws_conn_pool *p) {
  if (p->head) {
    struct buf_node *bn = p->head;
    p->head = p->head->next;
    bn->next = NULL; // unlink the buf_node mainly useful for debugging
    return bn->b;
  } else {
    return NULL;
  }
}

void ws_conn_free(struct ws_conn_pool *p, struct ws_conn_t *c) {
  uintptr_t diff =
      ((uintptr_t)c - (uintptr_t)p->base) / sizeof(struct ws_conn_t);

  memset(c, 0, sizeof(struct ws_conn_t));

  p->_buf_nodes[diff].next = p->head;
  p->head = &p->_buf_nodes[diff];
}

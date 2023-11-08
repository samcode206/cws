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
#include "buf.h"
#include "pool.h"
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
#include <sys/uio.h>

#define FIN 0x80

#define OP_CONT 0x0
#define OP_TXT 0x1
#define OP_BIN 0x2
#define OP_PING 0x9
#define OP_PONG 0xA
#define OP_CLOSE 0x8

#define PAYLOAD_LEN_16 126
#define PAYLOAD_LEN_64 127

typedef int (*ws_handler)(ws_server_t *s, ws_conn_t *conn);

typedef struct {
  int fd;            // socket fd
  bool bin;          // is binary message?
  bool upgraded;     // are we even upgraded?
  bool close_queued; // we are in the close list and should not attempt to do
                     // any IO on this connection, it will soon be closed and
                     // all resources will be recycled/freed

  bool fragmented;      // are we handling a fragmented msg?
  size_t fragments_len; // size of the data portion of the frames across
                        // fragmentation

  size_t needed_bytes; // bytes needed before we can do something with the frame
  buf_t read_buf;      // recv buffer structure
  ws_server_t *base;   // server ptr
} read_state_t;

typedef struct {
  int fd;            // socket fd
  bool writeable;    // can we write to the socket now?
  bool write_queued; // are we queued for writing?
  bool close_queued; // are we getting removed/closed
  bool using_shared; // are we using the shared send buffer
  buf_t write_buf;   // write buffer structure
  ws_server_t *base; // server ptr
} write_state_t;

struct ws_conn_t {
  read_state_t rx_state;
  write_state_t tx_state;
  void *ctx;
};

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
  buf_t shared_recv_buffer;
  ws_msg_fragment_cb_t on_ws_msg_fragment;
  ws_conn_t *shared_send_buffer_owner;

  buf_t shared_send_buffer;
  ws_open_cb_t on_ws_open;
  size_t open_conns; // open websocket connections
  size_t max_conns;  // max connections allowed
  ws_accept_cb_t on_ws_accept;

  ws_on_upgrade_req_cb_t on_ws_upgrade_req;
  ws_ping_cb_t on_ws_ping;
  ws_pong_cb_t on_ws_pong;
  ws_drain_cb_t on_ws_drain;
  ws_close_cb_t on_ws_close;
  ws_disconnect_cb_t on_ws_disconnect;
  ws_err_cb_t on_ws_err;
  ws_err_accept_cb_t on_ws_accept_err;
  
  struct buf_pool *buffer_pool;
  int fd; // server file descriptor
  int epoll_fd;
  bool accept_paused; // are we paused on accepting new connections
  struct epoll_event ev;
  struct epoll_event events[1024];
  struct conn_list writeable_conns;
  struct conn_list closeable_conns;
} ws_server_t;

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

static void handle_http(ws_conn_t *conn);
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
  if (c->tx_state.writeable && !c->tx_state.write_queued) {
    conn_list_append(&c->tx_state.base->writeable_conns, c);
    c->tx_state.write_queued = true;
  }
}

static void server_closeable_conns_append(ws_conn_t *c) {
  c->tx_state.base->ev.data.ptr = c;
  ws_server_epoll_ctl(c->tx_state.base, EPOLL_CTL_DEL, c->tx_state.fd);
  conn_list_append(&c->tx_state.base->closeable_conns, c);
  c->tx_state.close_queued = true;
  c->rx_state.close_queued = true;
}

static void server_writeable_conns_drain(ws_server_t *s) {
  if (s->shared_send_buffer_owner) {
    ws_conn_t *c = s->shared_send_buffer_owner;
    if (!c->tx_state.close_queued &&
        conn_drain_write_buf(c, &s->shared_send_buffer) == -1) {
      server_closeable_conns_append(c);
    };

    c->tx_state.using_shared = false;
    s->shared_send_buffer_owner = NULL;
  }
  if (s->writeable_conns.len) {
    printf("draining %zu connections\n", s->writeable_conns.len);
  }

  for (size_t i = 0; i < s->writeable_conns.len; ++i) {
    ws_conn_t *c = s->writeable_conns.conns[i];
    if (!c->tx_state.close_queued) {
      if (conn_drain_write_buf(c, &c->tx_state.write_buf) == -1) {
        server_closeable_conns_append(c);
      };
      c->tx_state.write_queued = false;
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
      assert(close(c->rx_state.fd) == 0);
      s->on_ws_disconnect(c, 0);
      buf_pool_free(s->buffer_pool, c->tx_state.write_buf.buf);
      buf_pool_free(s->buffer_pool, c->rx_state.read_buf.buf);

      if (s->shared_send_buffer_owner == c) {
        s->shared_send_buffer_owner = NULL;
      }
      free(c);
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
  s->buffer_pool = buf_pool_init(s->max_conns + s->max_conns + 2, buffer_size);
  s->max_msg_len = max_backpressure;
  buf_init(s->buffer_pool, &s->shared_recv_buffer);
  buf_init(s->buffer_pool, &s->shared_send_buffer);

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

static void ws_server_conns_establish(ws_server_t *s, int fd,
                                      struct sockaddr *sockaddr,
                                      socklen_t *socklen) {
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

        ws_conn_t *conn = calloc(1, sizeof(ws_conn_t));
        assert(conn != NULL); // TODO(sah): remove this
        s->ev.events = EPOLLIN | EPOLLRDHUP;
        conn->rx_state.fd = client_fd;
        conn->rx_state.base = s;
        conn->rx_state.needed_bytes = 2;

        conn->tx_state.writeable = 1;
        conn->tx_state.fd = client_fd;
        conn->tx_state.base = s;

        s->ev.data.ptr = conn;

        assert(buf_init(s->buffer_pool, &conn->rx_state.read_buf) == 0);
        assert(buf_init(s->buffer_pool, &conn->tx_state.write_buf) == 0);

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
  int fd = s->fd;
  int epfd = s->epoll_fd;

  struct sockaddr_storage client_sockaddr;
  socklen_t client_socklen;
  client_socklen = sizeof client_sockaddr;
  s->ev.data.ptr = s;
  s->ev.events = EPOLLIN;

  ws_server_epoll_ctl(s, EPOLL_CTL_ADD, fd);

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
      if (s->events[i].data.ptr == s) {
        ws_server_conns_establish(s, fd, (struct sockaddr *)&client_sockaddr,
                                  &client_socklen);
      } else {

        if (s->events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
          ws_conn_destroy(s->events[i].data.ptr);
        }

        if (s->events[i].events & EPOLLOUT) {
          ws_conn_t *c = s->events[i].data.ptr;
          if (!c->tx_state.close_queued) {

            int ret = conn_drain_write_buf(c, &c->tx_state.write_buf);
            if (ret == 1) {
              ws_conn_t *c = s->events[i].data.ptr;
              if (!c->rx_state.upgraded) {
                c->rx_state.upgraded = 1;
                c->rx_state.needed_bytes = 2;
                s->on_ws_open(c);
              } else {
                if (s->on_ws_drain) {
                  s->on_ws_drain(c);
                }
              }
              s->ev.data.ptr = c;
              s->ev.events = EPOLLIN | EPOLLRDHUP;
              if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->tx_state.fd, &s->ev) ==
                  -1) {
                if (s->on_ws_err) {
                  int err = errno;
                  s->on_ws_err(s, err);
                } else {
                  perror("epoll_ctl");
                  exit(EXIT_FAILURE);
                }
              };
            } else if (ret == -1) {
              if (!c->tx_state.close_queued) {
                server_closeable_conns_append(c);
              }
            }
          }
        }
        if (s->events[i].events & EPOLLIN) {
          ws_conn_t *c = s->events[i].data.ptr;
          if (!c->rx_state.close_queued) {
            if (!c->rx_state.upgraded) {
              handle_http(c);
            } else {
              ws_conn_handle(c);
            }

            assert(buf_len(&s->shared_recv_buffer) == 0);
          }
        }
      }
    }

    server_writeable_conns_drain(s); // drain writes

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
  ssize_t n = buf_recv(buf, conn->rx_state.fd, 0);
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

static inline int get_header(const char *headers, const char *key, char *val,
                             size_t n) {
  const char *header_start = strstr(headers, key);
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

static void handle_http(ws_conn_t *conn) {
  ws_server_t *s = conn->rx_state.base;
  buf_t *request_buf;
  buf_t *response_buf = NULL;
  size_t resp_len = 0;

  if (buf_len(&conn->rx_state.read_buf)) {
    request_buf = &conn->rx_state.read_buf;
  } else {
    request_buf = &s->shared_recv_buffer;
  }

  if (conn_read(conn, request_buf) == -1) {
    ws_conn_destroy(conn);
    buf_reset(&s->shared_recv_buffer);
    return;
  };

  uint8_t *headers = buf_peek(request_buf);
  size_t request_buf_len = buf_len(request_buf);

  headers[request_buf_len] = '\0';

  if (!strncmp((char *)headers, GET_RQ, GET_RQ_LEN)) {
    char sec_websocket_key[25] = {0};
    int ret = get_header((char *)headers, SEC_WS_KEY_HDR, sec_websocket_key,
                         sizeof sec_websocket_key);
    if (ret < 0) {
      printf("error parsing http headers: %d\n", ret);
      return;
    }

    char accept_key[32];
    int accept_key_len =
        ws_derive_accept_hdr(sec_websocket_key, accept_key, ret - 1) - 1;

    if (!s->on_ws_upgrade_req) {
      response_buf = request_buf;
      buf_reset(response_buf);
      buf_put(response_buf, switching_protocols, SWITCHING_PROTOCOLS_HDRS_LEN);
      buf_put(response_buf, accept_key, accept_key_len);
      buf_put(response_buf, CRLF2, CRLF2_LEN);
      resp_len = buf_len(response_buf);
    } else {

      if (!buf_len(&conn->tx_state.write_buf) &&
          s->shared_send_buffer_owner == NULL) {
        s->shared_send_buffer_owner = conn;
        conn->tx_state.using_shared = true;
        response_buf = &s->shared_send_buffer;
      } else {
        response_buf = &conn->tx_state.write_buf;
      }

      size_t max_resp_len = buf_space(response_buf);
      resp_len =
          s->on_ws_upgrade_req(conn, (char *)headers, accept_key, max_resp_len,
                               (char *)buf_peek(response_buf));

      buf_consume(request_buf, request_buf_len);

      if ((resp_len > 0) & (resp_len <= max_resp_len)) {
        response_buf->wpos += resp_len;
      } else {
        resp_len = 0;
      }
    }

    if (response_buf) {
      if (resp_len) {

        int ret = conn_drain_write_buf(conn, response_buf);

        if (ret == 1) {
          s->on_ws_open(conn);
          conn->rx_state.upgraded = 1;
          conn->rx_state.needed_bytes = 2;
        } else if (ret == -1) {
          if (!conn->tx_state.close_queued) {
            server_closeable_conns_append(conn);
          }
        }
      } else {
        // todo(sah): as printf suggests and maybe a 500 status code
        printf("should drop connection\n");
      }
    }

    return;
  }
}

static inline buf_t *ws_conn_choose_read_buf(ws_conn_t *conn) {

  if ((buf_len(&conn->rx_state.read_buf) != 0) &
      !(conn->rx_state.fragments_len + conn->rx_state.read_buf.rpos ==
        conn->rx_state.read_buf.wpos)) {
    // buf_debug(&conn->read_buf, "conn buffer chosen");

    return &conn->rx_state.read_buf;
  } else {
    return &conn->rx_state.base->shared_recv_buffer;
  }
}

static size_t ws_conn_readable_len(ws_conn_t *conn, buf_t *buf) {
  if (buf != &conn->rx_state.read_buf) {
    return buf->wpos - buf->rpos;
  } else {
    return buf->wpos - buf->rpos - conn->rx_state.fragments_len;
  }
}

static inline void ws_conn_handle(ws_conn_t *conn) {
  buf_t *buf = ws_conn_choose_read_buf(conn);
  ws_server_t *s = conn->rx_state.base;

  // total frame header bytes trimmed
  size_t total_trimmed = 0;
  size_t max_allowed_len = s->max_msg_len;

  if (conn_read(conn, buf) == -1) {
    ws_conn_destroy(conn);
    buf_reset(&s->shared_recv_buffer);
    return;
  }

  while (ws_conn_readable_len(conn, buf) - total_trimmed >=
         conn->rx_state.needed_bytes) {
    // payload start
    uint8_t *frame = buf_peek_at(
        buf, buf->rpos + ((buf == &conn->rx_state.read_buf) *
                          (conn->rx_state.fragments_len + total_trimmed)));

    uint8_t fin = frame_get_fin(frame);
    uint8_t opcode = frame_get_opcode(frame);
    // printf("fragments=%zu\n", conn->state.fragments_len);
    // run general validation checks on the header
    if (((fin == 0) & ((opcode > 2) & (opcode != OP_CONT))) |
        (frame_has_reserved_bits_set(frame) == 1) |
        (frame_is_masked(frame) == 0)) {
      buf_reset(&s->shared_recv_buffer);
      ws_conn_destroy(conn);
      return;
    }

    // make sure we can get the full msg
    size_t payload_len = 0;
    size_t frame_buf_len = buf_len(buf);
    if (&conn->rx_state.read_buf == buf) {
      frame_buf_len =
          frame_buf_len - conn->rx_state.fragments_len - total_trimmed;
    }
    // check if we need to do more reads to get the msg length
    int missing_header_len =
        frame_decode_payload_len(frame, frame_buf_len, &payload_len);
    if (missing_header_len) {
      // wait for atleast remaining of the header
      conn->rx_state.needed_bytes = missing_header_len;
      goto clean_up_buffer;
    }

    size_t mask_offset = frame_get_header_len(payload_len);
    size_t full_frame_len = payload_len + 4 + mask_offset;

    // validate frame length
    if (payload_len > max_allowed_len) {
      // drop the connection
      ws_conn_close(conn, NULL, 0, WS_CLOSE_TOO_LARGE);
      buf_reset(&s->shared_recv_buffer);
      return;
    }

    // check that we have atleast the whole frame, otherwise
    // set needed_bytes and exit waiting for more reads from the socket
    if (frame_buf_len < full_frame_len) {
      conn->rx_state.needed_bytes = full_frame_len;
      goto clean_up_buffer;
    }

    uint8_t *msg = frame + mask_offset + 4;
    msg_unmask(msg, frame + mask_offset, payload_len);
    // printf("buf_len=%zu frame_len=%zu opcode=%d fin=%d\n",
    // frame_buf_len,
    //        full_frame_len, opcode, fin);

    switch (opcode) {
    case OP_TXT:
    case OP_BIN:
      conn->rx_state.bin = opcode == OP_BIN;
      // fin and never fragmented
      // this handles both text and binary hence the fallthrough
      if (fin & (!conn->rx_state.fragmented)) {
        if (!conn->rx_state.bin && !utf8_is_valid(msg, payload_len)) {
          ws_conn_destroy(conn);
          buf_reset(&s->shared_recv_buffer);
          return; // TODO(sah): send a Close frame, & call close callback
        }
        s->on_ws_msg(conn, msg, payload_len, conn->rx_state.bin);
        buf_consume(buf, full_frame_len);
        conn->rx_state.bin = 0;
        conn->rx_state.needed_bytes = 2;

        break; /* OP_BIN don't fall through to fragmented msg */
      } else if (fin & (conn->rx_state.fragmented)) {
        // this is invalid because we expect continuation not text or binary
        // opcode
        ws_conn_destroy(conn);
        buf_reset(&s->shared_recv_buffer);
        return;
      }

    case OP_CONT:
      // accumulate bytes and increase fragments_len

      // move bytes over
      // call the callback
      // reset
      // can't send cont as first fragment
      if ((opcode == OP_CONT) & (!conn->rx_state.fragmented)) {
        ws_conn_destroy(conn);
        buf_reset(&s->shared_recv_buffer);
        return;
      }

      if (conn->rx_state.fragments_len + payload_len > max_allowed_len) {
        ws_conn_close(conn, NULL, 0, WS_CLOSE_TOO_LARGE);
        buf_reset(&s->shared_recv_buffer);
        return;
      }

      // set the state to fragmented after validation
      conn->rx_state.fragmented = 1;

      if (!s->on_ws_msg_fragment) {

        // we are using the shared buffer
        if (buf != &conn->rx_state.read_buf) {
          // trim off the header
          buf_consume(buf, mask_offset + 4);
          buf_move(buf, &conn->rx_state.read_buf, payload_len);
          conn->rx_state.fragments_len += payload_len;
          conn->rx_state.needed_bytes = 2;
        } else {
          // place back at the frame start which contains the header & mask
          // we want to get rid of but ensure to subtract by the frame_gap to
          // fill it if it isn't zero
          memmove(frame - total_trimmed, msg, payload_len);
          conn->rx_state.fragments_len += payload_len;
          total_trimmed += mask_offset + 4;
          conn->rx_state.needed_bytes = 2;
        }
        if (fin) {
          if (!conn->rx_state.bin &&
              !utf8_is_valid(buf_peek(&conn->rx_state.read_buf),
                             conn->rx_state.fragments_len)) {
            ws_conn_destroy(conn);
            buf_reset(&s->shared_recv_buffer);
            return; // TODO(sah): send a Close frame, & call close callback
          }
          s->on_ws_msg(conn, buf_peek(&conn->rx_state.read_buf),
                       conn->rx_state.fragments_len, conn->rx_state.bin);
          buf_consume(&conn->rx_state.read_buf, conn->rx_state.fragments_len);

          conn->rx_state.fragments_len = 0;
          conn->rx_state.fragmented = 0;
          conn->rx_state.needed_bytes = 2;
          conn->rx_state.bin = 0;
        }
      } else {
        s->on_ws_msg_fragment(conn, msg, payload_len, fin);
        buf_consume(buf, full_frame_len);
        conn->rx_state.needed_bytes = 2;
        if (fin) {
          conn->rx_state.fragments_len = 0;
          conn->rx_state.needed_bytes = 2;
          conn->rx_state.bin = 0;
          conn->rx_state.fragmented = 0;
        }
      }
      break;
    case OP_PING:
      if (payload_len > 125) {
        ws_conn_destroy(conn);
        buf_reset(&s->shared_recv_buffer);
        return;
      }
      if (s->on_ws_ping) {
        s->on_ws_ping(conn, msg, payload_len);
      } else {
        // Todo(sah): add some throttling to this
        // a bad client can constantly send pings and we would keep replying
        ws_conn_pong(conn, msg, payload_len);
      }
      if ((conn->rx_state.fragments_len != 0) &
          (buf == &conn->rx_state.read_buf)) {
        total_trimmed += full_frame_len;
        conn->rx_state.needed_bytes = 2;
      } else {
        // printf("here\n");
        buf_consume(buf, full_frame_len);
        conn->rx_state.needed_bytes = 2;
      }

      break;
    case OP_PONG:
      if (payload_len > 125) {
        ws_conn_destroy(conn);
        buf_reset(&s->shared_recv_buffer);
        return;
      }

      if (s->on_ws_pong) {
        // only call the callback if provided
        s->on_ws_pong(conn, msg, payload_len);
      }

      if ((conn->rx_state.fragments_len != 0) &
          (buf == &conn->rx_state.read_buf)) {
        total_trimmed += total_trimmed;
        conn->rx_state.needed_bytes = 2;
      } else {
        buf_consume(buf, full_frame_len);
        conn->rx_state.needed_bytes = 2;
      }
      break;
    case OP_CLOSE:
      if (!payload_len) {
        if (s->on_ws_close) {
          s->on_ws_close(conn, NULL, 0, WS_CLOSE_NORMAL);
        } else {
          ws_conn_close(conn, NULL, 0, WS_CLOSE_NORMAL);
        }
        buf_reset(&s->shared_recv_buffer);
        return;
      } else if (payload_len < 2) {
        if (s->on_ws_close) {
          s->on_ws_close(conn, NULL, 0, WS_CLOSE_PROTOCOL);
        } else {
          ws_conn_close(conn, NULL, 0, WS_CLOSE_PROTOCOL);
        }
        buf_reset(&s->shared_recv_buffer);
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
          buf_reset(&s->shared_recv_buffer);
          return;
        }

        if (!utf8_is_valid(msg + 2, payload_len - 2)) {
          ws_conn_destroy(conn);
          buf_reset(&s->shared_recv_buffer);
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
          buf_reset(&s->shared_recv_buffer);
          return;
        }

        if (s->on_ws_close) {
          s->on_ws_close(conn, msg + 2, payload_len - 2, code);
        } else {
          ws_conn_close(conn, NULL, 0, code);
        }

        buf_reset(&s->shared_recv_buffer);
        return;
      }

      break;
    default:
      ws_conn_destroy(conn);
      buf_reset(&s->shared_recv_buffer);
      return;
    }
  } /* loop end */

  // if we own the shared buffer drain it right now to allow next conn to
  // reuse it
  if (conn->tx_state.using_shared) {
    if (conn_drain_write_buf(conn, &s->shared_send_buffer) == -1) {
      if (!conn->tx_state.close_queued) {
        buf_reset(&s->shared_recv_buffer);
        server_closeable_conns_append(conn);
      }
    };
    conn->tx_state.using_shared = false;
    conn->tx_state.base->shared_send_buffer_owner = NULL;
  }

clean_up_buffer:
  if ((buf == &s->shared_recv_buffer) && (buf_len(buf) > 0)) {
    // move to connection specific buffer
    // printf("moving from shared to socket buffer: %zu\n", buf_len(buf));
    buf_move(buf, &conn->rx_state.read_buf, buf_len(buf));
  } else {

    memmove(buf->buf + buf->rpos + conn->rx_state.fragments_len,
            buf->buf + buf->rpos + conn->rx_state.fragments_len + total_trimmed,
            buf->wpos - buf->rpos + conn->rx_state.fragments_len +
                total_trimmed);
    buf->wpos =
        buf->rpos + conn->rx_state.fragments_len +
        (buf->wpos - buf->rpos - conn->rx_state.fragments_len - total_trimmed);
  }
}

static void ws_conn_notify_on_writeable(ws_conn_t *conn) {
  conn->tx_state.writeable = 0;
  conn->tx_state.base->ev.data.ptr = conn;
  conn->tx_state.base->ev.events = EPOLLOUT | EPOLLRDHUP;
  ws_server_epoll_ctl(conn->tx_state.base, EPOLL_CTL_MOD, conn->tx_state.fd);
}

static int conn_drain_write_buf(ws_conn_t *conn, buf_t *wbuf) {
  size_t to_write = buf_len(wbuf);
  ssize_t n = 0;

  if ((!to_write)) {
    return 0;
  }

  n = buf_send(wbuf, conn->tx_state.fd, 0);
  if ((n == -1 && errno != EAGAIN) | (n == 0)) {
    return -1;
  }

  if (to_write == n) {
    conn->tx_state.writeable = 1;
    return 1;
  } else {
    if (conn->tx_state.using_shared) {
      // worst case
      conn->tx_state.using_shared = false;
      buf_move(&conn->tx_state.base->shared_send_buffer,
               &conn->tx_state.write_buf,
               buf_len(&conn->tx_state.base->shared_send_buffer));
      conn->tx_state.base->shared_send_buffer_owner = NULL;
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
    if (!c->tx_state.close_queued) {
      server_closeable_conns_append(c);
    }
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
    if (!c->tx_state.close_queued) {
      server_closeable_conns_append(c);
    }
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
inline int ws_conn_send(ws_conn_t *c, void *msg, size_t n) {
  int stat = conn_write_frame(c, msg, n, OP_BIN);
  if (stat == -1) {
    if (!c->tx_state.close_queued) {
      server_closeable_conns_append(c);
    }
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
inline int ws_conn_send_txt(ws_conn_t *c, void *msg, size_t n) {
  int stat = conn_write_frame(c, msg, n, OP_TXT);
  if (stat == -1) {
    if (!c->tx_state.close_queued) {
      server_closeable_conns_append(c);
    }
  }
  return stat == 1;
}

void ws_conn_close(ws_conn_t *conn, void *msg, size_t len, uint16_t code) {
  // reason string must be less than 124
  // this isn't a websocket protocol restriction but it will be here for now
  if (len > 124) {
    return;
  }

  // make sure we haven't already done this
  if (conn->tx_state.close_queued) {
    return;
  }

  // if we can say good bye let's do that
  // not if a partial write is in progress this will
  // lead to us sending garbage as the other side is decoding the frames
  // because we will have interleaved data sent...
  // todo(Sah): solve for the above
  if (conn->tx_state.writeable) {
    // reason msg was provided
    struct iovec iovs[2];
    if (len) {
      uint8_t buf1[4] = {0, 0};
      buf1[0] = FIN | OP_CLOSE;
      buf1[1] = len + 2;
      buf1[2] = (code >> 8) & 0xFF;
      buf1[3] = code & 0xFF;
      iovs[0].iov_len = 4;
      iovs[0].iov_base = buf1;
      iovs[1].iov_len = len;
      iovs[1].iov_base = msg;
      writev(conn->tx_state.fd, iovs, 2);
    } else {
      uint8_t buf[4] = {0, 0};
      buf[0] = FIN | OP_CLOSE;
      buf[1] = 2;
      buf[2] = (code >> 8) & 0xFF;
      buf[3] = code & 0xFF;
      send(conn->tx_state.fd, buf, 4, MSG_NOSIGNAL);
    }
  }

  // prepare the connection to be close and queue it up in the close queue
  server_closeable_conns_append(conn);
}

void ws_conn_destroy(ws_conn_t *conn) {
  if (conn->rx_state.close_queued) {
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

static inline buf_t *conn_choose_send_buf(ws_conn_t *conn, size_t send_len) {
  if (send_len > 65535 || buf_len(&conn->tx_state.write_buf) != 0 ||
      !conn->tx_state.writeable) {
    return &conn->tx_state.write_buf;
  } else {
    if (!conn->tx_state.using_shared) {
      ws_conn_t *owner = conn->tx_state.base->shared_send_buffer_owner;
      if (owner) {
        if (!owner->tx_state.close_queued) {
          if (conn_drain_write_buf(
                  owner, &owner->tx_state.base->shared_send_buffer) == -1) {
            server_closeable_conns_append(owner);
          };
        }
        owner->tx_state.using_shared = false;
      }
      conn->tx_state.using_shared = true;
      conn->tx_state.base->shared_send_buffer_owner = conn;
    }
    return &conn->tx_state.base->shared_send_buffer;
  }
}

static int conn_write_frame(ws_conn_t *conn, void *data, size_t len,
                            uint8_t op) {

  if (!conn->tx_state.close_queued) {
    ws_server_t *s = conn->tx_state.base;
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

        if (!conn->tx_state.writeable) {
          buf_put(wbuf, hbuf, hlen);
          buf_put(wbuf, data, len);
        } else {
          ssize_t n;
          size_t total_write;

          // we have to drain from shared buffer so make it first iovec entry
          if (conn->tx_state.using_shared &&
              buf_len(&s->shared_send_buffer) != 0) {
            assert(buf_len(wbuf) == 0); // TODO: remove
            // here we use 3 iovecs
            // first iovec points to the shared buffer
            // second points to the stack allocated header
            // third points to the payload data
            struct iovec vecs[3];
            vecs[0].iov_len = buf_len(&s->shared_send_buffer);
            vecs[0].iov_base =
                s->shared_send_buffer.buf + s->shared_send_buffer.rpos;
            vecs[1].iov_base = hbuf;
            vecs[1].iov_len = hlen;
            vecs[2].iov_base = data;
            vecs[2].iov_len = len;
            total_write = flen + vecs[0].iov_len;

            // send of as much as we can and place the rest in the connection
            // buffer draining the shared buffer and also moving leftover data
            // there into the connection buffer if any
            n = buf_drain_write2v(&s->shared_send_buffer, vecs, total_write,
                                  wbuf, conn->tx_state.fd);

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
            n = buf_write2v(wbuf, conn->tx_state.fd, vecs, flen);
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
            n = buf_drain_write2v(wbuf, vecs, total_write, NULL,
                                  conn->tx_state.fd);
          }

          if (n == total_write) {
            return 1;
          } else if (n == 0 ||
                     ((n == -1) & ((errno != EAGAIN) | (errno != EINTR)))) {
            return -1;
          } else {
            if (conn->tx_state.using_shared) {
              conn->tx_state.using_shared = false;
              conn->tx_state.base->shared_send_buffer_owner = NULL;
            }
            ws_conn_notify_on_writeable(conn);
            return 1;
          }
        }
      }

      // queue up for writing if not using shared buffer
      if (wbuf != &s->shared_send_buffer) {
        // queue it up for writing
        server_writeable_conns_append(conn);
      }

      return 1;
    }
  }

  return 0;
}

int ws_conn_fd(ws_conn_t *c) { return c->rx_state.fd; }

inline ws_server_t *ws_conn_server(ws_conn_t *c) { return c->rx_state.base; }

inline void *ws_conn_ctx(ws_conn_t *c) { return c->ctx; }

inline void ws_conn_set_ctx(ws_conn_t *c, void *ctx) { c->ctx = ctx; }

inline bool ws_server_accept_paused(ws_server_t *s) { return s->accept_paused; }

inline bool ws_conn_msg_bin(ws_conn_t *c) { return c->rx_state.bin; }

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

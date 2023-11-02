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



typedef int (*ws_handler)(ws_server_t *s, struct ws_conn_t *conn);

typedef struct {
  bool bin;       // is binary message?
  bool writeable; // can we write to the socket?
  bool upgraded;  // are we even upgraded?

  bool write_queued; // do we currently have some data that is queued for
                     // writing (in s->writeable_conns)

  bool close_queued; // we are in the close list and should not attempt to do
                     // any IO on this connection, it will soon be closed and
                     // all resources will be recycled/freed

  size_t fragments_len; // size of the data portion of the frames across
                        // fragmentation

  size_t needed_bytes; // bytes needed before we can do something with the frame
} ws_state_t;

struct ws_conn_t {
  int fd;
  ws_state_t state;
  ws_server_t *base;
  void *ctx; // user data ptr
  buf_t read_buf;
  buf_t write_buf;
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
  int fd;             // server file descriptor
  int resv;           // unused
  size_t open_conns;  // open websocket connections
  size_t max_conns;   // max connections allowed
  size_t max_msg_len; // max allowed msg length
  struct buf_pool *buffer_pool;
  ws_open_cb_t on_ws_open;
  ws_msg_cb_t on_ws_msg;
  ws_ping_cb_t on_ws_ping;
  ws_pong_cb_t on_ws_pong;
  ws_drain_cb_t on_ws_drain;
  ws_close_cb_t on_ws_close;
  ws_disconnect_cb_t on_ws_disconnect;
  ws_err_cb_t on_ws_err;
  int epoll_fd;
  int shared_send_buffer_owner;
  buf_t shared_recv_buffer;
  buf_t shared_send_buffer;
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

int frame_decode_payload_len(uint8_t *buf, size_t rbuf_len, size_t *res) {
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

// HTTP & Handshake Utils
#define WS_VERSION 13

#define SPACE 0x20
#define CRLF "\r\n"
#define CRLF2 "\r\n\r\n"

#define GET_RQ "GET"
#define SEC_WS_KEY_HDR "Sec-WebSocket-Key"

#define SWITCHING_PROTOCOLS                                                    \
  "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "     \
  "Upgrade\r\nServer: cws\r\nSec-WebSocket-Accept: "
#define SWITCHING_PROTOCOLS_HDRS_LEN                                           \
  sizeof(SWITCHING_PROTOCOLS) - 1 // -1 to ignore the nul

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

static inline ssize_t ws_build_upgrade_headers(const char *accept_key,
                                               size_t keylen,
                                               char *resp_headers) {
  memcpy(resp_headers, SWITCHING_PROTOCOLS, SWITCHING_PROTOCOLS_HDRS_LEN);
  keylen -= 1;
  memcpy(resp_headers + SWITCHING_PROTOCOLS_HDRS_LEN, accept_key, keylen);
  memcpy(resp_headers + SWITCHING_PROTOCOLS_HDRS_LEN + keylen, CRLF2,
         sizeof(CRLF2));
  return SWITCHING_PROTOCOLS_HDRS_LEN + keylen + sizeof(CRLF2);
}

const char magic_str[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static inline int ws_derive_accept_hdr(const char *akhdr_val, char *derived_val,
                                       size_t len) {
  unsigned char buf[64] = {0};
  memcpy(buf, akhdr_val, strlen(akhdr_val));
  strcat((char *)buf, magic_str);
  len += sizeof magic_str;
  len -= 1;

  unsigned char hash[20] = {0};
  SHA1(buf, len, hash);

  return Base64encode(derived_val, (const char *)hash, sizeof hash);
}

// generic send function (used for upgrade)
static int conn_send(ws_conn_t *conn, const void *data, size_t n);

static void ws_conn_handle(struct ws_conn_t *conn);

static int handle_http(struct ws_conn_t *conn);

static inline int conn_read(struct ws_conn_t *conn, buf_t *buf);

static int conn_drain_write_buf(struct ws_conn_t *conn, buf_t *wbuf);

static int conn_write_frame(ws_conn_t *conn, void *data, size_t len,
                            uint8_t op);

static void conn_list_append(struct conn_list *cl, struct ws_conn_t *conn) {
  if (cl->len + 1 < cl->cap) {
    cl->conns[cl->len++] = conn;
  } else {
    void *new_list = realloc(cl->conns, sizeof cl->conns * (cl->cap + cl->cap));
    if (new_list == NULL) {
      // there really isn't another viable option but to crash in this case
      // what else are we gonna do??? silently failing isn't a great option
      // might consider making this a fixed size array and never growing it if
      // we can find out what the max amount of writeable connections there
      // ever can be in the list it should be the max number of connections
      // that we are willing to accept since duplicates aren't allowed but
      // that is currently unbounded (which is also bad, there is always a
      // limit to everything...)
      fprintf(stderr, "failed to grow connection list, last cap = %zu\n",
              cl->cap);
      exit(1);
    }
    cl->conns = new_list;
    cl->cap = cl->cap + cl->cap;

    cl->conns[cl->len++] = conn;
  }
}

static void server_writeable_conns_append(ws_conn_t *c) {
  // to be added to the list:
  // a connection must not already be queued for writing
  // a connection must be in a writeable state
  if (c->state.writeable && !c->state.write_queued) {
    conn_list_append(&c->base->writeable_conns, c);
    c->state.write_queued = true;
  }
}

static void server_closeable_conns_append(ws_conn_t *c) {
  c->base->ev.data.ptr = c;
  if (epoll_ctl(c->base->epoll_fd, EPOLL_CTL_DEL, c->fd, &c->base->ev) == -1) {
    int err = errno;
    c->base->on_ws_err(c->base, err);
  };
  conn_list_append(&c->base->closeable_conns, c);
  c->state.close_queued = true;
}

static void server_writeable_conns_drain(ws_server_t *s) {
  if (s->writeable_conns.len) {
    printf("draining %zu connections\n", s->writeable_conns.len);
  }

  for (size_t i = 0; i < s->writeable_conns.len; ++i) {
    struct ws_conn_t *c = s->writeable_conns.conns[i];
    if (!c->state.close_queued) {
      if (conn_drain_write_buf(c, &c->write_buf) == -1) {
        server_closeable_conns_append(c);
      };
      c->state.write_queued = 0;
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
      struct ws_conn_t *c = s->closeable_conns.conns[n];
      assert(close(c->fd) == 0);
      s->on_ws_disconnect(c, 0);
      buf_pool_free(s->buffer_pool, c->write_buf.buf);
      buf_pool_free(s->buffer_pool, c->read_buf.buf);

      if (s->shared_send_buffer_owner == c->fd) {
        buf_reset(&s->shared_send_buffer);
        s->shared_send_buffer_owner = -1;
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

  size_t max_backpressure =
      params->max_buffered_bytes ? params->max_buffered_bytes : 1000 * 16;
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
    s->max_conns = rlim.rlim_cur;
  } else {
    if (params->max_conns < rlim.rlim_cur) {
      s->max_conns = params->max_conns;
    } else {
      fprintf(stderr,
              "[WARN] params->max_conns %zu is more than the current limit of "
              "%zu max_conns will be set to the current limit\n",
              params->max_conns, rlim.rlim_cur);
      s->max_conns = rlim.rlim_cur;
    }
  }

  printf("max_conns = %zu\n", s->max_conns);
  s->buffer_pool = buf_pool_init(s->max_conns + s->max_conns + 2, buffer_size);
  s->max_msg_len = max_backpressure;
  buf_init(s->buffer_pool, &s->shared_recv_buffer);
  buf_init(s->buffer_pool, &s->shared_send_buffer);

  s->shared_send_buffer_owner = -1;
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
  // TODO: REMOVE ASAP and use s->ev


  s->ev.data.ptr = s;
  s->ev.events = EPOLLIN;

  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &s->ev) == -1) {
    if (s->on_ws_err) {
      int err = errno;
      s->on_ws_err(s, err);
    } else {
      perror("epoll_ctl");
      exit(1);
    }
    return -1;
  };
  int one = 1;

  for (;;) {
    int n_evs = epoll_wait(epfd, s->events, 1024, -1);
    if (n_evs < 0) {
      if (s->on_ws_err) {
        int err = errno;
        s->on_ws_err(s, err);
      } else {
        perror("epoll_wait");
        exit(1);
      }
      return -1;
    }

    // loop over events
    for (int i = 0; i < n_evs; ++i) {
      if (s->events[i].data.ptr == s) {
        size_t accepts = s->max_conns - s->open_conns;
        if (accepts) {
          while (accepts--) {
            int client_fd =
                accept4(fd, (struct sockaddr *)&client_sockaddr,
                        &client_socklen, SOCK_NONBLOCK | SOCK_CLOEXEC);

            if ((client_fd < 0)) {
              if (!(errno == EAGAIN)) {
                if (s->on_ws_err) {
                  int err = errno;
                  s->on_ws_err(s, err);
                } else {
                  perror("accept4");
                  exit(1);
                }
              }
              break;
            }

            assert(setsockopt(client_fd, SOL_TCP, TCP_NODELAY, &one,
                              sizeof(one)) == 0);

            s->ev.events = EPOLLIN | EPOLLRDHUP;
            struct ws_conn_t *conn = calloc(1, sizeof(struct ws_conn_t));
            assert(conn != NULL);
            conn->fd = client_fd;
            conn->base = s;
            conn->state.writeable = 1;
            conn->state.needed_bytes = 2;
            s->ev.data.ptr = conn;

            assert(buf_init(s->buffer_pool, &conn->read_buf) == 0);
            assert(buf_init(s->buffer_pool, &conn->write_buf) == 0);
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &s->ev) == -1) {
              if (s->on_ws_err) {
                int err = errno;
                s->on_ws_err(s, err);
              } else {
                perror("epoll_ctl");
                exit(1);
              }
            };

            ++s->open_conns;
          }
        }

        // can't accept more connection, remove server's fd from epoll
        // it would be re-added when there is a disconnect and a fd is closed
     
        if (s->max_conns == s->open_conns) {
          printf("[INFO] server at full capacity, pausing accepts on new connections\n");
          s->ev.data.ptr = s;
          if (epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, s->fd, &s->ev) == -1) {
            if (s->on_ws_err) {
              int err = errno;
              s->on_ws_err(s, err);
            } else {
              perror("epoll_ctl");
              exit(1);
            }
          };
        }

      } else {
        if (s->events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
          ws_conn_destroy(s->events[i].data.ptr);
        } else if (s->events[i].events & EPOLLOUT) {
          ws_conn_t *c = s->events[i].data.ptr;
          if (!c->state.close_queued) {
            int ret = conn_drain_write_buf(c, &c->write_buf);
            if (ret == 1) {
              ws_conn_t *c = s->events[i].data.ptr;
              if (s->on_ws_drain) {
                s->on_ws_drain(c);
              }
              s->ev.data.ptr = c;
              s->ev.events = EPOLLIN | EPOLLRDHUP;
              if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &s->ev) == -1) {
                if (s->on_ws_err) {
                  int err = errno;
                  s->on_ws_err(s, err);
                } else {
                  perror("epoll_ctl");
                  exit(1);
                }
              };
            } else if (ret == -1) {
              ws_conn_destroy(s->events[i].data.ptr);
            }
          }

        } else if (s->events[i].events & EPOLLIN) {
          ws_conn_t *c = s->events[i].data.ptr;
          if (!c->state.close_queued) {
            if (buf_len(&s->shared_send_buffer) == 0) {
              s->shared_send_buffer_owner = c->fd;
            }

            if (!c->state.upgraded) {
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

    // we are at full capacity and about to remove something
    // that's when we can rearm EPOLLIN on the listener 
    bool should_enable_accepts = s->max_conns == s->open_conns && s->closeable_conns.len;
    server_closeable_conns_close(s); // close connections
    if (should_enable_accepts) {
      printf("[INFO] starting to accept connections again\n");
      s->ev.events = EPOLLIN;
      s->ev.data.ptr = s;
      if (epoll_ctl(epfd, EPOLL_CTL_ADD, s->fd, &s->ev) == -1) {
        if (s->on_ws_err) {
          int err = errno;
          s->on_ws_err(s, err);
        } else {
          perror("epoll_ctl");
          exit(1);
        }
      };
    }
  }

  return 0;
}

static ssize_t handle_upgrade(const char *buf, char *res_hdrs, size_t n) {
  int ret = get_header(buf, SEC_WS_KEY_HDR, res_hdrs, n);
  if (ret < 0) {
    printf("error parsing http headers: %d\n", ret);
    return -1;
  }

  char accept_key[64];
  int len = ws_derive_accept_hdr(res_hdrs, accept_key, ret - 1);

  return ws_build_upgrade_headers(accept_key, len, res_hdrs);
}

static int conn_read(struct ws_conn_t *conn, buf_t *buf) {
  ssize_t n = buf_recv(buf, conn->fd, 0);
  if (n == -1) {
    if ((errno == EAGAIN || errno == EWOULDBLOCK)) {
      return 0;
    }
    ws_conn_destroy(conn);
    return -1;
  } else if (n == 0) {
    ws_conn_destroy(conn);
    return -1;
  }

  return 0;
}

static int handle_http(struct ws_conn_t *conn) {
  ws_server_t *s = conn->base;
  buf_t *buf;
  if (buf_len(&conn->read_buf)) {
    buf = &conn->read_buf;
  } else {
    buf = &s->shared_recv_buffer;
  }

  if (conn_read(conn, buf) == -1) {
    return -1;
  };

  uint8_t *headers = buf_peek(buf);
  size_t rbuf_len = buf_len(buf);
  if (!strncmp((char *)headers, GET_RQ, sizeof GET_RQ - 1)) {
    char res_hdrs[1024] = {0};
    ssize_t ret = handle_upgrade((char *)headers, res_hdrs, sizeof res_hdrs);

    int n = conn_send(conn, res_hdrs, ret - 1);
    if (n == ret - 1) {
      s->on_ws_open(conn); // websocket connection is upgraded
    } else {
      // TODO(sah): buffer up the remainder of the response and send more when
      // EPOLLOUT is triggered
      fprintf(
          stderr,
          "[Warn]: unimplemented logic around partial send during upgrade\n");
    }
    // printf("Res --------------------------------\n");
    // printf("%s\n", res_hdrs);
    buf_consume(buf, rbuf_len);
    conn->state.upgraded = 1;
    conn->state.needed_bytes = 2;
    return 0;
  }

  // TODO(sah): handle errors
  return -1;
}

static inline buf_t *ws_conn_choose_read_buf(struct ws_conn_t *conn) {

  if ((buf_len(&conn->read_buf) != 0) &
      !(conn->state.fragments_len + conn->read_buf.rpos ==
        conn->read_buf.wpos)) {
    // buf_debug(&conn->read_buf, "conn buffer chosen");

    return &conn->read_buf;
  } else {
    return &conn->base->shared_recv_buffer;
  }
}

static size_t ws_conn_readable_len(ws_conn_t *conn, buf_t *buf) {
  if (buf != &conn->read_buf) {
    return buf->wpos - buf->rpos;
  } else {
    return buf->wpos - buf->rpos - conn->state.fragments_len;
  }
}

static inline void ws_conn_handle(ws_conn_t *conn) {
  buf_t *buf = ws_conn_choose_read_buf(conn);
  ws_server_t *s = conn->base;

  // total frame header bytes trimmed
  size_t total_trimmed = 0;
  size_t max_allowed_len = s->max_msg_len;

  if (conn_read(conn, buf) == 0) {
    while (ws_conn_readable_len(conn, buf) - total_trimmed >=
           conn->state.needed_bytes) {
      // payload start
      uint8_t *frame_buf = buf_peek_at(
          buf, buf->rpos + ((buf == &conn->read_buf) *
                            (conn->state.fragments_len + total_trimmed)));

      uint8_t fin = frame_get_fin(frame_buf);
      uint8_t opcode = frame_get_opcode(frame_buf);
      // printf("fragments=%zu\n", conn->state.fragments_len);
      // run general validation checks on the header
      if (((fin == 0) & ((opcode > 2) & (opcode != OP_CONT))) |
          (frame_has_reserved_bits_set(frame_buf) == 1) |
          (frame_is_masked(frame_buf) == 0)) {
        printf("invalid frame closing\n");
        printf("opcode=%d\n", opcode);
        printf("fin=%d\n", fin);
        buf_reset(&s->shared_recv_buffer);
        ws_conn_destroy(conn);
        return;
      }

      // make sure we can get the full msg
      size_t payload_len = 0;
      size_t frames_buffer_len = buf_len(buf);
      if (&conn->read_buf == buf) {
        frames_buffer_len =
            frames_buffer_len - conn->state.fragments_len - total_trimmed;
      }
      // check if we need to do more reads to get the msg length
      int missing_header_len =
          frame_decode_payload_len(frame_buf, frames_buffer_len, &payload_len);
      if (missing_header_len) {
        // wait for atleast remaining of the header
        conn->state.needed_bytes = missing_header_len;
        goto clean_up_buffer;
      }

      // Todo(sah): add validation to payload_len
      // ....
      // ....

      size_t mask_offset = frame_get_header_len(payload_len);
      size_t full_frame_len = payload_len + 4 + mask_offset;

      if (full_frame_len > max_allowed_len) {
        // drop the connection
        printf("max allowed length exceeded: drop the connections "
               "[unimplemented]\n");
      }

      // check that we have atleast the whole frame, otherwise
      // set needed_bytes and exit waiting for more reads from the socket
      if (frames_buffer_len < full_frame_len) {
        conn->state.needed_bytes = full_frame_len;
        goto clean_up_buffer;
      }

      uint8_t *msg = frame_buf + mask_offset + 4;
      msg_unmask(msg, frame_buf + mask_offset, payload_len);
      // printf("buf_len=%zu frame_len=%zu opcode=%d fin=%d\n",
      // frames_buffer_len,
      //        full_frame_len, opcode, fin);

      switch (opcode) {
      case OP_TXT:
      case OP_BIN:
        conn->state.bin = opcode == OP_BIN;
        // fin and never fragmented
        // this handles both text and binary hence the fallthrough
        if (fin & (conn->state.fragments_len == 0)) {
          if (!conn->state.bin && !utf8_is_valid(msg, payload_len)) {
            printf("failed validation\n");
            ws_conn_destroy(conn);
            buf_reset(&s->shared_recv_buffer);
            return; // TODO(sah): send a Close frame, & call close callback
          }
          s->on_ws_msg(conn, msg, payload_len, conn->state.bin);
          buf_consume(buf, full_frame_len);
          conn->state.bin = 0;
          conn->state.needed_bytes = 2;

          break; /* OP_BIN don't fall through to fragmented msg */
        } else if (fin & (conn->state.fragments_len > 0)) {
          // this is invalid because we expect continuation not text or binary
          // opcode
          ws_conn_destroy(conn);
          buf_reset(&s->shared_recv_buffer);
          return;
        }

      case OP_CONT:
        // todo: add msg size validation for fragmented msgs
        // accumulate bytes and increase fragments_len

        // move bytes over
        // call the callback
        // reset

        // can't send cont as first fragment
        if ((opcode == OP_CONT) & (conn->state.fragments_len == 0)) {
          ws_conn_destroy(conn);
          buf_reset(&s->shared_recv_buffer);
          return;
        }

        // we are using the shared buffer
        if (buf != &conn->read_buf) {
          // trim off the header
          buf_consume(buf, mask_offset + 4);
          buf_move(buf, &conn->read_buf, payload_len);
          conn->state.fragments_len += payload_len;
          conn->state.needed_bytes = 2;
        } else {
          // place back at the frame_buf start which contains the header & mask
          // we want to get rid of but ensure to subtract by the frame_gap to
          // fill it if it isn't zero
          memmove(frame_buf - total_trimmed, msg, payload_len);
          conn->state.fragments_len += payload_len;
          total_trimmed += mask_offset + 4;
          conn->state.needed_bytes = 2;
        }

        if (fin) {
          if (!conn->state.bin && !utf8_is_valid(buf_peek(&conn->read_buf),
                                                 conn->state.fragments_len)) {
            printf("failed validation\n");
            ws_conn_destroy(conn);
            buf_reset(&s->shared_recv_buffer);
            return; // TODO(sah): send a Close frame, & call close callback
          }
          s->on_ws_msg(conn, buf_peek(&conn->read_buf),
                       conn->state.fragments_len, conn->state.bin);
          buf_consume(&conn->read_buf, conn->state.fragments_len);

          conn->state.fragments_len = 0;
          conn->state.needed_bytes = 2;
          conn->state.bin = 0;
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
        if ((conn->state.fragments_len != 0) & (buf == &conn->read_buf)) {
          total_trimmed += full_frame_len;
          conn->state.needed_bytes = 2;
        } else {
          // printf("here\n");
          buf_consume(buf, full_frame_len);
          conn->state.needed_bytes = 2;
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

        if ((conn->state.fragments_len != 0) & (buf == &conn->read_buf)) {
          total_trimmed += total_trimmed;
          conn->state.needed_bytes = 2;
        } else {
          buf_consume(buf, full_frame_len);
          conn->state.needed_bytes = 2;
        }
        break;
      case OP_CLOSE:
        if (!payload_len) {
          if (s->on_ws_close) {
            s->on_ws_close(conn, WS_CLOSE_NORMAL, NULL);
          } else {
            ws_conn_close(conn, NULL, 0, WS_CLOSE_NORMAL);
          }
          buf_reset(&s->shared_recv_buffer);
          return;
        } else if (payload_len < 2) {
          if (s->on_ws_close) {
            s->on_ws_close(conn, WS_CLOSE_EPROTO, NULL);
          } else {
            ws_conn_close(conn, NULL, 0, WS_CLOSE_EPROTO);
          }
          buf_reset(&s->shared_recv_buffer);
          return;
        }

        else {
          uint16_t code = WS_CLOSE_NOSTAT;
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
              s->on_ws_close(conn, WS_CLOSE_EPROTO, NULL);
            } else {
              ws_conn_close(conn, NULL, 0, WS_CLOSE_EPROTO);
            }
            buf_reset(&s->shared_recv_buffer);
            return;
          }

          if (s->on_ws_close) {
            s->on_ws_close(conn, code, msg + 2);
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
    if (s->shared_send_buffer_owner == conn->fd) {
      conn_drain_write_buf(conn, &s->shared_send_buffer);
    }

  clean_up_buffer:
    if ((buf == &s->shared_recv_buffer) && (buf_len(buf) > 0)) {
      // move to connection specific buffer
      // printf("moving from shared to socket buffer: %zu\n", buf_len(buf));
      buf_move(buf, &conn->read_buf, buf_len(buf));
    } else {

      memmove(buf->buf + buf->rpos + conn->state.fragments_len,
              buf->buf + buf->rpos + conn->state.fragments_len + total_trimmed,
              buf->wpos - buf->rpos + conn->state.fragments_len +
                  total_trimmed);
      buf->wpos =
          buf->rpos + conn->state.fragments_len +
          (buf->wpos - buf->rpos - conn->state.fragments_len - total_trimmed);
    }
  }
}

static void ws_conn_notify_on_writeable(struct ws_conn_t *conn) {
  conn->state.writeable = 0;
  conn->base->ev.data.ptr = conn;
  conn->base->ev.events = EPOLLOUT | EPOLLRDHUP;
  if (epoll_ctl(conn->base->epoll_fd, EPOLL_CTL_MOD, conn->fd,
                &conn->base->ev) == -1) {
    if (conn->base->on_ws_err) {
      int err = errno;
      conn->base->on_ws_err(conn->base, err);
    } else {
      perror("epoll_ctl");
      exit(1);
    }
    int err = errno;
    conn->base->on_ws_err(conn->base, err);
    // might wanna close the socket?
  }
}

static int conn_drain_write_buf(struct ws_conn_t *conn, buf_t *wbuf) {
  size_t to_write = buf_len(wbuf);
  ssize_t n = 0;

  if (!to_write) {
    return 0;
  }

  n = buf_send(wbuf, conn->fd, 0);
  if ((n == -1 && errno != EAGAIN) | (n == 0)) {
    return -1;
  }

  if (to_write == n) {
    conn->state.writeable = 1;
    return 1;
  } else {
    if (&conn->base->shared_send_buffer == wbuf) {
      // worst case
      buf_move(&conn->base->shared_send_buffer, &conn->write_buf,
               buf_len(&conn->base->shared_send_buffer));
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
     1 data was completely written

     0 part of the data was written caller should wait for on_drain event to
start sending more data or an error occurred in which the corresponding callback
will be called
*/
inline int ws_conn_send(ws_conn_t *c, void *msg, size_t n) {
  int stat = conn_write_frame(c, msg, n, OP_BIN);
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
inline int ws_conn_send_txt(ws_conn_t *c, void *msg, size_t n) {
  int stat = conn_write_frame(c, msg, n, OP_TXT);
  if (stat == -1) {
    ws_conn_destroy(c);
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
  if (conn->state.close_queued) {
    return;
  }

  // if we can say good bye let's do that
  // not if a partial write is in progress this will
  // lead to us sending garbage as the other side is decoding the frames
  // because we will have interleaved data sent...
  // todo(Sah): solve for the above
  if (conn->state.writeable) {
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
      writev(conn->fd, iovs, 2);
    } else {
      uint8_t buf[4] = {0, 0};
      buf[0] = FIN | OP_CLOSE;
      buf[1] = 2;
      buf[2] = (code >> 8) & 0xFF;
      buf[3] = code & 0xFF;
      send(conn->fd, buf, 4, 0);
    }
  }

  // prepare the connection to be close and queue it up in the close queue
  server_closeable_conns_append(conn);
}

void ws_conn_destroy(ws_conn_t *conn) {
  if (conn->state.close_queued) {
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

  /**
  * Todo(sah):
    in the common cases where there is a need to send data to a socket
    that has no event triggered that would cause us to drain the write buffer
    we need a way to "remember" draining all socket buffers which have data
    at the end of each loop iteration
    we should also keep in mind that sockets can be closed in the meantime and
    carefully manage connection & buffer lifetimes, this also includes changing
  how connection closing is handled at the moment to avoid serious memory/state
  problems
  */

  if (conn->state.close_queued) {
    // this function seriously needs better returns codes...
    return 0;
  }

  ws_server_t *s = conn->base;
  size_t hlen = frame_get_header_len(len);
  buf_t *wbuf;

  // use the connection write buffer when
  // we don't own the shared send buffer or
  // we are doing a large send 65kb+ or
  // the connection is not in a writeable state or
  // we currently have data in the connection write buffer pending
  if ((s->shared_send_buffer_owner != conn->fd) | (hlen == 10) |
      (buf_len(&conn->write_buf) != 0) | (conn->state.writeable == 0)) {
    wbuf = &conn->write_buf;
  } else {
    wbuf = &s->shared_send_buffer;
  }

  size_t flen = len + hlen;
  if (buf_space(wbuf) > flen) {
    if (hlen == 2) {
      uint8_t *hbuf =
          wbuf->buf + wbuf->wpos; // place the header in the write buffer
      memset(hbuf, 0, 2);
      hbuf[0] = FIN | op; // Set FIN bit and opcode
      hbuf[1] = (uint8_t)len;
      wbuf->wpos += hlen;
    } else if (hlen == 4) {
      uint8_t *hbuf =
          wbuf->buf + wbuf->wpos; // place the header in the write buffer
      memset(hbuf, 0, 2);
      hbuf[0] = FIN | op; // Set FIN bit and opcode
      hbuf[1] = PAYLOAD_LEN_16;
      hbuf[2] = (len >> 8) & 0xFF;
      hbuf[3] = len & 0xFF;
      wbuf->wpos += hlen;
    } else {
      // large sends are a bit more complex to handle
      // we attempt to avoid as much copying as possible
      // using vectored I/O
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

      // if connection is in a writeable state
      if (conn->state.writeable) {
        ssize_t n;
        size_t total_write;
        bool shared_buf_needs_drain = s->shared_send_buffer_owner == conn->fd &&
                                      buf_len(&s->shared_send_buffer) != 0;

        // we have to drain from shared buffer so make it first iovec entry
        if (shared_buf_needs_drain) {
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
          n = buf_drain_write2v(&s->shared_send_buffer, vecs, total_write, wbuf,
                                conn->fd);

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
          return 1;
        } else if (n == 0 || (n == -1 && errno != EAGAIN)) {
          return -1;
        } else {
          ws_conn_notify_on_writeable(conn);
          return 1;
        }
      }
      // connection is not writeable
      else {
        buf_put(wbuf, hbuf, hlen);
        buf_put(wbuf, data, len);
      }

      return 1;
    }

    // under 65kb case
    buf_put(wbuf, data, len);

    if (wbuf != &s->shared_send_buffer) {
      // queue it up for writing
      server_writeable_conns_append(conn);
    }
    return 1;
  } else {
    return 0;
  }
}

static int conn_send(ws_conn_t *conn, const void *data, size_t len) {
  // this function sucks, it's only used for the upgrade and should be reworked
  ssize_t n = 0;

  if ((conn->state.writeable == 1) & (buf_space(&conn->write_buf) > len)) {
    n = send(conn->fd, data, len, 0);
    if (n == 0) {
      return -1;
    } else if (n == -1) {
      if (!((errno == EAGAIN) | (errno == EWOULDBLOCK))) {
        return -1;
      }
      n = 0;
    }

    if (n < len) {
      conn->base->ev.events = EPOLLOUT | EPOLLRDHUP;
      conn->base->ev.data.ptr = conn;
      conn->state.writeable = 0;
      assert(buf_put(&conn->write_buf, (uint8_t *)data + n, len - n) == 0);
      if (epoll_ctl(conn->base->epoll_fd, EPOLL_CTL_MOD, conn->fd,
                    &conn->base->ev) == -1) {
        return -1;
      };

      return 0;
    }
  }

  return n;
}

int ws_conn_fd(ws_conn_t *c) { return c->fd; }

inline ws_server_t *ws_conn_server(ws_conn_t *c) { return c->base; }

inline void *ws_conn_ctx(ws_conn_t *c) { return c->ctx; }

inline void ws_conn_ctx_attach(ws_conn_t *c, void *ctx) { c->ctx = ctx; }

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

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

#define WITH_COMPRESSION

#ifdef WITH_COMPRESSION
#include <zlib.h>
#endif /* WITH_COMPRESSION */

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

struct basic_buffer {
  size_t len;
  size_t cap;
  char data[];
};

// mirrored buffer defintion and helpers
// this one is a ring buffer with hardware handled wrap around
// uses two memory mappings that are backed by the same physical memory (memfd)
// the two mapping are contigious in the virtual address space but both start at
// the beginning of the physical buffer to allow contigious access in all cases
// used for socket IO and HTTP/Websocket protocol parsing

typedef struct {
  size_t rpos;
  size_t wpos;
  size_t buf_sz;
  uint8_t *buf;
} mirrored_buf_t;

static inline size_t buf_len(mirrored_buf_t *r) { return r->wpos - r->rpos; }

static inline void buf_reset(mirrored_buf_t *r) {
  // we can do this because indexes are in the beginning
  memset(r, 0, sizeof(size_t) * 2);
}

static inline size_t buf_space(mirrored_buf_t *r) {
  return r->buf_sz - (r->wpos - r->rpos);
}

static inline int buf_put(mirrored_buf_t *r, const void *data, size_t n);

static inline uint8_t *buf_peek(mirrored_buf_t *r) { return r->buf + r->rpos; }

static inline uint8_t *buf_peek_at(mirrored_buf_t *r, size_t at) {
  return r->buf + at;
}

static inline uint8_t *buf_peekn(mirrored_buf_t *r, size_t n) {
  if (buf_len(r) < n) {
    return NULL;
  }

  return r->buf + r->rpos;
}

static inline int buf_consume(mirrored_buf_t *r, size_t n);

static inline void buf_move(mirrored_buf_t *src_b, mirrored_buf_t *dst_b,
                            size_t n) {
  buf_put(dst_b, src_b->buf + src_b->rpos, n);
  buf_consume(src_b, n);
}

static inline void buf_debug(mirrored_buf_t *r, const char *label) {
  printf("%s rpos=%zu wpos=%zu\n", label, r->rpos, r->wpos);
}

static inline ssize_t buf_recv(mirrored_buf_t *r, int fd, size_t len,
                               int flags);

static inline ssize_t buf_send(mirrored_buf_t *r, int fd, int flags);

static inline ssize_t buf_write2v(mirrored_buf_t *r, int fd,
                                  struct iovec const *iovs, size_t const total);

static inline ssize_t buf_drain_write2v(mirrored_buf_t *r,
                                        struct iovec const *iovs,
                                        size_t const total,
                                        mirrored_buf_t *rem_dst, int fd);

struct buf_node {
  void *b;
  struct buf_node *next;
};

struct mirrored_buf_pool {
  int fd;
  uint32_t nmemb;
  size_t buf_sz;
  void *base;

  size_t avb;
  size_t cap;
  struct buf_pool *pool;
  mirrored_buf_t **avb_list;
  mirrored_buf_t *mirrored_bufs;
};

static struct mirrored_buf_pool *mirrored_buf_pool_create(uint32_t nmemb,
                                                          size_t buf_sz);

static mirrored_buf_t *mirrored_buf_get(struct mirrored_buf_pool *bp);

static void mirrored_buf_put(struct mirrored_buf_pool *bp, mirrored_buf_t *buf);

struct ws_conn_t {
  int fd;                     // socket fd
  unsigned int flags;         // state flags
  unsigned int read_timeout;  // seconds
  unsigned int write_timeout; // seconds
  size_t fragments_len;       // size of the data portion of the frames across
                              // fragmentation

  size_t needed_bytes; // bytes needed before we can do something with the frame
  mirrored_buf_t *recv_buf;
  mirrored_buf_t *send_buf;
  ws_server_t *base; // server ptr
  void *ctx;         // user data pointer

#ifdef WITH_COMPRESSION
  struct basic_buffer *pmd_buf;
#endif /* WITH_COMPRESSION */
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
#define CONN_TX_WRITE_QUEUED (1u << 6)
#define CONN_TX_DISPOSING (1u << 7)
#define CONN_COMPRESSION_ALLOWED (1u << 8)
#define CONN_RX_COMPRESSED_FRAGMENTS (1u << 9)
#define CONN_TIMER_QUEUED (1u << 10)

struct conn_list {
  size_t len;
  size_t cap;
  ws_conn_t **conns;
};

// per message deflate buffer pool
// this one will be use malloc/calloc but only when needed
// once buffer usage is done it will stay in the pool until another
// buffer is needed in which we can skip going through the allocator and reuse
// a previously allocated buffer

struct basic_buffer_pool {
  size_t buf_sz;
  size_t avb;
  size_t inuse;
  size_t cap;
  struct basic_buffer **avb_list;
};

struct basic_buffer_pool *basic_buffer_pool_create(size_t nmemb,
                                                   size_t pmd_bufsz);
struct basic_buffer *pmd_buf_get(struct basic_buffer_pool *p);
void pmd_buf_put(struct basic_buffer_pool *p, struct basic_buffer *buf);

#ifdef WITH_COMPRESSION
static z_stream *inflation_stream_init();

static ssize_t inflation_stream_inflate(z_stream *istrm, char *input,
                                        size_t in_len, char *out,
                                        size_t out_len, bool no_ctx_takeover);

static z_stream *deflation_stream_init();

static ssize_t deflation_stream_deflate(z_stream *dstrm, char *input,
                                        size_t in_len, char *out,
                                        size_t out_len, bool no_ctx_takeover);
#endif /* WITH_COMPRESSION */

typedef struct server {
  size_t max_msg_len; // max allowed msg length
  ws_msg_cb_t on_ws_msg;
  ws_msg_fragment_cb_t on_ws_msg_fragment;
  ws_ping_cb_t on_ws_ping;
  struct mirrored_buf_pool *buffer_pool;
  size_t open_conns; // open websocket connections
  size_t max_conns;  // max connections allowed
  ws_accept_cb_t on_ws_accept;

  ws_open_cb_t on_ws_open;
  ws_drain_cb_t on_ws_drain;
  ws_disconnect_cb_t on_ws_disconnect;
  ws_on_upgrade_req_cb_t on_ws_upgrade_req;
  ws_close_cb_t on_ws_close;
  ws_pong_cb_t on_ws_pong;
  ws_on_timeout_t on_ws_conn_timeout;
  struct ws_conn_pool *conn_pool;

  ws_err_cb_t on_ws_err;
  ws_err_accept_cb_t on_ws_accept_err;
  struct basic_buffer_pool *basic_buffer_pool;
#ifdef WITH_COMPRESSION
  z_stream *istrm;
  z_stream *dstrm;
#endif    /* WITH_COMPRESSION */
  int fd; // server file descriptor
  int epoll_fd;
  bool accept_paused; // are we paused on accepting new connections
  struct epoll_event ev;
  struct epoll_event events[1024];
  struct conn_list pending_timers;
  struct conn_list writeable_conns;
  struct conn_list closeable_conns;
  int user_epoll;
} ws_server_t;

#ifdef WITH_COMPRESSION
static struct basic_buffer *conn_basic_buffer(ws_conn_t *c) {
  if (!c->pmd_buf) {
    c->pmd_buf = pmd_buf_get(c->base->basic_buffer_pool);
    return c->pmd_buf;
  } else {
    return c->pmd_buf;
  }
}

static void conn_basic_buffer_dispose(ws_conn_t *c) {
  if (c->pmd_buf) {
    pmd_buf_put(c->base->basic_buffer_pool, c->pmd_buf);
    c->pmd_buf = NULL;
  }
}
#endif /* WITH_COMPRESSION */

// maybe later we can support dedicated compression
// z_stream *conn_inflate_stream(ws_conn_t *c) {
//   if (c->buffers[3]) {
//     return c->buffers[3];
//   }

//   z_stream *strm = inflation_stream_init();
//   c->buffers[3] = strm;
//   return strm;
// }

// void conn_inflate_stream_destroy(ws_conn_t *c) {
//   if (c->buffers[3]) {
//     inflateEnd(c->buffers[3]);
//     c->buffers[3] = NULL;
//   }
// }

// z_stream *conn_deflate_stream(ws_conn_t *c) {
//   if (c->buffers[4]) {
//     return c->buffers[4];
//   }

//   z_stream *strm = deflation_stream_init();
//   c->buffers[4] = strm;

//   return strm;
// }

// void conn_deflate_stream_destroy(ws_conn_t *c) {
//   if (c->buffers[4]) {
//     deflateEnd(c->buffers[4]);
//     c->buffers[4] = NULL;
//   }
// }

// connection state utils so we don't fill the code with bit manipulation

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

static inline bool is_write_shutdown(ws_conn_t *c) {
  return (c->flags & CONN_TX_DISPOSING) != 0;
}

static inline void set_write_shutdown(ws_conn_t *c) {
  c->flags |= CONN_TX_DISPOSING;
}

static inline bool is_compression_allowed(ws_conn_t *c) {
  return (c->flags & CONN_COMPRESSION_ALLOWED) != 0;
}

static inline void set_compression_allowed(ws_conn_t *c) {
  c->flags |= CONN_COMPRESSION_ALLOWED;
}

static inline bool is_fragment_compressed(ws_conn_t *c) {
  return (c->flags & CONN_RX_COMPRESSED_FRAGMENTS) != 0;
}

static inline void set_fragment_compressed(ws_conn_t *c) {
  c->flags |= CONN_RX_COMPRESSED_FRAGMENTS;
}

static inline void clear_fragment_compressed(ws_conn_t *c) {
  c->flags &= ~CONN_RX_COMPRESSED_FRAGMENTS;
}

static inline bool has_pending_timers(ws_conn_t *c) {
  return (c->flags & CONN_TIMER_QUEUED) != 0;
}

static inline void set_has_pending_timers(ws_conn_t *c) {
  c->flags |= CONN_TIMER_QUEUED;
}

static inline void clear_has_pending_timers(ws_conn_t *c) {
  c->flags &= ~CONN_TIMER_QUEUED;
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

static inline bool is_compressed_msg(uint8_t const *buf) {
  return (buf[0] & 0x40) != 0;
}

static int base64_encode(char *coded_dst, const char *plain_src,
                         int len_plain_src);

static inline int frame_has_unsupported_reserved_bits_set(ws_conn_t *c,
                                                          uint8_t const *buf) {
  // printf("%d\n", is_compression_allowed(c));
  bool rsv1 = (buf[0] & 0x40) != 0;
  bool rsv2 = (buf[0] & 0x20) != 0;
  bool rsv3 = (buf[0] & 0x10) != 0;
  return rsv3 || rsv2 || (rsv1 && !is_compression_allowed(c));
}

static inline uint32_t frame_is_masked(const unsigned char *buf) {
  return (buf[1] >> 7) & 0x01;
}

static inline size_t frame_get_header_len(size_t const n) {
  return 2 + ((n > 125) * 2) + ((n > 0xFFFF) * 6);
}

static void msg_unmask(uint8_t *src, uint8_t const *mask, size_t const n) {

  size_t i = 0;
  size_t uneven = n & 7;

  for (; i < uneven; ++i) {
    src[i] = src[i] ^ mask[i & 3];
  }

  uint8_t tmp[8] = {mask[i & 3],       mask[(i + 1) & 3], mask[(i + 2) & 3],
                    mask[(i + 3) & 3], mask[(i + 4) & 3], mask[(i + 5) & 3],
                    mask[(i + 6) & 3], mask[(i + 7) & 3]};

  uint64_t mask64;
  uint64_t chunk;

  memcpy(&mask64, tmp, 8);

  while (i < n) {
    memcpy(&chunk, src + i, 8);
    chunk ^= mask64;
    memcpy(src + i, &chunk, 8);
    i += 8;
  }
}

// static void msg_unmask(uint8_t *src, uint8_t const *mask, size_t const n) {

//   size_t i = 0;
//   size_t left_over = n & 3;

//   for (; i < left_over; ++i) {
//     src[i] = src[i] ^ mask[i & 3];
//   }

//   while (i < n) {
//     src[i] = src[i] ^ mask[i & 3];
//     src[i + 1] = src[i + 1] ^ mask[(i + 1) & 3];
//     src[i + 2] = src[i + 2] ^ mask[(i + 2) & 3];
//     src[i + 3] = src[i + 3] ^ mask[(i + 3) & 3];
//     i += 4;
//   }
// }

static void ws_server_epoll_ctl(ws_server_t *s, int op, int fd);

static void ws_conn_handle(ws_conn_t *conn);

static void handle_upgrade(ws_conn_t *conn);

static inline int conn_read(ws_conn_t *conn, mirrored_buf_t *buf);

static int conn_drain_write_buf(ws_conn_t *conn);

static int conn_write_frame(ws_conn_t *conn, void *data, size_t len,
                            uint8_t op);

static void conn_list_append(struct conn_list *cl, ws_conn_t *conn) {
  if (cl->len + 1 < cl->cap) {
    cl->conns[cl->len++] = conn;
  } else {
    // this would be a serious bug, we should always have enough space unless we
    // are adding duplicates and theres a nasty bug so we can keep this check
    // maybe ??
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

static void server_pending_timers_append(ws_conn_t *c) {
  if (!has_pending_timers(c)) {
    conn_list_append(&c->base->pending_timers, c);
    set_has_pending_timers(c);
  }
}

static void server_pending_timers_remove(ws_conn_t *c) {
  if (has_pending_timers(c)) {
    // go through all timers in list and swap with the last
    while (c->base->pending_timers.len) {
      size_t i = c->base->pending_timers.len;
      while (i--) {
        if (c->base->pending_timers.conns[i] == c) {
          clear_has_pending_timers(c);
          ws_conn_t *tmp =
              c->base->pending_timers.conns[--c->base->pending_timers.len];
          c->base->pending_timers.conns[i] = tmp;
          break;
        }
      }
      break;
    }
  }
}

static void server_closeable_conns_append(ws_conn_t *c) {
  if (c->recv_buf) {
    mirrored_buf_put(c->base->buffer_pool, c->recv_buf);
    c->recv_buf = NULL;
  }
  if (c->send_buf) {
    mirrored_buf_put(c->base->buffer_pool, c->send_buf);
    c->send_buf = NULL;
  }

  c->base->ev.data.ptr = c;
  ws_server_epoll_ctl(c->base, EPOLL_CTL_DEL, c->fd);
  conn_list_append(&c->base->closeable_conns, c);
  mark_closing(c);
  server_pending_timers_remove(c);
}

static void server_check_pending_timers(ws_server_t *s) {
  int timeout_kind = 0;
  ws_on_timeout_t cb = s->on_ws_conn_timeout;
  unsigned int now = (unsigned int)time(NULL);

  while (s->pending_timers.len) {
    size_t i = s->pending_timers.len;
    while (i--) {
      ws_conn_t *c = s->pending_timers.conns[i];

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

    break;
  }
}

static void server_writeable_conns_drain(ws_server_t *s) {
  if (s->writeable_conns.len) {
    printf("draining %zu connections\n", s->writeable_conns.len);
  }

  for (size_t i = 0; i < s->writeable_conns.len; ++i) {
    ws_conn_t *c = s->writeable_conns.conns[i];
    if (!is_closing(c->flags)) {
      conn_drain_write_buf(c);
      clear_write_queued(c);
    }
  }

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
      (max_backpressure + 192 + page_size - 1) & ~(page_size - 1);

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
      mirrored_buf_pool_create(s->max_conns + s->max_conns + 2, buffer_size);

  s->conn_pool = ws_conn_pool_create(s->max_conns);

  assert(s->conn_pool != NULL);
  assert(s->buffer_pool != NULL);

  s->max_msg_len = max_backpressure;

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

  s->pending_timers.conns =
      calloc(s->max_conns, sizeof s->pending_timers.conns);
  s->pending_timers.len = 0;
  s->pending_timers.cap = s->max_conns;

  // make sure we got the mem needed
  assert(s->writeable_conns.conns != NULL && s->closeable_conns.conns != NULL &&
         s->pending_timers.conns != NULL);

#ifdef WITH_COMPRESSION
  s->istrm = inflation_stream_init();
  s->dstrm = deflation_stream_init();
#endif /* WITH_COMPRESSION */

  s->basic_buffer_pool = basic_buffer_pool_create(
      s->max_conns, s->max_msg_len + s->max_msg_len + 16);
  assert(s->basic_buffer_pool != NULL);

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

        assert(conn->send_buf == NULL);
        assert(conn->recv_buf == NULL);

        s->ev.data.ptr = conn;

#ifdef WITH_COMPRESSION
        conn->pmd_buf = NULL;
#endif /* WITH_COMPRESSION*/

        conn->read_timeout = now + READ_TIMEOUT;
        ws_server_epoll_ctl(s, EPOLL_CTL_ADD, client_fd);
        ++s->open_conns;

        server_pending_timers_append(conn);

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

    bool check_user_epoll = false;
    int *user_epoll_ptr = &s->user_epoll;
    // loop over events
    for (int i = 0; i < n_evs; ++i) {
      if ((s->events[i].data.ptr == &tfd) |
          (s->events[i].data.ptr == user_epoll_ptr)) {
        if (s->events[i].data.ptr == &tfd) {
          uint64_t _;
          assert(read(tfd, &_, 8) == 8);
          (void)_;

          server_check_pending_timers(s);
        } else {
          check_user_epoll = true;
        }
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
              int ret = conn_drain_write_buf(c);
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
              }
            }
          }
          if (s->events[i].events & EPOLLIN) {
            if (!is_closing(c->flags)) {
              if (!c->recv_buf) {
                c->recv_buf = mirrored_buf_get(s->buffer_pool);
              }

              if (is_upgraded(c)) {
                ws_conn_handle(c);
              } else {
                handle_upgrade(c);
                // remove
                if (c->recv_buf) {
                  exit(EXIT_FAILURE);
                }
                if (c->send_buf) {
                  exit(EXIT_FAILURE);
                }
              }
            }
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

    if (check_user_epoll) {
      int count = epoll_wait(*user_epoll_ptr, s->events, 1024, 0);
      if (count > 0) {
        for (int i = 0; i < count; ++i) {
          ws_poll_cb_ctx_t *ctx = s->events[i].data.ptr;
          if (ctx) {
            ctx->cb(s, ctx, s->events[i].events);
          }
        }
      }

      check_user_epoll = false;
    }
  }

  return 0;
}

static int conn_read(ws_conn_t *conn, mirrored_buf_t *buf) {
  size_t space = buf_space(buf);

#ifdef WITH_COMPRESSION
  space = space > 4 ? space - 4 : 0;
#endif /* WITH_COMPRESSION */

  ssize_t n = buf_recv(buf, conn->fd, space, 0);
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

  return base64_encode(derived_val, (const char *)hash, sizeof hash);
}

static void handle_upgrade(ws_conn_t *conn) {
  ws_server_t *s = conn->base;
  size_t resp_len = 0;

  // read from the socket
  if (conn_read(conn, conn->recv_buf) == -1) {
    ws_conn_destroy(conn);
    return;
  };

  // get how much has accumulated so far
  size_t request_buf_len = buf_len(conn->recv_buf);

  // if we are disposing the connection
  // it means that we received a bad request or an internal server error ocurred
  if (is_write_shutdown(conn)) {
    conn->fragments_len += request_buf_len;
    // client sending too much data after shutting down our write end
    if (conn->fragments_len > 8192) {
      ws_conn_destroy(conn);
    }

    // reset the buffer, we discard all data after socket is marked disposing
    buf_reset(conn->recv_buf);
    return;
  }

  // if we still have less than needed bytes
  // stop and wait for more
  if (request_buf_len < conn->needed_bytes) {
    return;
  }

  // if false by the time we write then call shutdown
  bool ok = false;

  uint8_t *headers = buf_peek(conn->recv_buf);

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

          int sec_websocket_extensions_ret =
              get_header((char *)headers, "Sec-WebSocket-Extensions",
                         sec_websocket_extensions, 1024);
          if (sec_websocket_extensions_ret > 0) {
            pmd = true;
          }

          if (!conn->send_buf) {
            conn->send_buf = mirrored_buf_get(conn->base->buffer_pool);
          }

          buf_reset(conn->send_buf);
          buf_put(conn->send_buf, switching_protocols,
                  SWITCHING_PROTOCOLS_HDRS_LEN);
          buf_put(conn->send_buf, accept_key, accept_key_len);
          if (pmd) {
            set_compression_allowed(conn);
            buf_put(conn->send_buf,
                    "\r\nSec-WebSocket-Extensions: permessage-deflate; "
                    "client_no_context_takeover",
                    74);
          }

          buf_put(conn->send_buf, CRLF2, CRLF2_LEN);
          resp_len = buf_len(conn->send_buf);
        } else {
          if (!conn->send_buf) {
            conn->send_buf = mirrored_buf_get(conn->base->buffer_pool);
          }

          size_t max_resp_len = buf_space(conn->send_buf);
          resp_len = s->on_ws_upgrade_req(conn, (char *)headers, accept_key,
                                          max_resp_len,
                                          (char *)buf_peek(conn->send_buf));

          buf_consume(conn->recv_buf, request_buf_len);
          if ((resp_len > 0) & (resp_len <= max_resp_len)) {
            conn->send_buf->wpos += resp_len;
            ok = true;
          } else {
            set_write_shutdown(conn);
            buf_put(conn->send_buf, internal_server_error,
                    INTERNAL_SERVER_ERROR_LEN);
          }
        }

      } else {
        set_write_shutdown(conn);
        if (!conn->send_buf) {
          conn->send_buf = mirrored_buf_get(conn->base->buffer_pool);
        }

        buf_put(conn->send_buf, bad_request, BAD_REQUEST_LEN);
        printf("error parsing http headers: %d\n", ret);
      }

    } else {
      // there's still more data to be read from the network to get the full
      // header

      return;
    }

  } else {
    if (!conn->send_buf) {
      conn->send_buf = mirrored_buf_get(conn->base->buffer_pool);
    }
    set_write_shutdown(conn);
    buf_put(conn->send_buf, bad_request, BAD_REQUEST_LEN);
  }

  if (is_writeable(conn)) {
    int ret = conn_drain_write_buf(conn);
    if (ret == 1) {
      mirrored_buf_put(conn->base->buffer_pool, conn->recv_buf);
      conn->recv_buf = NULL;

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
    }
  } else {
    buf_put(conn->send_buf, bad_request, BAD_REQUEST_LEN);
  }
}

static inline size_t ws_conn_readable_len(ws_conn_t *conn,
                                          mirrored_buf_t *buf) {
  return buf->wpos - buf->rpos - conn->fragments_len;
}

static inline void ws_conn_handle(ws_conn_t *conn) {
  ws_server_t *s = conn->base;

  unsigned int next_read_timeout = time(NULL) + READ_TIMEOUT;

  // total frame header bytes trimmed
  size_t total_trimmed = 0;
  size_t max_allowed_len = s->max_msg_len;

  if (conn_read(conn, conn->recv_buf) == -1) {
    ws_conn_destroy(conn);
    return;
  }

  while (ws_conn_readable_len(conn, conn->recv_buf) - total_trimmed >=
         conn->needed_bytes) {
    // payload start
    uint8_t *frame = conn->recv_buf->buf + conn->recv_buf->rpos +
                     conn->fragments_len + total_trimmed;

    uint8_t fin = frame_get_fin(frame);
    uint8_t opcode = frame_get_opcode(frame);
    bool is_compressed = is_compressed_msg(frame);

    // printf("fragments=%zu\n", conn->state.fragments_len);
    // run general validation checks on the header
    if (((fin == 0) & ((opcode > 2) & (opcode != OP_CONT))) |
        (frame_has_unsupported_reserved_bits_set(conn, frame) == 1) |
        (frame_is_masked(frame) == 0)) {
      ws_conn_destroy(conn);
      return;
    }

    // make sure we can get the full msg
    size_t payload_len = 0;
    size_t frame_buf_len =
        buf_len(conn->recv_buf) - conn->fragments_len - total_trimmed;

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
      return;
    }

    // check that we have atleast the whole frame, otherwise
    // set needed_bytes and exit waiting for more reads from the socket
    if (frame_buf_len < full_frame_len) {
      conn->needed_bytes = full_frame_len;
      goto clean_up_buffer;
    }

    // buf_debug(buf, "buffer");

    uint8_t *msg = frame + mask_offset + 4;
    msg_unmask(msg, frame + mask_offset, payload_len);
    conn->read_timeout = next_read_timeout;
    switch (opcode) {
    case OP_TXT:
    case OP_BIN:
      conn->flags &= ~CONN_RX_BIN;
      conn->flags |= (opcode == OP_BIN) * CONN_RX_BIN;
      // fin and never fragmented
      // this handles both text and binary hence the fallthrough
      if (fin & (!is_fragmented(conn))) {

#ifdef WITH_COMPRESSION

        // printf("payload len = %zu\n", payload_len);

        if (!is_compressed) {
          if (!is_bin(conn) && !utf8_is_valid(msg, payload_len)) {
            ws_conn_destroy(conn);
            return; // TODO(sah): send a Close frame, & call close callback
          }
          s->on_ws_msg(conn, msg, payload_len, is_bin(conn));
          buf_consume(conn->recv_buf, full_frame_len);
          clear_bin(conn);
          conn->needed_bytes = 2;

        } else {
          struct basic_buffer *inflated_buf = conn_basic_buffer(conn);
          ssize_t inflated_sz = inflation_stream_inflate(
              s->istrm, (char *)msg, payload_len, inflated_buf->data,
              inflated_buf->cap, true);

          if (inflated_sz > 0) {
            // printf("%.*s\n", (int)inflated_sz, out);
            // printf("\ninflated_sz = %zi\n", inflated_sz);

            if (!is_bin(conn) &&
                !utf8_is_valid((uint8_t *)inflated_buf->data, inflated_sz)) {
              printf("invalid utf\n");
              ws_conn_destroy(conn);
              return; // TODO(sah): send a Close frame, & call close callback
            }
            s->on_ws_msg(conn, inflated_buf->data, inflated_sz, is_bin(conn));
            buf_consume(conn->recv_buf, full_frame_len);
            conn->needed_bytes = 2;
            clear_bin(conn);
            conn_basic_buffer_dispose(conn);
          } else {
            // TODO handle error
            printf("inflate error\n");
            ws_conn_destroy(conn);
            conn_basic_buffer_dispose(conn);
            return; // TODO(sah): send a Close frame, & call close callback
          }
        }

#else
        if (!is_bin(conn) && !utf8_is_valid(msg, payload_len)) {
          ws_conn_destroy(conn);
          return; // TODO(sah): send a Close frame, & call close callback
        }
        s->on_ws_msg(conn, msg, payload_len, is_bin(conn));
        buf_consume(buf, full_frame_len);
        clear_bin(conn);
        conn->needed_bytes = 2;
#endif /* WITH_COMPRESSION */

        break; /* OP_BIN don't fall through to fragmented msg */
      } else if (fin & (is_fragmented(conn))) {
        // this is invalid because we expect continuation not text or binary
        // opcode
        ws_conn_destroy(conn);
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
        return;
      }

      if (conn->fragments_len + payload_len > max_allowed_len) {
        ws_conn_close(conn, NULL, 0, WS_CLOSE_TOO_LARGE);
        return;
      }

      // set the state to fragmented after validation
      set_fragmented(conn);
      if (is_compressed) {
        set_fragment_compressed(conn);
      }

      if (!s->on_ws_msg_fragment) {

        // place back at the frame start which contains the header & mask
        // we want to get rid of but ensure to subtract by the frame_gap to
        // fill it if it isn't zero
        // printf("%p placing at %zu\n", conn, (uintptr_t)frame-total_trimmed -
        // (uintptr_t)conn->recv_buf->buf);
        memmove(frame - total_trimmed, msg, payload_len);
        conn->fragments_len += payload_len;
        total_trimmed += mask_offset + 4;
        conn->needed_bytes = 2;
        if (fin) {

#ifdef WITH_COMPRESSION

          if (is_fragment_compressed(conn)) {
            clear_fragment_compressed(conn);

            struct basic_buffer *inflated_buf = conn_basic_buffer(conn);

            assert(inflated_buf->len == 0);
            ssize_t inflated_sz = inflation_stream_inflate(
                s->istrm, (char *)buf_peek(conn->recv_buf), conn->fragments_len,
                inflated_buf->data, inflated_buf->cap, true);

            if (inflated_sz) {
              inflated_buf->len += inflated_sz;
              // printf("%s\n", inflated_buf->data);
              // printf("%zu\n", inflated_sz);
              if (!is_bin(conn) &&
                  !utf8_is_valid((uint8_t *)inflated_buf->data, inflated_sz)) {
                ws_conn_destroy(conn);
                conn_basic_buffer_dispose(conn);
                return; // TODO(sah): send a Close frame, & call close callback
              }
              s->on_ws_msg(conn, inflated_buf->data, inflated_sz, is_bin(conn));
            } else {
              ws_conn_destroy(conn);
            }

            conn_basic_buffer_dispose(conn);
          } else {
            if (!is_bin(conn) &&
                !utf8_is_valid(buf_peek(conn->recv_buf), conn->fragments_len)) {
              ws_conn_destroy(conn);
              return; // TODO(sah): send a Close frame, & call close callback
            }
            s->on_ws_msg(conn, buf_peek(conn->recv_buf), conn->fragments_len,
                         is_bin(conn));
          }

#else
          if (!is_bin(conn) &&
              !utf8_is_valid(buf_peek(conn->recv_buf), conn->fragments_len)) {
            ws_conn_destroy(conn);
            return; // TODO(sah): send a Close frame, & call close callback
          }
          s->on_ws_msg(conn, buf_peek(conn->recv_buf), conn->fragments_len,
                       is_bin(conn));
#endif /* WITH_COMPRESSION */

          buf_consume(conn->recv_buf, conn->fragments_len);

          conn->fragments_len = 0;
          clear_fragmented(conn);
          clear_bin(conn);
          conn->needed_bytes = 2;
        }
      } else {
        s->on_ws_msg_fragment(conn, msg, payload_len, fin);
        buf_consume(conn->recv_buf, full_frame_len);
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
        return;
      }
      if (s->on_ws_ping) {
        s->on_ws_ping(conn, msg, payload_len);
      } else {
        // Todo(sah): add some throttling to this
        // a bad client can constantly send pings and we would keep replying
        ws_conn_pong(conn, msg, payload_len);
      }
      if ((conn->fragments_len != 0)) {
        total_trimmed += full_frame_len;
        conn->needed_bytes = 2;
      } else {
        // printf("here\n");
        buf_consume(conn->recv_buf, full_frame_len);
        conn->needed_bytes = 2;
      }

      break;
    case OP_PONG:
      if (payload_len > 125) {
        ws_conn_destroy(conn);
        return;
      }

      if (s->on_ws_pong) {
        // only call the callback if provided
        s->on_ws_pong(conn, msg, payload_len);
      }

      if ((conn->fragments_len != 0)) {
        total_trimmed += total_trimmed;
        conn->needed_bytes = 2;
      } else {
        buf_consume(conn->recv_buf, full_frame_len);
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
        return;
      } else if (payload_len < 2) {
        if (s->on_ws_close) {
          s->on_ws_close(conn, NULL, 0, WS_CLOSE_PROTOCOL);
        } else {
          ws_conn_close(conn, NULL, 0, WS_CLOSE_PROTOCOL);
        }

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
          return;
        }

        if (!utf8_is_valid(msg + 2, payload_len - 2)) {
          ws_conn_destroy(conn);
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
          return;
        }

        if (s->on_ws_close) {
          s->on_ws_close(conn, msg + 2, payload_len - 2, code);
        } else {
          ws_conn_close(conn, NULL, 0, code);
        }

        return;
      }

      break;
    default:
      ws_conn_destroy(conn);
      return;
    }
  } /* loop end */

  size_t move_total;

clean_up_buffer:
  move_total = conn->recv_buf->wpos - conn->recv_buf->rpos -
               conn->fragments_len - total_trimmed;

  if ((move_total != 0) | (total_trimmed != 0)) {
    memmove(conn->recv_buf->buf + conn->recv_buf->rpos + conn->fragments_len,
            conn->recv_buf->buf + conn->recv_buf->rpos + conn->fragments_len +
                total_trimmed,
            move_total);

    conn->recv_buf->wpos =
        conn->recv_buf->rpos + conn->fragments_len + move_total;
  }

  if (!buf_len(conn->recv_buf)) {
    mirrored_buf_put(conn->base->buffer_pool, conn->recv_buf);
    conn->recv_buf = NULL;
  }
}

static void ws_conn_notify_on_writeable(ws_conn_t *conn) {
  clear_writeable(conn);
  conn->base->ev.data.ptr = conn;
  conn->base->ev.events = EPOLLOUT | EPOLLRDHUP;
  ws_server_epoll_ctl(conn->base, EPOLL_CTL_MOD, conn->fd);
}

static int conn_drain_write_buf(ws_conn_t *conn) {
  size_t to_write = buf_len(conn->send_buf);
  ssize_t n = 0;

  if ((!to_write)) {
    return 0;
  }

  n = buf_send(conn->send_buf, conn->fd, MSG_NOSIGNAL);
  if ((n == -1 && errno != EAGAIN) | (n == 0)) {
    ws_conn_destroy(conn);
    return -1;
  }

  if (to_write == n) {
    mirrored_buf_put(conn->base->buffer_pool, conn->send_buf);
    conn->send_buf = NULL;
    set_writeable(conn);
    return 1;
  } else {
    if (is_writeable(conn)) {
      ws_conn_notify_on_writeable(conn);
    }
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
inline int ws_conn_send(ws_conn_t *c, void *msg, size_t n, bool compress) {
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
inline int ws_conn_send_txt(ws_conn_t *c, void *msg, size_t n, bool compress) {
  int stat;
#ifdef WITH_COMPRESSION
  if (!compress) {
    stat = conn_write_frame(c, msg, n, OP_TXT);
  } else {
    struct basic_buffer *deflate_buf = conn_basic_buffer(c);

    ssize_t compressed_len = deflation_stream_deflate(
        c->base->dstrm, msg, n, deflate_buf->data + deflate_buf->len,
        deflate_buf->cap - deflate_buf->len, true);

    stat = conn_write_frame(c, deflate_buf->data + deflate_buf->len,
                            compressed_len, OP_TXT | 0x40);
  }
#else
  stat = conn_write_frame(c, msg, n, OP_TXT);
#endif /* WITH_COMPRESSION */

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
  if (is_closing(conn->flags)) {
    return;
  }

  if (!conn->send_buf) {
    conn->send_buf = mirrored_buf_get(conn->base->buffer_pool);
  }

  uint8_t *buf = buf_peek(conn->send_buf);

  buf[0] = FIN | OP_CLOSE;
  buf[1] = 2 + len;
  buf[2] = (code >> 8) & 0xFF;
  buf[3] = code & 0xFF;
  conn->send_buf->wpos += 4;
  buf_put(conn->send_buf, msg, len);

  conn_drain_write_buf(conn);
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

    if (!conn->send_buf) {
      conn->send_buf = mirrored_buf_get(s->buffer_pool);
    }

    size_t flen = len + hlen;
    if (buf_space(conn->send_buf) > flen) {
      // large sends are a bit more complex to handle
      // we attempt to avoid as much copying as possible
      // using vectored I/O

      if (hlen == 4) {
        uint8_t *hbuf =
            conn->send_buf->buf +
            conn->send_buf->wpos; // place the header in the write buffer
        memset(hbuf, 0, 2);
        hbuf[0] = FIN | op; // Set FIN bit and opcode
        conn->send_buf->wpos += hlen;
        hbuf[1] = PAYLOAD_LEN_16;
        hbuf[2] = (len >> 8) & 0xFF;
        hbuf[3] = len & 0xFF;
        buf_put(conn->send_buf, data, len);
      } else if (hlen == 2) {
        uint8_t *hbuf =
            conn->send_buf->buf +
            conn->send_buf->wpos; // place the header in the write buffer
        memset(hbuf, 0, 2);
        hbuf[0] = FIN | op; // Set FIN bit and opcode
        conn->send_buf->wpos += hlen;
        hbuf[1] = (uint8_t)len;
        buf_put(conn->send_buf, data, len);
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
          buf_put(conn->send_buf, hbuf, hlen);
          buf_put(conn->send_buf, data, len);
        } else {
          ssize_t n;
          size_t total_write;

          if (!buf_len(conn->send_buf)) {
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
            n = buf_write2v(conn->send_buf, conn->fd, vecs, flen);
          } else {
            // here there is data pending in the connection send buffer
            // first iovec points to the connection buffer
            // second points to the stack allocated header
            // third points to the payload data
            struct iovec vecs[3];
            vecs[0].iov_len = buf_len(conn->send_buf);
            vecs[0].iov_base = conn->send_buf->buf + conn->send_buf->rpos;
            vecs[1].iov_base = hbuf;
            vecs[1].iov_len = hlen;
            vecs[2].iov_base = data;
            vecs[2].iov_len = len;
            total_write = flen + vecs[0].iov_len;

            // send of as much as we can and place the rest in the connection
            // buffer
            n = buf_drain_write2v(conn->send_buf, vecs, total_write, NULL,
                                  conn->fd);
          }

          if (n == total_write) {
            mirrored_buf_put(conn->base->buffer_pool, conn->send_buf);
            conn->send_buf = NULL;

            return 1;
          } else if (n == 0 ||
                     ((n == -1) & ((errno != EAGAIN) | (errno != EINTR)))) {
            return -1;
          } else {
            ws_conn_notify_on_writeable(conn);
            return 1;
          }
        }
      }

      conn_drain_write_buf(conn);

      // queue up for writing if not using shared buffer
      // if (wbuf == conn->send_buf) {
      //   // queue it up for writing
      //   server_writeable_conns_append(conn);
      // } else if (is_using_shared(conn)) {
      //   if (conn_drain_write_buf(conn, s->shared_send_buffer) == -1) {
      //     // buf_reset(s->shared_recv_buffer);
      //     ws_conn_destroy(conn);
      //   };
      //   clear_using_shared(conn);
      //   conn->base->shared_send_buffer_owner = NULL;
      // }

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
  struct ws_conn_pool *pool = calloc(1, sizeof(struct ws_conn_pool) +
                                            (nmemb * sizeof(struct buf_node)));
  assert(pool != NULL);
  pool->base = calloc(nmemb, sizeof(ws_conn_t));
  assert(pool->base != NULL);
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

struct basic_buffer_pool *basic_buffer_pool_create(size_t nmemb,
                                                   size_t pmd_bufsz) {
  struct basic_buffer_pool *p = malloc(sizeof(struct basic_buffer_pool));
  assert(p != NULL);
  p->buf_sz = pmd_bufsz;
  p->avb = 0;
  p->inuse = 0;
  p->cap = nmemb;

  p->avb_list = calloc(nmemb, sizeof(struct basic_buffer *));
  assert(p->avb_list != NULL);

  return p;
}

struct basic_buffer *pmd_buf_get(struct basic_buffer_pool *p) {
  while (p->avb) {
    p->inuse++;
    return p->avb_list[--p->avb];
  }

  if (p->inuse + 1 <= p->cap) {
    struct basic_buffer *buf = malloc(sizeof(struct basic_buffer) + p->buf_sz);
    assert(buf != NULL);
    buf->cap = p->buf_sz;
    buf->len = 0;
    p->inuse++;
    return buf;
  }

  return NULL;
}

void pmd_buf_put(struct basic_buffer_pool *p, struct basic_buffer *buf) {
  if (buf) {
    buf->len = 0;
    p->avb_list[p->avb++] = buf;
    p->inuse--;
  }
}

inline bool ws_conn_compression_allowed(ws_conn_t *c) {
  return is_compression_allowed(c);
}

#ifdef WITH_COMPRESSION

static z_stream *inflation_stream_init() {
  z_stream *istrm = calloc(1, sizeof(z_stream));
  assert(istrm != NULL);

  inflateInit2(istrm, -15);
  return istrm;
}

static ssize_t inflation_stream_inflate(z_stream *istrm, char *input,
                                        size_t in_len, char *out,
                                        size_t out_len, bool no_ctx_takeover) {
  // Save off the bytes we're about to overwrite
  char *tail_addr = input + in_len;
  char pre_tail[4];
  memcpy(pre_tail, tail_addr, 4);

  // Append tail to chunk
  unsigned char tail[4] = {0x00, 0x00, 0xff, 0xff};
  memcpy(tail_addr, tail, 4);
  in_len += 4;

  istrm->next_in = (Bytef *)input;
  istrm->avail_in = (unsigned int)in_len;

  int err;
  ssize_t total = 0;
  do {
    // printf("inflating...\n");
    istrm->next_out = (Bytef *)out + total;
    istrm->avail_out = out_len - total;
    err = inflate(istrm, Z_SYNC_FLUSH);
    if ((err == Z_OK) & (istrm->avail_out != 0)) {
      total += out_len - istrm->avail_out;
      break;
    } else {
      fprintf(stderr, "inflate(): %s\n", istrm->msg);
      exit(EXIT_FAILURE);
    }

  } while ((istrm->avail_out == 0) & (total <= out_len));

  if (no_ctx_takeover) {
    inflateReset(istrm);
  }

  // DON'T FORGET TO DO THIS
  memcpy(tail_addr, pre_tail, 4);

  if ((err < 0) || total > out_len) {
    fprintf(stderr, "Decompression error or payload too large %d %zu %zu\n",
            err, total, out_len);

    return err < 0 ? err : -1;
  }

  return total;
}

static z_stream *deflation_stream_init() {
  z_stream *dstrm = calloc(1, sizeof(z_stream));
  assert(dstrm != NULL);

  deflateInit2(dstrm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
               Z_DEFAULT_STRATEGY);
  return dstrm;
}

static ssize_t deflation_stream_deflate(z_stream *dstrm, char *input,
                                        size_t in_len, char *out,
                                        size_t out_len, bool no_ctx_takeover) {

  dstrm->next_in = (Bytef *)input;
  dstrm->avail_in = (unsigned int)in_len;

  int err;
  ssize_t total = 0;

  do {
    // printf("deflating...\n");
    assert(out_len - total >= 6);
    dstrm->next_out = (Bytef *)out + total;
    dstrm->avail_out = out_len - total;

    err = deflate(dstrm, Z_SYNC_FLUSH);
    if (err != Z_OK) {
      break;
    } else if (err == Z_OK && dstrm->avail_out) {
      // printf("done\n");
      total += out_len - dstrm->avail_out;
      break;
    }
    total += out_len - dstrm->avail_out;

  } while (1);

  if (no_ctx_takeover) {
    deflateReset(dstrm);
  }

  return total - 4;
}

#endif /* WITH_COMPRESSION */

// static void buf_pool_destroy(struct buf_pool *p) {
//   long page_size = sysconf(_SC_PAGESIZE);

//   size_t pool_sz = (sizeof(struct buf_pool) +
//                     (p->nmemb * sizeof(struct buf_node)) + page_size - 1) &
//                    ~(page_size - 1);

//   size_t buf_pool_sz = p->buf_sz * p->nmemb * 2;

//   void *pool_mem_addr = ((uint8_t *)p->base) - pool_sz;

//   assert(close(p->fd) == 0);
//   assert(munmap(pool_mem_addr, pool_sz + buf_pool_sz) == 0);
// }

static struct mirrored_buf_pool *mirrored_buf_pool_create(uint32_t nmemb,
                                                          size_t buf_sz) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size == -1) {
    fprintf(stderr, "sysconf(_SC_PAGESIZE): failed to determine page size\n");
    exit(1);
  }

  if (buf_sz % page_size) {
    return NULL;
  }

  size_t pool_sz =
      (sizeof(struct mirrored_buf_pool) + page_size - 1) & ~(page_size - 1);
  size_t buf_pool_sz = buf_sz * nmemb * 2; // size of buffers

  void *pool_mem = mmap(NULL, pool_sz + buf_pool_sz, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (pool_mem == MAP_FAILED) {
    perror("mmap");
    return NULL;
  }

  if (mprotect(pool_mem, pool_sz, PROT_READ | PROT_WRITE) == -1) {
    perror("mprotect");
    return NULL;
  };

  struct mirrored_buf_pool *pool = pool_mem;

  pool->avb = nmemb;
  pool->cap = nmemb;

  pool->mirrored_bufs = calloc(nmemb, sizeof(mirrored_buf_t));
  assert(pool->mirrored_bufs != NULL);

  pool->avb_list = calloc(nmemb, sizeof(mirrored_buf_t *));
  assert(pool->avb_list != NULL);

  pool->fd = memfd_create("buf", 0);
  if (pool->fd == -1) {
    perror("memfd_create");
    return NULL;
  }

  pool->buf_sz = buf_sz;
  pool->nmemb = nmemb;
  pool->base = ((uint8_t *)pool_mem) + pool_sz;

  if (ftruncate(pool->fd, buf_sz * nmemb) == -1) {
    perror("ftruncate");
    return NULL;
  };

  uint32_t i;

  uint8_t *pos = pool->base;
  size_t offset = 0;

  size_t j = nmemb;

  for (i = 0; i < nmemb; ++i) {
    if (mmap(pos, buf_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
             pool->fd, offset) == MAP_FAILED) {
      close(pool->fd);
      return NULL;
    };

    if (mmap(pos + buf_sz, buf_sz, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_FIXED, pool->fd, offset) == MAP_FAILED) {
      close(pool->fd);
      return NULL;
    };

    j--;
    pool->mirrored_bufs[j].buf = pos;
    pool->mirrored_bufs[j].buf_sz = buf_sz;

    offset += buf_sz;
    pos = pos + buf_sz + buf_sz;
  }

  j = nmemb;

  while (j--) {
    pool->avb_list[j] = &pool->mirrored_bufs[j];
  }

  return pool;
}

static mirrored_buf_t *mirrored_buf_get(struct mirrored_buf_pool *bp) {
  if (bp->avb) {
    return bp->avb_list[--bp->avb];
  }

  return NULL;
}

static void mirrored_buf_put(struct mirrored_buf_pool *bp,
                             mirrored_buf_t *buf) {
  if (buf) {
    buf->rpos = 0;
    buf->wpos = 0;
    bp->avb_list[bp->avb++] = buf;
  }
}

static inline int buf_put(mirrored_buf_t *r, const void *data, size_t n) {
  if (buf_space(r) < n) {
    return -1;
  }
  memcpy(r->buf + r->wpos, data, n);
  r->wpos += n;
  return 0;
}

static inline ssize_t buf_send(mirrored_buf_t *r, int fd, int flags) {
  ssize_t n = send(fd, r->buf + r->rpos, buf_len(r), flags);
  r->rpos += (n > 0) * n;

  if (r->rpos == r->wpos) {
    buf_reset(r);
  } else {
    int ovf = (r->rpos > r->buf_sz) * r->buf_sz;
    r->rpos -= ovf;
    r->wpos -= ovf;
  }

  return n;
}

static inline int buf_consume(mirrored_buf_t *r, size_t n) {
  if (buf_len(r) < n) {
    return -1;
  }

  r->rpos += n;

  if (r->rpos == r->wpos) {
    buf_reset(r);
  } else {
    int ovf = (r->rpos > r->buf_sz) * r->buf_sz;
    r->rpos -= ovf;
    r->wpos -= ovf;
  }

  return 0;
}

/*
 * writes two io vectors first is a header and the second is a payload
 * returns total written and copies any leftover if we didn't drain the buffer
 */
static inline ssize_t buf_write2v(mirrored_buf_t *r, int fd,
                                  struct iovec const *iovs,
                                  size_t const total) {
  ssize_t n = writev(fd, iovs, 2);
  // everything was written
  if (n == total) {
    return n;
    // some error happened
  } else if (n == 0 || n == -1) {
    // if temporary error do a copy
    if ((n == -1) & ((errno == EAGAIN) | (errno == EINTR))) {
      buf_put(r, iovs[0].iov_base, iovs[0].iov_len);
      buf_put(r, iovs[1].iov_base, iovs[1].iov_len);
    }

    return n;
    // less than the header was written
  } else if (n < iovs[0].iov_len) {
    buf_put(r, (uint8_t *)iovs[0].iov_base + n, iovs[0].iov_len - n);
    buf_put(r, iovs[1].iov_base, iovs[1].iov_len);
    return n;
  } else {
    // header was written but only part of the payload
    size_t leftover = n - iovs[0].iov_len;
    buf_put(r, (uint8_t *)iovs[1].iov_base + leftover,
            iovs[1].iov_len - leftover);
    return n;
  }
}

static inline ssize_t buf_drain_write2v(mirrored_buf_t *r,
                                        struct iovec const *iovs,
                                        size_t const total,
                                        mirrored_buf_t *rem_dst, int fd) {

  mirrored_buf_t *dst;
  if (rem_dst) {
    dst = rem_dst;
  } else {
    dst = r;
  }

  ssize_t n = writev(fd, iovs, 3);
  // everything was written
  if (n == total) {
    // consume what we drained from the buffer
    buf_consume(r, iovs[0].iov_len);
    return n;
    // some error happened
  } else if (n == 0 || n == -1) {
    // if temporary error do a copy
    if ((n == -1) & ((errno == EAGAIN) | (errno == EINTR))) {
      // copy both header and payload first iov already in the buffer
      buf_put(dst, iovs[1].iov_base, iovs[1].iov_len);
      buf_put(dst, iovs[2].iov_base, iovs[2].iov_len);
    }
    return n;
    // couldn't drain the buffer copy the header and payload
  } else if (n < iovs[0].iov_len) {
    buf_consume(r, n);
    if (rem_dst) {
      buf_move(r, dst, buf_len(r));
    }

    buf_put(dst, iovs[1].iov_base, iovs[1].iov_len);
    buf_put(dst, iovs[2].iov_base, iovs[2].iov_len);
  }
  // drained the buffer but only wrote parts of the new frame
  else if (n > iovs[0].iov_len) {
    ssize_t wrote = n - iovs[0].iov_len;
    buf_consume(r, iovs[0].iov_len);

    // less than header was written
    if (wrote < iovs[1].iov_len) {
      buf_put(dst, (uint8_t *)iovs[1].iov_base + wrote,
              iovs[1].iov_len - wrote);
      buf_put(dst, iovs[2].iov_base, iovs[2].iov_len);
    } else {
      // parts of payload were written
      size_t leftover = wrote - iovs[1].iov_len;
      buf_put(dst, (uint8_t *)iovs[2].iov_base + leftover,
              iovs[2].iov_len - leftover);
    }
  }

  return n;
}

static inline ssize_t buf_recv(mirrored_buf_t *r, int fd, size_t len,
                               int flags) {
  ssize_t n = recv(fd, r->buf + r->wpos, len, flags);

  r->wpos += (n > 0) * n;
  return n;
}

int ws_poller_init(ws_server_t *s) {
  if (!s->user_epoll) {
    s->user_epoll = epoll_create1(O_CLOEXEC);
    if (s->user_epoll == -1) {
      return -1;
    } else {
      s->ev.events = EPOLLIN;
      s->ev.data.ptr = &s->user_epoll;
      ws_server_epoll_ctl(s, EPOLL_CTL_ADD, s->user_epoll);
      return 0;
    }
  }

  return s->user_epoll ? 0 : -1;
}

int ws_pollable_register(ws_server_t *s, int fd, ws_poll_cb_ctx_t *cb_ctx,
                         int events) {
  if (!s->user_epoll || !cb_ctx) {
    return -1;
  }

  s->ev.events = events;
  s->ev.data.ptr = cb_ctx;

  return epoll_ctl(s->user_epoll, EPOLL_CTL_ADD, fd, &s->ev);
}

int ws_pollable_unregister(ws_server_t *s, int fd) {
  if (!s->user_epoll) {
    return -1;
  }

  s->ev.events = 0;
  s->ev.data.ptr = NULL;

  return epoll_ctl(s->user_epoll, EPOLL_CTL_DEL, fd, &s->ev);
}

int ws_pollable_modify(ws_server_t *s, int fd, ws_poll_cb_ctx_t *cb_ctx,
                       int events) {
  if (!s->user_epoll || !cb_ctx) {
    return -1;
  }

  s->ev.events = events;
  s->ev.data.ptr = cb_ctx;

  return epoll_ctl(s->user_epoll, EPOLL_CTL_MOD, fd, &s->ev);
}

static int base64_encode(char *encoded, const char *string, int len) {
  int i;
  char *p;

  const char b64_table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  p = encoded;
  for (i = 0; i < len - 2; i += 3) {
    *p++ = b64_table[(string[i] >> 2) & 0x3F];
    *p++ = b64_table[((string[i] & 0x3) << 4) |
                     ((int)(string[i + 1] & 0xF0) >> 4)];
    *p++ = b64_table[((string[i + 1] & 0xF) << 2) |
                     ((int)(string[i + 2] & 0xC0) >> 6)];
    *p++ = b64_table[string[i + 2] & 0x3F];
  }
  if (i < len) {
    *p++ = b64_table[(string[i] >> 2) & 0x3F];
    if (i == (len - 1)) {
      *p++ = b64_table[((string[i] & 0x3) << 4)];
      *p++ = '=';
    } else {
      *p++ = b64_table[((string[i] & 0x3) << 4) |
                       ((int)(string[i + 1] & 0xF0) >> 4)];
      *p++ = b64_table[((string[i + 1] & 0xF) << 2)];
    }
    *p++ = '=';
  }

  *p++ = '\0';
  return p - encoded;
}

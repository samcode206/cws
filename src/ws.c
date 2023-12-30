/* The MIT License

   Copyright (c) 2023 by Sam H

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
#include "ws.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <time.h>

#ifdef WITH_COMPRESSION
#include <zlib.h>
#endif /* WITH_COMPRESSION */

struct conn_list {
  size_t len;
  size_t cap;
  ws_conn_t **conns;
};

typedef struct ws_timer_t ws_timer_t;

typedef ws_timer_t *timer_queue;

#define P
#define T timer_queue
#include "./lib/pqu.h"

typedef struct ws_timer_t {
  uint64_t expiry_ns;
  void *ctx;
  timeout_cb_t cb;
  bool cancelled;
} ws_timer_t;

struct timer_queue {
  uint64_t cur_time;        // current time (only updated when we need it!)
  uint64_t next_expiration; // next expiry in nano seconds (since cur_time)
  int timer_fd;             // timer fd for epoll
  pqu_timer_queue *pqu_tq;  // min heap of soonest expiring timers
};

typedef struct server {
  size_t max_msg_len;  // max allowed msg length
  size_t max_per_read; // max bytes to read per read call
  ws_on_msg_cb_t on_ws_msg;
  struct mirrored_buf_pool *buffer_pool;
  unsigned int server_time;
  int active_events; // number of active events per epoll_wait call
  int fd;            // server file descriptor
  int epoll_fd;
  void *ctx;
  ws_open_cb_t on_ws_open;

  unsigned open_conns; // open websocket connections
  unsigned max_conns;  // max connections allowed
  ws_accept_cb_t on_ws_accept;
  struct ws_conn_pool *conn_pool;
  ws_drain_cb_t on_ws_drain;
  ws_disconnect_cb_t on_ws_disconnect;
  size_t max_handshake_headers;
  struct ws_conn_handshake *hs;
  ws_handshake_cb_t on_ws_handshake;
  ws_on_timeout_t on_ws_conn_timeout;
  struct ws_server_async_runner *async_runner;
  ws_err_cb_t on_ws_err;
  ws_err_accept_cb_t on_ws_accept_err;
#ifdef WITH_COMPRESSION
  z_stream *istrm;
  z_stream *dstrm;
#endif /* WITH_COMPRESSION */

  int tfd;                // timer fd
  struct timer_queue *tq; // High resolution timer queue
  size_t internal_polls;  // number of internal fds being watched by epoll
  int user_epoll;
  struct epoll_event events[1024];
  struct conn_list pending_timers;
  struct conn_list writeable_conns;
} ws_server_t;

#define SECONDS_PER_TICK 1
#define TIMER_CHECK_TICKS 5
#define READ_TIMEOUT 60
#define BUF_POOL_LONG_AVG_TICKS 256
#define BUF_POOL_GC_TICKS 16

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static inline size_t align_to(size_t n, size_t to) {
  return (n + (to - 1)) & ~(to - 1);
}

struct ws_server_async_runner_buf {
  size_t len;
  size_t cap;
  struct async_cb_ctx **cbs;
};

struct ws_server_async_runner {
  pthread_mutex_t mu;
  int chanfd;
  struct ws_server_async_runner_buf *pending;
  struct ws_server_async_runner_buf *ready;
};

static void server_pending_timers_remove(ws_conn_t *c);

static void server_writeable_conns_append(ws_conn_t *c);

static void ws_conn_proccess_frames(ws_conn_t *conn);

static void ws_server_epoll_ctl(ws_server_t *s, int op, int fd,
                                struct epoll_event *ev);

inline void *ws_server_ctx(ws_server_t *s) { return s->ctx; }

inline size_t ws_server_open_conns(ws_server_t *s) { return s->open_conns; }

inline void ws_server_set_ctx(ws_server_t *s, void *ctx) { s->ctx = ctx; }

static void ws_server_time_update(ws_server_t *s);
static unsigned ws_server_time(ws_server_t *s);

static inline uint_fast8_t io_tmp_err(ssize_t n) {
#if EAGAIN != EWOULDBLOCK
  return (n == -1) &
         ((errno == EAGAIN) | (errno == EWOULDBLOCK) | (errno == EINTR));
#else
  return (n == -1) & ((errno == EAGAIN) | (errno == EINTR));
#endif
}

static size_t get_pagesize() {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    fprintf(stderr, "sysconf(_SC_PAGESIZE): failed to determine page size\n");
    exit(EXIT_FAILURE);
  }

  return (size_t)page_size;
}

/************** connection ***************/

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
#define CONN_RX_PROCESSING_FRAMES (1u << 11)
#define CONN_TX_SENDING_FRAGMENTS (1u << 12)
#define CONN_RX_PAUSED (1u << 13)

typedef struct mirrored_buf_t mirrored_buf_t;
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
  struct dyn_buf *pmd_buf;
#endif /* WITH_COMPRESSION */
};

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

static inline bool is_processing(ws_conn_t *c) {
  return (c->flags & CONN_RX_PROCESSING_FRAMES) != 0;
}

static inline void set_processing(ws_conn_t *c) {
  c->flags |= CONN_RX_PROCESSING_FRAMES;
}

static inline void clear_processing(ws_conn_t *c) {
  c->flags &= ~CONN_RX_PROCESSING_FRAMES;
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

static inline void clear_compression_allowed(ws_conn_t *c) {
  c->flags &= ~CONN_COMPRESSION_ALLOWED;
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

static inline bool is_sending_fragments(ws_conn_t *c) {
  return (c->flags & CONN_TX_SENDING_FRAGMENTS) != 0;
}

static inline void set_sending_fragments(ws_conn_t *c) {
  c->flags |= CONN_TX_SENDING_FRAGMENTS;
}

static inline void clear_sending_fragments(ws_conn_t *c) {
  c->flags &= ~CONN_TX_SENDING_FRAGMENTS;
}

static inline bool is_read_paused(ws_conn_t *c) {
  return (c->flags & CONN_RX_PAUSED) != 0;
}

static inline void set_read_paused(ws_conn_t *c) { c->flags |= CONN_RX_PAUSED; }

static inline void clear_read_paused(ws_conn_t *c) {
  c->flags &= ~CONN_RX_PAUSED;
}

static inline bool is_closed(ws_conn_t *c) { return c->fd == -1; }

static inline void mark_closed(ws_conn_t *c) { c->fd = -1; }

// shutting down our write end of the socket
// must be called AFTER the final write has completed
static int conn_shutdown_wr(ws_conn_t *c) {
  if (shutdown(c->fd, SHUT_WR) == -1) {
    ws_conn_destroy(c, WS_ERR_BAD_HANDSHAKE);
    return -1;
  }

  return 0;
}

struct ws_conn_pool {
  ws_conn_t *base;
  size_t avb;
  size_t cap;
  ws_conn_t **avb_stack;
};

static struct ws_conn_pool *ws_conn_pool_create(size_t nmemb) {
  size_t page_size = get_pagesize();

  size_t pool_sz = align_to(sizeof(struct ws_conn_pool), 64);

  size_t pool_and_avb_stk_sz =
      align_to(pool_sz + (nmemb * sizeof(ws_conn_t *)), 128);

  size_t ws_conns_sz = align_to((sizeof(ws_conn_t) * nmemb), 128);

  size_t total_size = align_to(pool_and_avb_stk_sz + ws_conns_sz, page_size);

  void *pool_mem = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (pool_mem == MAP_FAILED) {
    perror("mmap");
    exit(EXIT_FAILURE);
    return NULL;
  }

  struct ws_conn_pool *pool = pool_mem;
  pool->avb = nmemb;
  pool->cap = nmemb;

  pool->avb_stack = (ws_conn_t **)((uintptr_t)pool_mem + pool_sz);

  pool->base = (ws_conn_t *)((uintptr_t)pool_mem + pool_and_avb_stk_sz);

  assert((uintptr_t)pool + pool_sz == (uintptr_t)pool->avb_stack);
  assert((uintptr_t)pool->avb_stack + (sizeof(ws_conn_t *) * nmemb) <=
         (uintptr_t)pool->base);

  size_t i = nmemb;
  size_t j = 0;

  while (i--) {
    pool->avb_stack[i] = &pool->base[j++];
  }

  return pool;
}

static void server_ws_conn_pool_destroy(ws_server_t *s) {
  size_t nmemb = s->conn_pool->cap;
  size_t page_size = get_pagesize();

  size_t pool_sz = align_to(sizeof(struct ws_conn_pool), 64);

  size_t pool_and_avb_stk_sz =
      align_to(pool_sz + (nmemb * sizeof(ws_conn_t *)), 128);

  size_t ws_conns_sz = align_to((sizeof(ws_conn_t) * nmemb), 128);

  size_t total_size = align_to(pool_and_avb_stk_sz + ws_conns_sz, page_size);

  munmap(s->conn_pool, total_size);

  s->conn_pool = NULL;
}

static struct ws_conn_t *ws_conn_get(struct ws_conn_pool *p) {
  if (p->avb) {
    return p->avb_stack[--p->avb];
  }

  return NULL;
}

static void ws_conn_put(struct ws_conn_pool *p, struct ws_conn_t *c) {
  if (c) {
    p->avb_stack[p->avb++] = c;
  }
}

inline bool ws_conn_compression_allowed(ws_conn_t *c) {
  return is_compression_allowed(c);
}

void ws_conn_pause_read(ws_conn_t *c) {
  // if we aren't currently paused
  if (!is_read_paused(c)) {
    ws_server_t *s = c->base;

    struct epoll_event ev = {
        .events = EPOLLRDHUP,
        .data = {.ptr = c},
    };

    // if we aren't writeable keep EPOLLOUT
    if (!is_writeable(c)) {
      ev.events |= EPOLLOUT;
    }

    ws_server_epoll_ctl(s, EPOLL_CTL_MOD, c->fd, &ev);

    set_read_paused(c);
    clear_processing(c);
  }
}

void ws_conn_resume_reads(ws_conn_t *c) {
  if (is_read_paused(c)) {
    ws_server_t *s = c->base;

    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLRDHUP,
        .data = {.ptr = c},
    };

    if (!is_writeable(c)) {
      ev.events |= EPOLLOUT;
    }

    ws_server_epoll_ctl(s, EPOLL_CTL_MOD, c->fd, &ev);
    clear_read_paused(c);

    // if there is data in the buffer after resuming
    // and the connection upgraded connection isn't the current being processed
    // start processing frames
    if (c->recv_buf && !is_processing(c) && is_upgraded(c)) {
      set_processing(c);
      ws_conn_proccess_frames(c);
      clear_processing(c);
    }
  }
}

void ws_conn_set_read_timeout(ws_conn_t *c, unsigned secs) {
  if ((secs != 0) & !is_closed(c)) {
    c->read_timeout = ws_server_time(c->base) + secs;
  } else {
    c->read_timeout = 0;
  }
}

void ws_conn_set_write_timeout(ws_conn_t *c, unsigned secs) {
  if ((secs != 0) & !is_closed(c)) {
    c->write_timeout = ws_server_time(c->base) + secs;
  } else {
    c->write_timeout = 0;
  }
}

static void mirrored_buf_put(struct mirrored_buf_pool *bp, mirrored_buf_t *buf);

#ifdef WITH_COMPRESSION
struct dyn_buf {
  size_t len;
  size_t cap;
  char data[];
};

static struct dyn_buf *conn_dyn_buf_get(ws_conn_t *c);

static void conn_dyn_buf_dispose(ws_conn_t *c);

#endif /* WITH_COMPRESSION */

void ws_conn_destroy(ws_conn_t *c, unsigned long reason) {
  if (is_closed(c)) {
    return;
  }

  ws_server_t *s = c->base;

  if (c->recv_buf) {
    mirrored_buf_put(c->base->buffer_pool, c->recv_buf);
    c->recv_buf = NULL;
  }
  if (c->send_buf) {
    mirrored_buf_put(c->base->buffer_pool, c->send_buf);
    c->send_buf = NULL;
  }
  server_pending_timers_remove(c);

#ifdef WITH_COMPRESSION
  conn_dyn_buf_dispose(c);
#endif /* WITH_COMPRESSION */

  struct epoll_event ev = {
      0,
  };

  ws_server_epoll_ctl(c->base, EPOLL_CTL_DEL, c->fd, &ev);
  clear_writeable(c);
  set_read_paused(c);
  close(c->fd);
  mark_closed(c);

  // needed_bytes holds the reason
  c->needed_bytes = reason;
  s->on_ws_disconnect(c, reason);

  ws_conn_put(s->conn_pool, c);
  --s->open_conns;
}

inline ws_server_t *ws_conn_server(ws_conn_t *c) { return c->base; }

inline void *ws_conn_ctx(ws_conn_t *c) { return c->ctx; }

inline void ws_conn_set_ctx(ws_conn_t *c, void *ctx) { c->ctx = ctx; }

inline bool ws_conn_msg_bin(ws_conn_t *c) { return is_bin(c); }

int ws_conn_fd(ws_conn_t *c) { return c->fd; }

inline bool ws_conn_is_read_paused(ws_conn_t *c) { return is_read_paused(c); }

const char *ws_conn_err_table[] = {
    "Unkown Error Code",
    "EOF",
    "Read Error",
    "Write Error",
    "Bad Frame Received",
    "Invalid Upgrade Request",
    "Read Timeout Exceeded",
    "Write Timeout Exceeded",
    "Read/Write Timeout Exceeded",
    "Unkown Websocket opcode Received",
    "Decompression Error",
    "Invalid UTF-8 Received",

    "Graceful Websocket Closure",                     // 1000
    "Going Away",                                     // 1001
    "Websocket Protocol Error",                       // 1003
    "Websocket Status Code 1003 Unsupported",         // 1003
    "Websocket Status Code 1005 No Status",           // 1005
    "Websocket abnormal Closure",                     // 1006
    "Websocket Status Code 1007 Invalid Data",        // 1007
    "Websocket Policy Violation",                     // 1008
    "Websocket Message Too Large",                    // 1009
    "Websocket Extension Negotiation Failed",         // 1010
    "Websocket Status Code 1011 Unexpected Condition" // 1011
};

const char *ws_conn_strerror(ws_conn_t *c) {
  if (is_closed(c)) {
    unsigned long err = c->needed_bytes;
    if (err < 990 || err > 1011 || err == 1004) {
      return ws_conn_err_table[0];
    }

    if (err == WS_ERR_READ || err == WS_ERR_WRITE) {
      if (!c->fragments_len) {
        return ws_conn_err_table[1];
      } else {
        return strerror((int)c->fragments_len);
      }
    }

    err = err - 988;

    return ws_conn_err_table[err];
  } else {
    return NULL;
  }
}

/******************** Buffering *********************/

typedef struct mirrored_buf_t {
  size_t rpos;
  size_t wpos;
  size_t buf_sz;
  uint8_t *buf;
} mirrored_buf_t;

struct mirrored_buf_pool {
  int fd;        // memfd_create file descriptor
  size_t buf_sz; // size of each buffer
  uint8_t *pos;  // mmap position
  size_t offset; // file offset
  void *base;    // raw memory
  size_t avb;    // number of buffers available
  size_t cap;    // total buffers
  mirrored_buf_t *
      *avb_stack; // LIFO used for handing out and putting back buffers

  size_t depth_reached; // current max depth reached per tick

  size_t max_depth_since_gc; // max depth reached since last time gc ran

  size_t avg_depth_reached_since_gc; // average depth reached recorded since
                                     // last gc

  size_t ticks; // total ticks for updating metrics

  size_t touched_bufs; // total used since last gc

  size_t avg_depths[BUF_POOL_LONG_AVG_TICKS]; // average depth for the last
                                              // BUF_POOL_LONG_AVG_TICKS

  mirrored_buf_t *mirrored_bufs; // the buffer structures each pointing to their
                                 // respective segment of base
};

static struct mirrored_buf_pool *
mirrored_buf_pool_create(uint32_t nmemb, size_t buf_sz, bool defer_bufs_mmap) {
  size_t page_size = get_pagesize();

  if (buf_sz % page_size) {
    return NULL;
  }

  size_t mirrored_bufs_total_size = nmemb * sizeof(mirrored_buf_t);
  size_t avb_stack_total_size = nmemb * sizeof(mirrored_buf_t *);

  size_t pool_sz =
      align_to((sizeof(struct mirrored_buf_pool) + mirrored_bufs_total_size +
                avb_stack_total_size + 128),
               page_size);

  size_t buf_pool_sz = buf_sz * nmemb * 2; // size of buffers

  void *pool_mem = mmap(NULL, pool_sz + buf_pool_sz, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (pool_mem == MAP_FAILED) {
    perror("mmap");
    exit(EXIT_FAILURE);
    return NULL;
  }

  if (mprotect(pool_mem, pool_sz, PROT_READ | PROT_WRITE) == -1) {
    perror("mprotect");
    exit(EXIT_FAILURE);
    return NULL;
  };

  struct mirrored_buf_pool *pool = pool_mem;

  pool->avb = nmemb;
  pool->cap = nmemb;
  pool->depth_reached = 0;

  pool->mirrored_bufs =
      (mirrored_buf_t *)((uintptr_t)pool_mem +
                         align_to(sizeof(struct mirrored_buf_pool), 32));
  pool->avb_stack =
      (mirrored_buf_t **)((uintptr_t)pool_mem +
                          align_to(sizeof(struct mirrored_buf_pool), 32) +
                          mirrored_bufs_total_size);

  pool->fd = memfd_create("buf", 0);
  if (pool->fd == -1) {
    perror("memfd_create");
    return NULL;
  }

  pool->buf_sz = buf_sz;
  pool->base = ((uint8_t *)pool_mem) + pool_sz;

  if (ftruncate(pool->fd, (off_t)buf_sz * nmemb) == -1) {
    perror("ftruncate");
    exit(EXIT_FAILURE);
    return NULL;
  };

  uint32_t i;

  pool->offset = 0;
  pool->pos = pool->base;

  uint8_t *pos = pool->base;
  size_t offset = 0;

  for (i = 0; i < nmemb; ++i) {
    if (!defer_bufs_mmap) {
      if (mmap(pos, buf_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
               pool->fd, (off_t)offset) == MAP_FAILED) {
        perror("mmap");
        close(pool->fd);
        exit(EXIT_FAILURE);
        return NULL;
      };

      if (mmap(pos + buf_sz, buf_sz, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_FIXED, pool->fd, (off_t)offset) == MAP_FAILED) {
        perror("mmap");
        close(pool->fd);
        exit(EXIT_FAILURE);
        return NULL;
      };

      pool->offset += buf_sz;
      pool->pos = pool->pos + buf_sz + buf_sz;
    }

    pool->mirrored_bufs[i].buf = pos;
    pool->mirrored_bufs[i].buf_sz = buf_sz;

    offset += buf_sz;
    pos = pos + buf_sz + buf_sz;
  }

  i = 0;
  uint32_t j = nmemb;
  while (j--) {
    pool->avb_stack[j] = &pool->mirrored_bufs[i++];
  }

  return pool;
}

static void server_mirrored_buf_pool_destroy(ws_server_t *s) {
  size_t page_size = get_pagesize();

  size_t nmemb = s->buffer_pool->cap;
  size_t buf_sz = s->buffer_pool->buf_sz;

  size_t mirrored_bufs_total_size = nmemb * sizeof(mirrored_buf_t);
  size_t avb_stack_total_size = nmemb * sizeof(mirrored_buf_t *);

  size_t pool_sz =
      align_to((sizeof(struct mirrored_buf_pool) + mirrored_bufs_total_size +
                avb_stack_total_size + 128),
               page_size);

  size_t buf_pool_sz = buf_sz * nmemb * 2; // size of buffers
  size_t total_size = pool_sz + buf_pool_sz;

  for (size_t i = 0; i < s->buffer_pool->cap; i++) {
    if (s->buffer_pool->pos > s->buffer_pool->mirrored_bufs[i].buf) {
      munmap(s->buffer_pool->mirrored_bufs[i].buf, buf_sz);
      munmap(s->buffer_pool->mirrored_bufs[i].buf + buf_sz, buf_sz);
    }

    s->buffer_pool->mirrored_bufs[i].buf = NULL;
  }

  close(s->buffer_pool->fd);

  munmap(s->buffer_pool, total_size);

  s->buffer_pool = NULL;
}

static mirrored_buf_t *mirrored_buf_get(struct mirrored_buf_pool *bp) {
  if (likely(bp->avb)) {
    mirrored_buf_t *b = bp->avb_stack[--bp->avb];
    register size_t current_depth = bp->cap - bp->avb;

    // check if we need to create the memory mappings for the mirrored buffer
    if (bp->pos <= b->buf) {
      uint8_t *pos = bp->pos;
      size_t offset = bp->offset;
      size_t buf_sz = bp->buf_sz;

      void *mem = mmap(pos, buf_sz, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED, bp->fd, (off_t)offset);
      // like we warned this usually fails due to mmap count limit and we have
      // to exit
      if (unlikely(mem == MAP_FAILED)) {
        perror("mmap");
        close(bp->fd);
        exit(EXIT_FAILURE);
        return NULL;
      };

      mem = mmap(pos + buf_sz, buf_sz, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_FIXED, bp->fd, (off_t)offset);

      if (unlikely(mem == MAP_FAILED)) {
        perror("mmap");
        close(bp->fd);
        exit(EXIT_FAILURE);
        return NULL;
      };

      (void)mem;

      bp->offset += buf_sz;
      bp->pos = bp->pos + buf_sz + buf_sz;
    }

    bp->depth_reached =
        current_depth > bp->depth_reached ? current_depth : bp->depth_reached;

    return b;
  }

  // we should NEVER EVER end up here
  // if this does happen we are guarantee to have a bug somewhere
  // we need to exit immediately because this is not within the expected program
  // behavior
  fprintf(stderr, "[PANIC] buffer pool is empty, this should never happen, app "
                  "state is corrupted, shutting down\n");
  exit(EXIT_FAILURE);

  return NULL;
}

static void mirrored_buf_put(struct mirrored_buf_pool *bp,
                             mirrored_buf_t *buf) {
  if (buf) {
    buf->rpos = 0;
    buf->wpos = 0;
    bp->avb_stack[bp->avb++] = buf;
  }
}

static inline size_t buf_len(mirrored_buf_t *r) { return r->wpos - r->rpos; }

static inline void buf_reset(mirrored_buf_t *r) {
  // we can do this because indexes are in the beginning
  memset(r, 0, sizeof(size_t) * 2);
}

static inline size_t buf_space(mirrored_buf_t *r) {
  return r->buf_sz - (r->wpos - r->rpos);
}

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

static inline int buf_put(mirrored_buf_t *r, const void *data, size_t n) {
  if (buf_space(r) < n) {
    return -1;
  }
  memmove(r->buf + r->wpos, data, n);
  r->wpos += n;
  return 0;
}

static inline ssize_t buf_send(mirrored_buf_t *r, int fd, int flags) {
  ssize_t n = send(fd, r->buf + r->rpos, buf_len(r), flags);
  r->rpos += (size_t)((n > 0) * n);

  if (r->rpos == r->wpos) {
    buf_reset(r);
  } else {
    size_t ovf = (r->rpos > r->buf_sz) * r->buf_sz;
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
    size_t ovf = (r->rpos > r->buf_sz) * r->buf_sz;
    r->rpos -= ovf;
    r->wpos -= ovf;
  }

  return 0;
}

static inline void buf_move(mirrored_buf_t *src_b, mirrored_buf_t *dst_b,
                            size_t n) {
  buf_put(dst_b, src_b->buf + src_b->rpos, n);
  buf_consume(src_b, n);
}

static inline void buf_debug(mirrored_buf_t *r, const char *label) {
  printf("%s rpos=%zu wpos=%zu\n", label, r->rpos, r->wpos);
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
  if (n == 0 || n == -1) {
    // if temporary error do a copy
    if (io_tmp_err(n)) {
      buf_put(r, iovs[0].iov_base, iovs[0].iov_len);
      buf_put(r, iovs[1].iov_base, iovs[1].iov_len);
    }

    return n;
    // less than the header was written
  } else if ((size_t)n == total) {
    return n;
    // some error happened
  } else if ((size_t)n < iovs[0].iov_len) {
    buf_put(r, (uint8_t *)iovs[0].iov_base + n, iovs[0].iov_len - (size_t)n);
    buf_put(r, iovs[1].iov_base, iovs[1].iov_len);
    return n;
  } else {
    // header was written but only part of the payload
    size_t leftover = (size_t)n - iovs[0].iov_len;
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
  if (n == 0 || n < 0) {
    // if temporary error do a copy
    if (io_tmp_err(n)) {
      // copy both header and payload first iov already in the buffer
      buf_put(dst, iovs[1].iov_base, iovs[1].iov_len);
      buf_put(dst, iovs[2].iov_base, iovs[2].iov_len);
    }
    return n;

  } else if ((size_t)n == total) {
    // consume what we drained from the buffer
    buf_consume(r, iovs[0].iov_len);
    return n;
    // some error happened
  } else if ((size_t)n < iovs[0].iov_len) {
    buf_consume(r, (size_t)n);
    if (rem_dst) {
      buf_move(r, dst, buf_len(r));
    }

    buf_put(dst, iovs[1].iov_base, iovs[1].iov_len);
    buf_put(dst, iovs[2].iov_base, iovs[2].iov_len);
  }
  // drained the buffer but only wrote parts of the new frame
  else if ((size_t)n > iovs[0].iov_len) {
    size_t wrote = (size_t)n - iovs[0].iov_len;
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

  r->wpos += (size_t)((n > 0) * n);
  return n;
}

static ssize_t conn_readn(ws_conn_t *conn, size_t n) {
  mirrored_buf_t *rb = conn->recv_buf;
  size_t space = buf_space(rb);

#ifdef WITH_COMPRESSION
  space = space > 4 ? space - 4 : 0;
#endif /* WITH_COMPRESSION */

  if (!is_upgraded(conn)) {
    space = space > 1 ? space - 1 : 0;
  }

  if (space < n) {
    ws_conn_destroy(conn, WS_ERR_READ);
    return -1;
  }

  ssize_t ret = buf_recv(rb, conn->fd, n, 0);
  if (ret == 0 || (ret == -1 && errno != EAGAIN && errno != EINTR)) {
    ws_conn_destroy(conn, WS_ERR_READ);
    return -1;
  }

  return ret;
}

static int conn_read(ws_conn_t *conn) {
  mirrored_buf_t *rb = conn->recv_buf;
  // check wether we need to read more first
  if (is_upgraded(conn) &
      (buf_len(conn->recv_buf) - conn->fragments_len >= conn->needed_bytes)) {
    return 0;
  }

  size_t space = buf_space(rb);

#ifdef WITH_COMPRESSION
  space = space > 4 ? space - 4 : 0;
#endif /* WITH_COMPRESSION */

  if (!is_upgraded(conn)) {
    space = space > 1 ? space - 1 : 0;
  }

  if (!space) {
    ws_conn_destroy(conn, WS_ERR_READ);
    return -1;
  }

  size_t max_per_read = conn->base->max_per_read;
  if ((max_per_read < space) & (conn->needed_bytes < max_per_read)) {
    space = max_per_read;
  }

  ssize_t n = buf_recv(rb, conn->fd, space, 0);
  if (n == -1 || n == 0) {
    if (io_tmp_err(n)) {
      return 0;
    }
    conn->fragments_len = (size_t)(n == -1 ? errno : 0);
    ws_conn_destroy(conn, WS_ERR_READ);
    return -1;
  }

  return 0;
}

static void ws_conn_notify_on_writeable(ws_conn_t *conn) {
  clear_writeable(conn);
  struct epoll_event ev = {
      .events = EPOLLOUT | EPOLLRDHUP,
      .data = {.ptr = conn},
  };

  if (is_sending_fragments(conn)) {
    // keep EPOLLIN armed if we are in the sending_fragments state
    // this is so that we can continue to read control frames
    // we usually disable reading if there is backpressure
    // until the client has read what was sent but this case
    // we may be in the sending_fragments state for too long and don't wanna
    // miss any ping/pongs or other messages that may cause a read timeout
    // TODO : add a pause function for when the client wants to stop reading
    // from a connection temporarily
    ev.events |= EPOLLIN;
  } else {
    set_read_paused(conn);
  }

  ws_server_epoll_ctl(conn->base, EPOLL_CTL_MOD, conn->fd, &ev);
}

static int conn_drain_write_buf(ws_conn_t *conn) {
  size_t to_write = buf_len(conn->send_buf);
  ssize_t n = 0;

  if (!to_write) {
    return 0;
  }

  n = buf_send(conn->send_buf, conn->fd, MSG_NOSIGNAL);
  if ((n == -1 && errno != EAGAIN && errno != EINTR) | (n == 0)) {
    conn->fragments_len = (size_t)(n == -1 ? errno : 0);
    ws_conn_destroy(conn, WS_ERR_WRITE);
    return -1;
  }

  // we checked for -1 so it's safe to cast to size_t
  if (to_write == (size_t)n) {
    mirrored_buf_put(conn->base->buffer_pool, conn->send_buf);
    conn->send_buf = NULL;
    set_writeable(conn);
    conn->write_timeout = 0;
    return 1;
  } else {
    if (is_writeable(conn)) {
      ws_conn_notify_on_writeable(conn);
    }
  }

  return 0;
}

static void conn_prep_send_buf(ws_conn_t *conn) {
  if (!conn->send_buf) {
    conn->send_buf = mirrored_buf_get(conn->base->buffer_pool);
  }
}

/******************** Websocket Handshake ********************/

#define CRLF "\r\n"
#define CRLF_LEN 2

static inline bool is_letter(unsigned char byte) {
  return (((byte > 0x60) & (byte < 0x7B)) | ((byte > 0x40) & (byte < 0x5B)));
}

static inline bool is_alpha_numeric_or_hyphen(unsigned char byte) {
  return is_letter(byte) | (byte == '-') | ((byte > 0x2F) & (byte < 0x3A));
}

static bool http_header_field_name_valid(struct http_header *hdr) {
  if (hdr->name == NULL)
    return false;

  size_t len = strlen(hdr->name);
  if (len == 0)
    return false;

  // should start with a letter
  if (!is_letter((unsigned char)hdr->name[0])) {
    return false;
  }

  for (size_t i = 1; i < len; ++i) {
    if (!is_alpha_numeric_or_hyphen((unsigned char)hdr->name[i])) {
      return false;
    }
  }

  return true;
}

static bool http_header_field_value_valid(struct http_header *hdr) {
  if (hdr->val == NULL)
    return false;

  size_t len = strlen(hdr->val);
  if (len == 0)
    return false;

  for (size_t i = 0; i < len; ++i) {
    unsigned char byte = (unsigned char)hdr->val[i];
    // Check if character is a valid visible character or space/horizontal tab
    if (!((byte > 0x1F) | (byte == 0x09))) {
      return false;
    }
  }

  return true;
}

static ssize_t ws_conn_handshake_get_ln(char *line) {
  char *ln_end = strstr(line, CRLF);
  if (!ln_end || *ln_end == '\0')
    return -1;

  ssize_t len = ln_end - line;
  if (len < 0)
    return -1;

  line[len] = '\0';

  return len;
}

static ssize_t
ws_conn_handshake_parse_request_ln(char *line, struct ws_conn_handshake *hs) {
  ssize_t len = ws_conn_handshake_get_ln(line);
  if (len < 14) {
    return -1;
  }

  const char *beginning = line;
  while (*line <= 0x20 && *line != '\0')
    ++line;

  // must be a GET request we are strict about the casing
  // and no space must come before the method
  if (memcmp(line, "GET ", 4) != 0) {
    return -1;
  };

  // skip GET and space after
  line += 4;

  // skip any control chars
  while (*line <= 0x20 && *line != '\0')
    ++line;

  if (*line == '\0')
    return -1;

  // should be at start of path now
  char *path = line;
  if (memcmp(path, "/", 1) != 0) {
    return -1;
  }

  ssize_t remaining = len - (path - beginning);
  /* '/ HTTP/1.1' got to be at least 10 bytes left */
  if (remaining < 10) {
    return -1;
  }

  for (size_t i = 0; i < (size_t)remaining; i++) {
    // look for any ! or less (ctl chars) in the path and stop there
    if ((unsigned char)path[i] < 0x21) {
      // if valid expect this to just be a space followed by protocol version
      char *path_end = path + i;
      if (memcmp(path_end, " HTTP/1.1", 9) == 0) {
        path[i] = '\0';
        hs->path = path;
      } else {
        return -1;
      }
      break;
    }
  }

  return len;
}

static ssize_t ws_conn_handshake_parse_header(char *line,
                                              struct http_header *hdr) {
  ssize_t len = ws_conn_handshake_get_ln(line);
  if (len > 2) {
    char *sep = strchr(line, ':');
    if (*sep == '\0' || sep == NULL)
      return -1;

    sep[0] = '\0'; // nul terminate the header name

    if (sep - line < 1)
      return -1;

    ++sep; // skip nul (previously ':')

    hdr->name = line;
    if (!http_header_field_name_valid(hdr)) {
      fprintf(stderr, "invalid field name\n");
      return -1;
    };

    // while ctl char or space skip
    while (*sep < 0x21) {
      // if not a space then invalid
      if (*sep != 0x20) {
        return -1;
      }
      sep++;
    }

    hdr->val = sep;
    if (!http_header_field_value_valid(hdr)) {
      fprintf(stderr, "invalid field value\n");
      return -1;
    };

  } else {
    return len == 0 ? len : -1;
  }

  return len;
}

static inline bool http_header_name_is(const struct http_header *hdr,
                                       const char *name, size_t n) {
  return strncasecmp(hdr->name, name, n) == 0;
}

static ssize_t base64_encode(char *encoded, const char *string, ssize_t len);

void SHA1(char *hash_out, const char *str, uint32_t len);

static const char magic_str[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

#define MAGIC_STR_LEN 36

static int ws_conn_handshake_parse(char *raw_req, struct ws_conn_handshake *hs,
                                   size_t max_headers) {
  ssize_t n = ws_conn_handshake_parse_request_ln(raw_req, hs);
  if (n < 0) {
    return -1;
  }

  size_t offset = (size_t)n + 2; // skip CRLF
  bool done = false;
  bool sec_websocket_key_found = false;
  bool connection_header_found = false;
  bool upgrade_header_found = false;

  for (size_t i = 0; i < max_headers; ++i) {
    n = ws_conn_handshake_parse_header(raw_req + offset, hs->headers + i);
    if (n == 0) {
      done = true;
      break;
    } else if (n < 0) {
      return -1;
    }

    if (!connection_header_found &&
        http_header_name_is(hs->headers + i, "Connection", 10) &&
        strcasestr(hs->headers[i].val, "upgrade")) {

      connection_header_found = true;

    } else if (!upgrade_header_found &&
               http_header_name_is(hs->headers + i, "Upgrade", 7) &&
               strcasestr(hs->headers[i].val, "websocket")) {

      upgrade_header_found = true;

    } else if (!sec_websocket_key_found &&
               strncasecmp(hs->headers[i].name, "Sec-WebSocket-Key", 17) == 0) {

      // should be 24 bytes
      if (strlen(hs->headers[i].val) != 24)
        return -1;

      char key_with_magic_str[61];
      memcpy(key_with_magic_str, hs->headers[i].val, 24);
      memcpy(key_with_magic_str + 24, magic_str, MAGIC_STR_LEN);
      key_with_magic_str[60] = '\0';

      char hash[20];
      SHA1(hash, key_with_magic_str, 60);
      base64_encode(hs->sec_websocket_accept, (char *)hash, 20);
      sec_websocket_key_found = true;
      // calculate the accept key
      // Sec-WebSocket-Accept
    }
#ifdef WITH_COMPRESSION
    // check for per message deflate start
    else if (http_header_name_is(hs->headers + i, "Sec-WebSocket-Extensions",
                                 24)) {

      if (strcasestr(hs->headers[i].val, "permessage-deflate")) {
        hs->per_msg_deflate_requested = true;
      }
    }
// check for per message deflate end
#endif /* WITH_COMPRESSION */

    hs->header_count += 1;
    offset += (size_t)n + 2; // skip CRLF
  }

  if (!sec_websocket_key_found || !done || !connection_header_found ||
      !upgrade_header_found) {
    return -1;
  }

  return 0;
}

static enum ws_send_status
ws_conn_do_handshake_reply(ws_conn_t *c,
                           struct ws_conn_handshake_response *resp) {
  // bail out if the connection is closed or already upgraded
  if (is_closed(c) || is_upgraded(c)) {
    return WS_SEND_DROPPED_NOT_ALLOWED;
  }

  // we need these or we don't have much of a response
  if (!resp->status || !resp->headers || !resp->header_count) {
    return WS_SEND_DROPPED_NOT_ALLOWED;
  }

  size_t status_len = strlen(resp->status);
  if (status_len < 3) {
    return WS_SEND_DROPPED_NOT_ALLOWED;
  }

  bool upgrade = strstr(resp->status, "101") != NULL;

  // grab the send buffer
  conn_prep_send_buf(c);

  // write first line
  int put_ret;
  put_ret = buf_put(c->send_buf, "HTTP/1.1 ", 9);
  put_ret = buf_put(c->send_buf, resp->status, status_len);
  put_ret = buf_put(c->send_buf, CRLF, CRLF_LEN);

  // write headers
  for (size_t i = 0; i < resp->header_count; ++i) {
    if (resp->headers[i].name == NULL || resp->headers[i].val == NULL ||
        http_header_name_is(resp->headers + i, "Connection", 10) ||
        http_header_name_is(resp->headers + i, "Upgrade", 7) ||
        http_header_name_is(resp->headers + i, "Sec-WebSocket-Extensions",
                            24)) {
      continue;
    }

    put_ret = buf_put(c->send_buf, resp->headers[i].name,
                      strlen(resp->headers[i].name));
    put_ret = buf_put(c->send_buf, ": ", 2);
    put_ret = buf_put(c->send_buf, resp->headers[i].val,
                      strlen(resp->headers[i].val));
    put_ret = buf_put(c->send_buf, CRLF, CRLF_LEN);
  }

  put_ret = buf_put(c->send_buf, "Upgrade: websocket\r\n", 20);
  put_ret = buf_put(c->send_buf, "Connection: Upgrade\r\n", 21);

#ifdef WITH_COMPRESSION
  if (upgrade && resp->per_msg_deflate && is_compression_allowed(c)) {
    put_ret =
        buf_put(c->send_buf,
                "Sec-WebSocket-Extensions: permessage-deflate; "
                "client_no_context_takeover; server_no_context_takeover\r\n",
                102);

  } else {
    // clear it incase it was set when we saw the header
    // in the request and WITH_COMPRESSION is defined
    clear_compression_allowed(c);
  }
#endif /* WITH_COMPRESSION */

  // end of headers
  put_ret = buf_put(c->send_buf, CRLF, CRLF_LEN);

  // if we have a body
  // ignore if this is an upgrade request
  if (!upgrade && resp->body) {
    size_t body_len = strlen(resp->body);
    // add response body
    put_ret = buf_put(c->send_buf, resp->body, body_len);
  }

  // check if put failed (it only fails when no space is available)
  if (put_ret == -1) {
    mirrored_buf_put(c->base->buffer_pool, c->send_buf);
    c->send_buf = NULL;
    return WS_SEND_DROPPED_TOO_LARGE;
  }

  if (!upgrade) {
    // we are going to close the connection after sending the response
    set_write_shutdown(c);
  }

  int ret = conn_drain_write_buf(c);
  if (ret == 1) {
    if (upgrade) {
      clear_http_get_request(c);
      set_upgraded(c);
      c->base->on_ws_open(c);
    } else {
      // everything written to the socket
      // shutdown our write end
      if (shutdown(c->fd, SHUT_WR) == -1) {
        if (c->base->on_ws_err) {
          int err = errno;
          c->base->on_ws_err(c->base, err);
        } else {
          perror("shutdown");
        }
      };
    }
  } else if (ret == 0) {
    return WS_SEND_OK_BACKPRESSURE;
  } else {
    return WS_SEND_FAILED;
  }

  return WS_SEND_OK;
}

static void
ws_conn_handshake_send_default_response(ws_conn_t *conn,
                                        struct ws_conn_handshake *hs) {
#define WS_CONN_HANDSHAKE_DEFAULT_RESP_HEADER_COUNT 2
  ws_server_t *s = conn->base;

  struct http_header resp_headers[WS_CONN_HANDSHAKE_DEFAULT_RESP_HEADER_COUNT] =
      {
          {
              "Server",
              "cws",
          },
          {
              "Sec-WebSocket-Accept",
              hs->sec_websocket_accept,
          },
      };

  struct ws_conn_handshake_response r = {
      .per_msg_deflate = true, // keep this true it will be ignored if
                               // not supported or not requested
      .body = NULL,
      .status = WS_HANDSHAKE_STATUS_101,
      .headers = resp_headers,
      .header_count = WS_CONN_HANDSHAKE_DEFAULT_RESP_HEADER_COUNT,
  };

  mirrored_buf_put(s->buffer_pool, conn->recv_buf);
  conn->recv_buf = NULL;
  ws_conn_do_handshake_reply(conn, &r);
}

static void handle_bad_request(ws_conn_t *conn) {
  struct http_header resp_headers[3] = {
      {
          "Content-Length",
          "0",
      },
      {
          "Connection",
          "close",
      },
      {
          "Server",
          "cws",
      },
  };

  struct ws_conn_handshake_response r = {
      .status = WS_HANDSHAKE_STATUS_400,
      .headers = resp_headers,
      .header_count = 3,
  };

  mirrored_buf_put(conn->base->buffer_pool, conn->recv_buf);
  conn->recv_buf = NULL;
  set_write_shutdown(conn);
  ws_conn_do_handshake_reply(conn, &r);
}

static void ws_conn_do_handshake(ws_conn_t *conn) {
  ws_server_t *s = conn->base;
  // read from the socket
  if (conn_read(conn) == -1) {
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
      ws_conn_destroy(conn, WS_ERR_BAD_HANDSHAKE);
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

  char *headers = (char *)buf_peek(conn->recv_buf);
  headers[request_buf_len] = '\0';

  // make sure it's a GET request
  bool is_get_request = is_http_get_request(conn);
  if (!is_get_request) {
    if (memcmp((char *)headers, "GET ", 4) == 0) {
      set_http_get_request(conn);
      is_get_request = true;
      // Sec-WebSocket-Accept:s3pPLMBiTxaQ9kYGzzhZRbK+xOo= is 49 bytes and
      // that's the absolute minimum (practically still higher because there
      // will be other headers)
      conn->needed_bytes += 49;
    };
  };

  // if after the first read we still don't have a GET request
  // then we have a bad handshake
  if (!is_get_request) {
    handle_bad_request(conn);
    return;
  }

  // only start processing when the final header has arrived
  bool header_end_reached =
      memcmp((char *)headers + request_buf_len - 4, CRLF CRLF, 4) == 0;

  if (!header_end_reached) {
    // wait for more
    return;
  }

  struct ws_conn_handshake *hs = s->hs;
  hs->header_count = 0;
  if (ws_conn_handshake_parse((char *)headers, hs, s->max_handshake_headers) ==
      0) {
#ifdef WITH_COMPRESSION
    if (hs->per_msg_deflate_requested) {
      set_compression_allowed(conn);
    }
#endif /* WITH_COMPRESSION */

    conn->needed_bytes = 2;
    conn->read_timeout = ws_server_time(s) + READ_TIMEOUT;
    mirrored_buf_put(s->buffer_pool, conn->recv_buf);
    conn->recv_buf = NULL;
    if (s->on_ws_handshake) {
      s->on_ws_handshake(conn, hs);
    } else {
      ws_conn_handshake_send_default_response(conn, hs);
    }

  } else {
    handle_bad_request(conn);
    return;
  }
}

inline enum ws_send_status
ws_conn_handshake_reply(ws_conn_t *c, struct ws_conn_handshake_response *resp) {
  return ws_conn_do_handshake_reply(c, resp);
}

const struct http_header *
ws_conn_handshake_header_find(struct ws_conn_handshake *hs, const char *name) {
  size_t count = hs->header_count;
  struct http_header *headers = hs->headers;

  if (count) {
    while (count--) {
      if (strcasecmp(headers[count].name, name) == 0) {
        return &headers[count];
      }
    }
  }

  return NULL;
}

static unsigned utf8_is_valid(uint8_t *str, size_t n) {
  uint8_t *end = str + n;
  while (str < end) {
    // Check for ASCII optimization
    uint32_t tmp;
    if (str + 4 <= end) {
      memcpy(&tmp, str, 4);
      if ((tmp & 0x80808080) == 0) {
        str += 4;
        continue;
      }
    }

    // ASCII characters
    while (!(*str & 0x80) && ++str < end)
      ;

    // Multi-byte characters
    if (str == end)
      return 1;
    if ((str[0] & 0x60) == 0x40) { // 2-byte sequence
      if (str + 1 >= end || (str[1] & 0xc0) != 0x80 || (str[0] & 0xfe) == 0xc0)
        return 0;
      str += 2;
    } else if ((str[0] & 0xf0) == 0xe0) { // 3-byte sequence
      if (str + 2 >= end || (str[1] & 0xc0) != 0x80 ||
          (str[2] & 0xc0) != 0x80 ||
          (str[0] == 0xe0 && (str[1] & 0xe0) == 0x80) ||
          (str[0] == 0xed && (str[1] & 0xe0) == 0xa0))
        return 0;
      str += 3;
    } else if ((str[0] & 0xf8) == 0xf0) { // 4-byte sequence
      if (str + 3 >= end || (str[1] & 0xc0) != 0x80 ||
          (str[2] & 0xc0) != 0x80 || (str[3] & 0xc0) != 0x80 ||
          (str[0] == 0xf0 && (str[1] & 0xf0) == 0x80) ||
          (str[0] == 0xf4 && str[1] > 0x8f) || str[0] > 0xf4)
        return 0;
      str += 4;
    } else {
      return 0;
    }
  }
  return 1;
}

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static ssize_t base64_encode(char *encoded, const char *string, ssize_t len) {
  ssize_t i;
  char *p;

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

/******************** Websocket Protocol ********************/

#define FIN 0x80
#define OP_CONT 0x0
#define OP_CLOSE 0x8

static inline uint_fast8_t frame_fin(const unsigned char *buf) {
  return (buf[0] >> 7) & 0x01;
}

static inline uint_fast8_t frame_opcode(const unsigned char *buf) {
  return buf[0] & 0x0F;
}

static unsigned frame_decode_payload_len(uint8_t *buf, size_t rbuf_len,
                                         size_t *res) {
  uint8_t raw_len = buf[1] & 0X7F;
  *res = raw_len;

  switch (raw_len) {
  case 126:
    if (rbuf_len > 3) {
      *res = (buf[2] << 8) | buf[3];
    } else {
      return 4;
    }
    break;
  case 127:
    if (rbuf_len > 9) {
      *res = ((uint64_t)buf[2] << 56) | ((uint64_t)buf[3] << 48) |
             ((uint64_t)buf[4] << 40) | ((uint64_t)buf[5] << 32) |
             ((uint64_t)buf[6] << 24) | ((uint64_t)buf[7] << 16) |
             ((uint64_t)buf[8] << 8) | (uint64_t)buf[9];
    } else {
      return 10;
    }
    break;
  }

  return 0;
}

static inline bool is_compressed_msg(uint8_t const *buf) {
  return (buf[0] & 0x40) != 0;
}

static inline int frame_has_unsupported_reserved_bits_set(ws_conn_t *c,
                                                          uint8_t const *buf) {
  return (buf[0] & 0x10) != 0 || (buf[0] & 0x20) != 0 ||
         ((buf[0] & 0x40) != 0 && !is_compression_allowed(c));
}

static inline uint32_t frame_is_masked(const unsigned char *buf) {
  return (buf[1] >> 7) & 0x01;
}

static inline size_t frame_get_header_len(size_t const n) {
  return (size_t)2 + ((n > (size_t)125) * (size_t)2) +
         ((n > (size_t)0xFFFF) * (size_t)6);
}

static void msg_unmask(uint8_t *restrict src, uint8_t const *restrict mask,
                       size_t const n) {

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
  size_t count = n >> 3;

  while (count) {
    memcpy(&chunk, src + i, 8);
    chunk ^= mask64;
    memcpy(src + i, &chunk, 8);
    i += 8;
    count--;
  }
}

static inline uint_fast8_t frame_valid(ws_conn_t *c, uint8_t const *frame,
                                       uint_fast8_t fin, uint_fast8_t opcode) {
  if ((((fin == 0) & ((opcode > 2) & (opcode != OP_CONT))) |
       (frame_has_unsupported_reserved_bits_set(c, frame) == 1) |
       (frame_is_masked(frame) == 0)) == 0) {
    return 1;
  } else {
    ws_conn_destroy(c, WS_ERR_BAD_FRAME);
    return 0;
  }
}

// loop unrolling version, maybe faster for smaller messages??
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

#ifdef WITH_COMPRESSION
static int ws_conn_handle_compressed_frame(ws_conn_t *conn, uint8_t *data,
                                           size_t payload_len);

#endif /* WITH_COMPRESSION */

static void ws_conn_proccess_frames(ws_conn_t *conn) {
  ws_server_t *s = conn->base;

  unsigned int next_read_timeout = ws_server_time(s) + READ_TIMEOUT;

  // total frame header bytes trimmed
  size_t total_trimmed = 0;
  size_t max_allowed_len = s->max_msg_len;

  if (conn_read(conn) == -1) {
    return;
  }

  for (;;) {
    if ((!is_read_paused(conn) & (conn->recv_buf != NULL)) &&
        buf_len(conn->recv_buf) - conn->fragments_len - total_trimmed >=
            conn->needed_bytes) {

      // payload start
      uint8_t *frame = conn->recv_buf->buf + conn->recv_buf->rpos +
                       conn->fragments_len + total_trimmed;

      uint_fast8_t fin = frame_fin(frame);
      uint_fast8_t opcode = frame_opcode(frame);
      bool is_compressed = is_compressed_msg(frame);

      if (frame_valid(conn, frame, fin, opcode)) {
        // make sure we can get the full msg
        size_t payload_len = 0;
        size_t frame_buf_len =
            buf_len(conn->recv_buf) - conn->fragments_len - total_trimmed;

        // check if we need to do more reads to get the msg length
        unsigned int missing_header_len =
            frame_decode_payload_len(frame, frame_buf_len, &payload_len);
        if (missing_header_len) {
          size_t remaining = missing_header_len - frame_buf_len;
          ssize_t rn = 0;

          if (!is_fragmented(conn)) {
            rn = conn_readn(conn, remaining);
            if (rn == -1) {
              return;
            }
          }

          // we made sure it's not -1 so it's safe to cast
          if ((size_t)rn != remaining) {
            // wait for atleast remaining of the header
            conn->needed_bytes = missing_header_len;
            goto clean_up_buffer;
          } else {
            missing_header_len =
                frame_decode_payload_len(frame, frame_buf_len, &payload_len);
            assert(missing_header_len == 0);
          }
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
          size_t remaining = full_frame_len - frame_buf_len;
          ssize_t rn = 0;

          if (!is_fragmented(conn)) {
            rn = conn_readn(conn, remaining);
            if (rn == -1) {
              return;
            }
          }
          // we made sure it's not -1 so it's safe to cast
          if ((size_t)rn != remaining) {
            conn->needed_bytes = full_frame_len;
            goto clean_up_buffer;
          } else {
            frame_buf_len += (size_t)rn;
          }
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
            if (!total_trimmed) {
              buf_consume(conn->recv_buf, full_frame_len);
            } else {
              total_trimmed += full_frame_len;
            }
            conn->needed_bytes = 2;

#ifdef WITH_COMPRESSION
            if (is_compressed) {
              if (ws_conn_handle_compressed_frame(conn, msg, payload_len) ==
                  0) {
                continue;
              } else {
                return;
              }
            }
#endif /* WITH_COMPRESSION */
            if (!is_bin(conn) && !utf8_is_valid(msg, payload_len)) {
              ws_conn_destroy(conn, WS_ERR_INVALID_UTF8);
              return; // TODO(sah): send a Close frame, & call close callback
            }

            s->on_ws_msg(conn, msg, payload_len, opcode);
            clear_bin(conn);

            break; /* OP_BIN don't fall through to fragmented msg */
          } else if (fin & (is_fragmented(conn))) {
            // this is invalid because we expect continuation not text or binary
            // opcode
            ws_conn_destroy(conn, WS_CLOSE_PROTOCOL);
            return;
          }
        // fall through
        case OP_CONT:
          // accumulate bytes and increase fragments_len

          // move bytes over
          // call the callback
          // reset
          // can't send cont as first fragment
          if ((opcode == OP_CONT) & (!is_fragmented(conn))) {
            ws_conn_destroy(conn, WS_CLOSE_PROTOCOL);
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

          // place back at the frame start which contains the header & mask
          // we want to get rid of but ensure to subtract by the frame_gap to
          // fill it if it isn't zero
          // printf("%p placing at %zu\n", conn, (uintptr_t)frame-total_trimmed
          // - (uintptr_t)conn->recv_buf->buf);
          memmove(frame - total_trimmed, msg, payload_len);
          conn->fragments_len += payload_len;
          total_trimmed += mask_offset + 4;
          conn->needed_bytes = 2;
          if (fin) {
#ifdef WITH_COMPRESSION
            if (is_fragment_compressed(conn)) {
              if (ws_conn_handle_compressed_frame(conn,
                                                  buf_peek(conn->recv_buf),
                                                  conn->fragments_len) == 0) {
                continue;
              } else {
                return;
              }
            }
#endif /* WITH_COMPRESSION */

            uint8_t *msg = buf_peek(conn->recv_buf);
            buf_consume(conn->recv_buf, conn->fragments_len);
            if (!is_bin(conn) && !utf8_is_valid(msg, conn->fragments_len)) {
              ws_conn_destroy(conn, WS_ERR_INVALID_UTF8);
              return; // TODO(sah): send a Close frame, & call close callback
            }
            s->on_ws_msg(conn, msg, conn->fragments_len,
                         is_bin(conn) ? OP_BIN : OP_TXT);

            conn->fragments_len = 0;
            clear_fragmented(conn);
            clear_bin(conn);
            conn->needed_bytes = 2;
          }

          break;
        case OP_PING:
        case OP_PONG:
          if ((conn->fragments_len != 0)) {
            total_trimmed += full_frame_len;
          } else {
            buf_consume(conn->recv_buf, full_frame_len);
          }
          conn->needed_bytes = 2;
          if (payload_len > 125) {
            ws_conn_destroy(conn, WS_CLOSE_PROTOCOL);
            return;
          }
          s->on_ws_msg(conn, msg, payload_len, opcode);
          break;
        case OP_CLOSE:
          if (!payload_len) {
            ws_conn_close(conn, NULL, 0, WS_CLOSE_NORMAL);
            return;
          } else if (payload_len < 2) {
            ws_conn_close(conn, NULL, 0, WS_CLOSE_PROTOCOL);
            return;
          } else {
            uint16_t code = WS_CLOSE_NO_STATUS;
            if (payload_len > 125) {
              ws_conn_destroy(conn, WS_CLOSE_PROTOCOL);
              return;
            }

            if (!utf8_is_valid(msg + 2, payload_len - 2)) {
              ws_conn_destroy(conn, WS_ERR_INVALID_UTF8);
              return;
            };

            code = (msg[0] << 8) | msg[1];
            if (code < 1000 || code == 1004 || code == 1100 || code == 1005 ||
                code == 1006 || code == 1015 || code == 1016 || code == 2000 ||
                code == 2999) {
              ws_conn_close(conn, NULL, 0, WS_CLOSE_PROTOCOL);
              return;
            }

            ws_conn_close(conn, NULL, 0, code);
            return;
          }

          break;
        default:
          ws_conn_destroy(conn, WS_UNKNOWN_OPCODE);
          return;
        }
      } else {
        return;
      }

    } else {
      break;
    }
  } /* loop end */

  if ((conn->send_buf != NULL) & (is_writeable(conn)) & (!is_closed(conn))) {
    conn_drain_write_buf(conn);
  }

  size_t move_total;

clean_up_buffer:
  if (conn->recv_buf) {
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
}

static int conn_write_large_frame(ws_conn_t *conn, void *data, size_t len,
                                  uint8_t opAndFinOpts) {

  size_t hlen = 10;
  size_t flen = len + 10;

  // large send
  uint8_t hbuf[hlen]; // place the header on the stack
  memset(hbuf, 0, 2);
  hbuf[0] = opAndFinOpts;
  hbuf[1] = 127;
  hbuf[2] = (uint8_t)(len >> 56) & 0xFF;
  hbuf[3] = (uint8_t)(len >> 48) & 0xFF;
  hbuf[4] = (uint8_t)(len >> 40) & 0xFF;
  hbuf[5] = (uint8_t)(len >> 32) & 0xFF;
  hbuf[6] = (uint8_t)(len >> 24) & 0xFF;
  hbuf[7] = (uint8_t)(len >> 16) & 0xFF;
  hbuf[8] = (uint8_t)(len >> 8) & 0xFF;
  hbuf[9] = (uint8_t)len & 0xFF;

  if (is_writeable(conn)) {
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
      n = buf_drain_write2v(conn->send_buf, vecs, total_write, NULL, conn->fd);
    }

    if (n == 0 || ((n == -1) & ((errno != EAGAIN) & (errno != EINTR)))) {
      return WS_SEND_FAILED;
    } else if ((size_t)n >= total_write) {
      mirrored_buf_put(conn->base->buffer_pool, conn->send_buf);
      conn->send_buf = NULL;
      return WS_SEND_OK;
    } else {
      ws_conn_notify_on_writeable(conn);
      return WS_SEND_OK_BACKPRESSURE;
    }

  } else {
    buf_put(conn->send_buf, hbuf, hlen);
    buf_put(conn->send_buf, data, len);
    // data was queued but there's some backpressure built up
    return WS_SEND_OK_BACKPRESSURE;
  }
}

static int conn_write_frame(ws_conn_t *conn, void *data, size_t len,
                            uint8_t opAndFinOpts) {

  if (!is_closed(conn)) {
    size_t hlen = frame_get_header_len(len);

    conn_prep_send_buf(conn);

    size_t flen = len + hlen;

    if (buf_space(conn->send_buf) < flen) {
      // if we drain would we be able to fit the msg?
      if (conn->send_buf->buf_sz >= flen) {
        if (is_writeable(conn)) {
          int ret = conn_drain_write_buf(conn);
          switch (ret) {
          case -1:
            return WS_SEND_FAILED;
            break;
          default:
            conn_prep_send_buf(conn);
            // we couldn't drain enough to fit the msg
            // but the buffer can still hold it once drained
            if (buf_space(conn->send_buf) < flen) {
              return WS_SEND_DROPPED_NEEDS_DRAIN;
            }
            break;
          }
        } else {
          // we can't write to the socket now due to backpressure
          // but once we have drained the buffer and got some more space
          // we may try again
          return WS_SEND_DROPPED_NEEDS_DRAIN;
        }
      } else {
        // put the buffer back if it's empty
        if (buf_len(conn->send_buf) == 0) {
          mirrored_buf_put(conn->base->buffer_pool, conn->send_buf);
          conn->send_buf = NULL;
        }
        // this msg is larger than the send buffer
        // and must be fragmented
        if (opAndFinOpts & OP_TXT || opAndFinOpts & OP_BIN) {
          return WS_SEND_DROPPED_NEEDS_FRAGMENTATION;
        }

        // this is a large ping/pong/close frame
        return WS_SEND_DROPPED_TOO_LARGE;
      }
    }

    if (hlen == 4) {
      uint8_t *hbuf =
          conn->send_buf->buf +
          conn->send_buf->wpos; // place the header in the write buffer
      memset(hbuf, 0, 2);
      hbuf[0] = opAndFinOpts;
      conn->send_buf->wpos += hlen;
      hbuf[1] = 126;
      hbuf[2] = (len >> 8) & 0xFF;
      hbuf[3] = len & 0xFF;
      buf_put(conn->send_buf, data, len);
    } else if (hlen == 2) {
      uint8_t *hbuf =
          conn->send_buf->buf +
          conn->send_buf->wpos; // place the header in the write buffer
      memset(hbuf, 0, 2);
      hbuf[0] = opAndFinOpts;
      conn->send_buf->wpos += hlen;
      hbuf[1] = (uint8_t)len;
      buf_put(conn->send_buf, data, len);
    } else {
      return conn_write_large_frame(conn, data, len, opAndFinOpts);
    }

    return WS_SEND_OK;

  } else {
    // the connection is in a closing state
    return WS_SEND_FAILED;
  }
}

// check the status of writing the frame in the buffer
// and move on to queuing the write if that went ok
static inline void ws_conn_do_put(ws_conn_t *c, int stat) {
  // if we are sending to another connection outside of the currently
  // processed connection in the event loop, we have to queue up the request
  // so we really send it to the other side this does hold on to a buffer for
  // longer than ideal but can be useful for certain cases where a bunch of
  // small messages are emmitted in a short time and we wanna send them all in
  // a single syscall (if possible)
  // WS_SEND_OK_BACKPRESSURE isn't included here because
  // the socket is already waiting for EPOLLOUT so we don't need to queue it
  if ((stat == WS_SEND_OK) & (c->send_buf != NULL) & (!is_processing(c))) {
    server_writeable_conns_append(c);
  } else if (stat == WS_SEND_FAILED) {
    // destroy if this is a failure
    ws_conn_destroy(c, WS_CLOSE_ABNORMAL);
  }
}

static inline int ws_conn_do_send(ws_conn_t *c, int stat) {
  // WS_SEND_OK_BACKPRESSURE isn't included here because
  // the socket is already waiting for EPOLLOUT so we don't need to queue it
  if ((stat == WS_SEND_OK) & (c->send_buf != NULL) & is_writeable(c)) {
    int drain_ret = conn_drain_write_buf(c);
    if (drain_ret == 0) {
      return WS_SEND_OK_BACKPRESSURE;
    } else if (drain_ret == -1) {
      return WS_SEND_FAILED;
    }

  } else if (stat == WS_SEND_FAILED) {
    ws_conn_destroy(c, WS_CLOSE_ABNORMAL);
  }
  return stat;
}

#ifdef WITH_COMPRESSION

static ssize_t deflation_stream_deflate(z_stream *dstrm, char *input,
                                        unsigned in_len, char *out,
                                        unsigned out_len, bool no_ctx_takeover);

#endif /* WITH_COMPRESSION */

static inline int conn_write_msg(ws_conn_t *c, void *msg, size_t n, uint8_t op,
                                 bool compress) {

#ifndef WITH_COMPRESSION
  (void)compress; // suppress unused warning
#endif            /* WITH_COMPRESSION */

  if (is_sending_fragments(c)) {
    return WS_SEND_DROPPED_NOT_ALLOWED;
  }

  int stat;
#ifdef WITH_COMPRESSION
  if (!compress || !is_compression_allowed(c)) {
    stat = conn_write_frame(c, msg, n, op);
  } else {
    struct dyn_buf *deflate_buf = conn_dyn_buf_get(c);

    ssize_t compressed_len = deflation_stream_deflate(
        c->base->dstrm, msg, (unsigned)n, deflate_buf->data + deflate_buf->len,
        (unsigned)deflate_buf->cap - (unsigned)deflate_buf->len, true);
    if (compressed_len > 0) {
      stat = conn_write_frame(c, deflate_buf->data + deflate_buf->len,
                              (size_t)compressed_len, op | 0x40);

      if (!deflate_buf->len) {
        conn_dyn_buf_dispose(c);
      }

    } else {
      return WS_SEND_FAILED; // compression error
    }
  }
#else
  stat = conn_write_frame(c, msg, n, op);
#endif /* WITH_COMPRESSION */

  return stat;
}

enum ws_send_status ws_conn_put_msg(ws_conn_t *c, void *msg, size_t n,
                                    uint8_t opcode, bool hint_compress) {
  int stat;
  switch (opcode) {
  case OP_TXT:
  case OP_BIN:
    stat = conn_write_msg(c, msg, n, FIN | opcode, hint_compress);
    ws_conn_do_put(c, stat);
    return stat;

  case OP_PING:
  case OP_PONG:
    stat = conn_write_frame(c, msg, n, FIN | opcode);
    ws_conn_do_put(c, stat);
    return stat;

  default:
    return WS_SEND_FAILED;
  }
}

enum ws_send_status ws_conn_send_msg(ws_conn_t *c, void *msg, size_t n,

                                     uint8_t opcode, bool hint_compress) {
  int stat;
  switch (opcode) {
  case OP_TXT:
  case OP_BIN:
    stat = conn_write_msg(c, msg, n, FIN | opcode, hint_compress);
    return ws_conn_do_send(c, stat);

  case OP_PING:
  case OP_PONG:
    stat = conn_write_frame(c, msg, n, FIN | opcode);
    return ws_conn_do_send(c, stat);

  default:
    return WS_SEND_FAILED;
  }
}

void ws_conn_flush_pending(ws_conn_t *c) {
  if (!is_closed(c) & (c->send_buf != NULL) & is_writeable(c)) {
    conn_drain_write_buf(c);
  }
}

size_t ws_conn_max_sendable_len(ws_conn_t *c) {
  if (!is_closed(c)) {
    if (c->send_buf != NULL) {
      size_t space = buf_space(c->send_buf);
      if (space > 10) {
        return space - 10; // accounts for max header size (server frame)
      }
    } else {
      // buf_sz is at least one page in size so this should be safe
      return c->base->buffer_pool->buf_sz - 10;
    }
  }
  return 0;
}

size_t ws_conn_estimate_readable_len(ws_conn_t *c) {
  mirrored_buf_t *rb = c->recv_buf;
  if (rb) {
    return buf_len(rb);
  } else {
    return 0;
  }
}

bool ws_conn_can_put_msg(ws_conn_t *c, size_t msg_len) {
  msg_len += frame_get_header_len(msg_len);
  if (c->send_buf) {
    return buf_space(c->send_buf) >= msg_len;
  } else {
    return c->base->buffer_pool->buf_sz >= msg_len;
  }
}

inline size_t ws_conn_pending_bytes(ws_conn_t *c) {
  if (c->send_buf) {
    return buf_len(c->send_buf);
  } else {
    return 0;
  }
}

bool ws_conn_msg_ready(ws_conn_t *c) {
  mirrored_buf_t *rb = c->recv_buf;
  if (is_upgraded(c) & !is_closed(c) & (rb != NULL)) {
    size_t val = 0;
    // note* : this doesn't account for total_trimmed and may be wrong
    unsigned int ret = frame_decode_payload_len(
        rb->buf + rb->rpos + c->fragments_len, buf_len(rb), &val);
    (void)val;
    return ret == 0;
  } else {
    return 0;
  }
}

inline bool ws_conn_sending_fragments(ws_conn_t *c) {
  return is_sending_fragments(c);
}

int ws_conn_send_fragment(ws_conn_t *c, void *msg, size_t len, bool txt,
                          bool final) {
  bool is_continuation = is_sending_fragments(c);
  set_sending_fragments(c);
  uint8_t frame_cfg = OP_CONT;

  // first fragment
  if (!is_continuation) {
    frame_cfg = txt ? OP_TXT : OP_BIN;
  }

  if (final)
    frame_cfg |= FIN;

  int stat = conn_write_frame(c, msg, len, frame_cfg);
  int ret = ws_conn_do_send(c, stat);

  if ((final == 1) & ((ret == WS_SEND_OK) | (ret == WS_SEND_OK_BACKPRESSURE))) {
    // if we placed the final frame successfully clear the sending_fragments
    // state to allow non fragmented bin|txt frames to be sent again
    clear_sending_fragments(c);
  }

  return ret;
}

void ws_conn_close(ws_conn_t *conn, void *msg, size_t len, uint16_t code) {
  // reason string must be less than 124
  // this isn't a websocket protocol restriction but it will be here for now
  if (len > 124) {
    return;
  }

  // make sure we haven't already done this
  if (is_closed(conn)) {
    return;
  }

  conn_prep_send_buf(conn);

  uint8_t *buf = buf_peek(conn->send_buf);

  buf[0] = FIN | OP_CLOSE;
  buf[1] = 2 + (uint8_t)len;
  buf[2] = (uint8_t)(code >> 8) & 0xFF;
  buf[3] = (uint8_t)code & 0xFF;
  conn->send_buf->wpos += 4;
  buf_put(conn->send_buf, msg, len);

  conn_drain_write_buf(conn);
  ws_conn_destroy(conn, code);
}

/****************** Server **********************/

static struct timer_queue *timer_queue_init(struct timer_queue *tq);

static void timer_queue_run_expired_callbacks(struct timer_queue *tq,
                                              ws_server_t *s);

static void timer_queue_destroy(struct timer_queue *tq);

static void ws_server_time_update(ws_server_t *s) {
  long t = time(NULL);
  if (likely((t > 0) & (t < 4294967296))) {
    s->server_time = (unsigned int)t;
  } else {
    perror("time");
    exit(EXIT_FAILURE);
  }
}

static inline unsigned ws_server_time(ws_server_t *s) { return s->server_time; }

static void ws_server_register_timer_queue(ws_server_t *s, int *tfd) {
  assert(s->tq->timer_fd > 0);

  struct epoll_event ev = {
      .events = EPOLLIN,
      .data = {.ptr = tfd},
  };

  ws_server_epoll_ctl(s, EPOLL_CTL_ADD, s->tq->timer_fd, &ev);
}

static void conn_list_append(struct conn_list *cl, ws_conn_t *conn) {
  if (cl->len + 1 <= cl->cap) {
    cl->conns[cl->len++] = conn;
  } else {
    // this would be a serious bug, we should always have enough space unless we
    // are adding duplicates and theres a nasty bug
    fprintf(stderr, "[PANIC] connection list reached capacity, this should "
                    "never happen, app state is corrupted, shutting down\n");
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
      ws_server_t *s = c->base;

      while (i--) {
        if (s->pending_timers.conns[i] == c) {
          clear_has_pending_timers(c);
          ws_conn_t *tmp = s->pending_timers.conns[--s->pending_timers.len];
          s->pending_timers.conns[i] = tmp;
          break;
        }
      }
      break;
    }
  }
}

static size_t buf_pool_max_depth_running_avg(struct mirrored_buf_pool *p) {
  size_t total = 0;
  size_t count =
      p->ticks >= BUF_POOL_LONG_AVG_TICKS ? BUF_POOL_LONG_AVG_TICKS : p->ticks;

  if (count) {
    size_t i = count;
    while (i--) {
      total += p->avg_depths[i];
    }

    return total / count;
  }

  return 0;
}

static void server_check_pending_timers(ws_server_t *s) {
  static_assert(WS_ERR_READ_TIMEOUT == 994,
                "WS_ERR_READ_TIMEOUT should be 994");

  unsigned int timeout_kind = 993;
  ws_on_timeout_t cb = s->on_ws_conn_timeout;
  unsigned int now = ws_server_time(s);

  while (s->pending_timers.len) {
    size_t i = s->pending_timers.len;
    while (i--) {
      ws_conn_t *c = s->pending_timers.conns[i];

      timeout_kind +=
          (unsigned int)(c->read_timeout != 0 && c->read_timeout < now);
      timeout_kind +=
          (unsigned int)((c->write_timeout != 0 && c->write_timeout < now) * 2);

      if (timeout_kind != 993) {
        c->read_timeout = 0;
        c->write_timeout = 0;

        if (cb) {
          cb(c, timeout_kind);
        } else {
          ws_conn_destroy(c, timeout_kind);
        }

        timeout_kind = 993;
      } else {
        // ping the client if they are near read timeout
        if (c->read_timeout != 0 &&
            c->read_timeout < now + (TIMER_CHECK_TICKS + TIMER_CHECK_TICKS)) {
          if (is_writeable(c) && is_upgraded(c)) {
            ws_conn_send_msg(c, NULL, 0, OP_PING, 0);
          }
        }
      }
    }

    break;
  }
}

static void server_do_mirrored_buf_pool_gc(ws_server_t *s) {
  struct mirrored_buf_pool *p = s->buffer_pool;

  // place the max depth reached for the tick in the avg_depths ring
  p->avg_depths[s->buffer_pool->ticks++ % BUF_POOL_LONG_AVG_TICKS] =
      p->depth_reached;

  // save the current max depth
  size_t current_depth = p->depth_reached;

  // reset the per tick metric
  p->depth_reached = 0;

  size_t tick_no = p->ticks;

  // add up to the eventual avg
  // averaging out happens when it's needed
  p->avg_depth_reached_since_gc += current_depth;

  // update max_depth per gc
  // if the tick we are processing holds the current max
  // update the pool's max_depth_since_gc
  p->max_depth_since_gc = current_depth > p->max_depth_since_gc
                              ? current_depth
                              : p->max_depth_since_gc;

  // every BUF_POOL_GC_TICKS ticks check what can be collected
  if (!(tick_no % BUF_POOL_GC_TICKS)) {
    // calculate the avg from the past 32 tick
    // short term per BUF_POOL_GC_TICKS avg max depth seen
    size_t st_avg_depth = p->avg_depth_reached_since_gc / BUF_POOL_GC_TICKS;
    p->avg_depth_reached_since_gc = 0; // reset per gc metric
    // longer term avg for the above
    size_t lt_avg_depth = buf_pool_max_depth_running_avg(p);

    // get the current max depth and reset it
    size_t max_depth = p->max_depth_since_gc;
    p->max_depth_since_gc = 0; // reset per gc metric
    if (max_depth > p->touched_bufs) {
      p->touched_bufs = max_depth;
    }

    size_t avb_bufs = p->avb;

    // printf("%zu buffers in use\n", p->touched_bufs);

    // there's a downward trend in usage
    if (st_avg_depth <= lt_avg_depth) {
      size_t unneeded = 0;

      // are there spikes still?
      if (max_depth > st_avg_depth || max_depth > lt_avg_depth) {
        // is the current spike still lower than what we touched since last GC
        // if so we don't need to wait for the spike to drop and can free some
        // memory
        if (p->touched_bufs > max_depth) {
          unneeded = p->touched_bufs - max_depth;
          if (unneeded > 4) {
            unneeded /= 2; // remove half at once
          }
        } else {
          return;
        }
      } else {
        // no spikes check how many buffers were paged in
        // make sure to keep some for the average cases
        unneeded = p->touched_bufs - lt_avg_depth - st_avg_depth;
      }

      if (unneeded > 2 && avb_bufs > unneeded) {
        size_t madvise_count = unneeded - 2;
        if (madvise_count > 4096) {
          // limit calls to madvise to 4096 per GC cycle
          // the aim is to increase frequency of GC runs but limit time spent
          // per GC cycle at the cost of slower memory reclamation
          madvise_count = 4096;
        }

        size_t madvise_from = p->cap - p->touched_bufs;
        size_t madvise_to = madvise_from + madvise_count;

        assert(madvise_to <= p->cap - 2);

        for (size_t i = madvise_from; i < madvise_to; ++i) {
          // printf("MADV_DONTNEED %p\n", p->avb_stack[i]->buf);
          madvise(p->avb_stack[i]->buf, p->buf_sz * 2, MADV_DONTNEED);
        }

        p->touched_bufs -= madvise_count;
      }
    }
  }
}

static void server_writeable_conns_drain(ws_server_t *s) {
  size_t n = s->writeable_conns.len;

  for (size_t i = 0; i < n; ++i) {
    ws_conn_t *c = s->writeable_conns.conns[i];
    if (!is_closed(c) & (c->send_buf != NULL) & is_writeable(c)) {
      conn_drain_write_buf(c);
    }
    clear_write_queued(c);
  }

  // if draining caused more connections to get added to writeable_conns
  // copy them to the front and update writeable_conns len
  if (s->writeable_conns.len > n) {
    memcpy(s->writeable_conns.conns,
           s->writeable_conns.conns + s->writeable_conns.len,
           sizeof s->writeable_conns.conns * (s->writeable_conns.len - n));
    s->writeable_conns.len = s->writeable_conns.len - n;
  } else {
    s->writeable_conns.len = 0;
  }
}

static int ws_server_socket_bind(ws_server_t *s,
                                 struct ws_server_params *params) {
  struct sockaddr_in _;

  const char *addr = params->addr;
  uint16_t port = params->port;

  bool ipv6 = 0;
  if (inet_pton(AF_INET, addr, &_) != 1) {
    struct sockaddr_in6 _;
    if (inet_pton(AF_INET6, addr, &_) != 1) {
      return -1;
    } else {
      ipv6 = 1;
    }
  }

  int ret;

  s->fd = socket(ipv6 ? AF_INET6 : AF_INET,
                 SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  if (s->fd < 0) {
    return -1;
  }

  // socket config
  int on = 1;
  ret = setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &on,
                   sizeof(int));
  if (ret < 0) {
    return -1;
  }

  if (ipv6) {
    int off = 0;
    ret = setsockopt(s->fd, SOL_IPV6, IPV6_V6ONLY, &off, sizeof(int));
    if (ret < 0) {
      return -1;
    }

    struct sockaddr_in6 srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin6_family = AF_INET6;
    srv_addr.sin6_port = htons(port);
    inet_pton(AF_INET6, addr, &srv_addr.sin6_addr);

    ret = bind(s->fd, (const struct sockaddr *)&srv_addr, sizeof(srv_addr));
    if (ret < 0) {
      return -1;
    }
  } else {
    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(port);
    inet_pton(AF_INET, addr, &srv_addr.sin_addr);

    ret = bind(s->fd, (const struct sockaddr *)&srv_addr, sizeof(srv_addr));
    if (ret < 0) {
      return -1;
    }
  }

  return 0;
}

static void ws_server_register_callbacks(ws_server_t *s,
                                         struct ws_server_params *params) {
  // mandatory callbacks
  s->on_ws_open = params->on_ws_open;
  s->on_ws_msg = params->on_ws_msg;
  s->on_ws_disconnect = params->on_ws_disconnect;

  if (params->on_ws_err) {
    s->on_ws_err = params->on_ws_err;
  }

  if (params->on_ws_drain) {
    s->on_ws_drain = params->on_ws_drain;
  }

  if (params->on_ws_accept) {
    s->on_ws_accept = params->on_ws_accept;
  }

  if (params->on_ws_accept_err) {
    s->on_ws_accept_err = params->on_ws_accept_err;
  }

  if (params->on_ws_handshake) {
    s->on_ws_handshake = params->on_ws_handshake;
  }

  if (params->on_ws_conn_timeout) {
    s->on_ws_conn_timeout = params->on_ws_conn_timeout;
  }
}

static void ws_server_async_runner_create(ws_server_t *s, size_t init_cap);

static void ws_server_register_buffers(ws_server_t *s,
                                       struct ws_server_params *params) {
  size_t max_backpressure =
      params->max_buffered_bytes ? params->max_buffered_bytes : 16000;
  size_t page_size = get_pagesize();

  // account for an interleaved control msg during fragmentation
  // since we never dynamically allocate more buffers
  size_t buffer_size =
      (max_backpressure + 192 + page_size - 1) & ~(page_size - 1);

  struct rlimit rlim = {0};
  getrlimit(RLIMIT_NOFILE, &rlim);

  unsigned max_map_count;

  FILE *f = fopen("/proc/sys/vm/max_map_count", "r");
  if (f == NULL) {
    perror("Error Opening /proc/sys/vm/max_map_count");
  } else {
    if (fscanf(f, "%u", &max_map_count) != 1) {
      perror("Error Reading /proc/sys/vm/max_map_count");
    }

    fclose(f);
  }

  size_t lim = rlim.rlim_cur > 16 ? rlim.rlim_cur - 16 : rlim.rlim_cur;
  max_map_count = max_map_count ? max_map_count : 65530;

  if (!params->max_conns) {
    // account for already open files
    s->max_conns = (unsigned)(lim < 4294967296 ? lim : 4294967295);

  } else if (params->max_conns <= rlim.rlim_cur) {
    s->max_conns = params->max_conns;
    if (!params->silent && rlim.rlim_cur > 8) {
      if (s->max_conns > rlim.rlim_cur - 8) {
        fprintf(
            stderr,
            "[WARN] params->max_conns %u may be too high. RLIMIT_NOFILE=%zu "
            "only %zu can be opened for other tasks when running at "
            "max_conns\n",
            s->max_conns, rlim.rlim_cur, rlim.rlim_cur - s->max_conns);
      }
    }

  } else if (params->max_conns > rlim.rlim_cur) {
    s->max_conns = (unsigned)(lim < 4294967296 ? lim : 4294967295);
    if (!params->silent) {
      fprintf(stderr, "[WARN] params->max_conns %u exceeds RLIMIT_NOFILE %zu\n",
              params->max_conns, rlim.rlim_cur);
    }
  }

  if (s->max_conns * 4 > max_map_count) {
    fprintf(stderr,
            "[WARN] max_map_count %u is too low and may cause non recoverable "
            "mmap "
            "failures. "
            "Consider increasing it to %zu or higher # sudo sysctl -w "
            "vm.max_map_count=%zu\n",
            max_map_count, align_to((s->max_conns * 4) + 64, 2),
            align_to((s->max_conns * 4) + 64, 2));

    // leave off 64 mappings
    unsigned new_max_conns = (max_map_count / 4) > 64 ? (max_map_count / 4) - 64
                                                      : (max_map_count / 4);
    s->max_conns = new_max_conns;
  }

  s->buffer_pool =
      mirrored_buf_pool_create(s->max_conns + s->max_conns, buffer_size, 1);

  s->conn_pool = ws_conn_pool_create(s->max_conns);

  s->max_msg_len = max_backpressure;

  // allocate the list (dynamic array of pointers to ws_conn_t) to track
  // writeable connections
  s->writeable_conns.conns =
      calloc(s->max_conns, sizeof s->writeable_conns.conns);
  s->writeable_conns.len = 0;
  s->writeable_conns.cap = s->max_conns;

  s->pending_timers.conns =
      calloc(s->max_conns, sizeof s->pending_timers.conns);
  s->pending_timers.len = 0;
  s->pending_timers.cap = s->max_conns;

  if (!params->max_header_count) {
    params->max_header_count = 32;
  } else if (params->max_header_count > 512) {
    params->max_header_count = 512;
  }

  // allocate shared handshake structure
  s->hs =
      calloc(1, sizeof(struct ws_conn_handshake) +
                    (sizeof(struct http_header) * params->max_header_count));
  if (s->hs == NULL) {
    perror("calloc");
    exit(EXIT_FAILURE);
  }

  s->max_handshake_headers = params->max_header_count;

  // make sure we got the mem needed
  if (s->writeable_conns.conns == NULL || s->pending_timers.conns == NULL) {
    perror("calloc");
    exit(EXIT_FAILURE);
  };

  s->tq = calloc(1, sizeof *s->tq);
  if (s->tq == NULL) {
    perror("calloc");
    exit(EXIT_FAILURE);
  }

  s->tq = timer_queue_init(s->tq);
}

#ifdef WITH_COMPRESSION
static z_stream *inflation_stream_init();
static z_stream *deflation_stream_init();
#endif /* WITH_COMPRESSION */

ws_server_t *ws_server_create(struct ws_server_params *params) {

  if (params->port <= 0) {
    errno = EINVAL;
    return NULL;
  }

  if (!params->on_ws_open || !params->on_ws_disconnect || !params->on_ws_msg) {
    errno = EINVAL;
    return NULL;
  }

  ws_server_t *s;

  // allocate memory for server and events for epoll
  s = (ws_server_t *)calloc(1, (sizeof *s));
  if (s == NULL) {
    return NULL;
  }

  if (params->ctx) {
    s->ctx = params->ctx;
  }

  // init epoll
  s->epoll_fd = epoll_create1(0);
  if (s->epoll_fd < 0) {
    free(s);
    return NULL;
  }

  int res = ws_server_socket_bind(s, params);
  if (res != 0) {
    if (s->fd != -1)
      close(s->fd);

    close(s->epoll_fd);
    free(s);
    return NULL;
  }

  ws_server_register_callbacks(s, params);

  // register timer fd
  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  s->tfd = tfd;
  if (s->tfd == -1) {
    close(s->fd);
    close(s->epoll_fd);
    free(s);
    return NULL;
  }

  // this crashes on failure (fix?)
  ws_server_register_buffers(s, params);

#ifdef WITH_COMPRESSION
  const char *compression_enabled = "true";
  s->istrm = inflation_stream_init();
  s->dstrm = deflation_stream_init();
#else
  const char *compression_enabled = "false";
#endif /* WITH_COMPRESSION */

#ifdef NDEBUG
  const char *debug_enabled = "false";
#else
  const char *debug_enabled = "true";
#endif

  ws_server_async_runner_create(s, 2);

  if (!params->silent) {
    printf("- listening:   %s:%d\n", params->addr, params->port);
    printf("- buffer_size: %zu\n", s->buffer_pool->buf_sz);
    printf("- max_msg_len: %zu\n",
           params->max_buffered_bytes ? params->max_buffered_bytes : 16000);
    printf("- max_conns:   %u\n", s->max_conns);
    printf("- compression: %s\n", compression_enabled);
    printf("- debug:       %s\n", debug_enabled);
    printf("- max_headers: %zu\n", params->max_header_count);
  }

  s->max_per_read = s->buffer_pool->buf_sz;

  // server resources all ready
  return s;
}

static void ws_server_epoll_ctl(ws_server_t *s, int op, int fd,
                                struct epoll_event *ev) {
  if (epoll_ctl(s->epoll_fd, op, fd, ev) == -1) {
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

static void ws_server_new_conn(ws_server_t *s, int client_fd,
                               unsigned int now) {
  ws_conn_t *conn = ws_conn_get(s->conn_pool);
  if (unlikely(conn == NULL)) {
    // this would NEVER happen
    // because when the server is created we set up the limits
    // to ensure that we can accommodate all incoming connections upto specified
    // max this indicates a bug somewhere and it's best to exit because this is
    // not within expected program behavior
    fprintf(stderr, "[PANIC] connection pool empty, this should never happen, "
                    "app state is corrupted, shutting down\n");
    exit(EXIT_FAILURE);
    return;
  }

  conn->fd = client_fd;
  conn->flags = 0;
  conn->write_timeout = 0;
  conn->read_timeout = now + READ_TIMEOUT;
  conn->needed_bytes = 12;
  conn->fragments_len = 0;
  conn->base = s;
  set_writeable(conn);
  conn->ctx = NULL;

  assert(conn->send_buf == NULL);
  assert(conn->recv_buf == NULL);

#ifdef WITH_COMPRESSION
  conn->pmd_buf = NULL;
#endif /* WITH_COMPRESSION*/

  struct epoll_event ev = {
      .events = EPOLLIN | EPOLLRDHUP,
      .data = {.ptr = conn},
  };

  ws_server_epoll_ctl(s, EPOLL_CTL_ADD, client_fd, &ev);
  ++s->open_conns;

  server_pending_timers_append(conn);
}

static bool ws_server_accept_err_recoverable(int err) {
  switch (err) {
  case ENONET:
  case EPROTO:
  case ENOPROTOOPT:
  case EOPNOTSUPP:
  case ENETDOWN:
  case ENETUNREACH:
  case EHOSTDOWN:
  case EHOSTUNREACH:
  case ECONNABORTED:
  case EMFILE:
  case ENFILE:
  case EAGAIN:
#if EAGAIN != EWOULDBLOCK
  case EWOULDBLOCK:
#endif
  case EINTR:
    return true;
  default:
    return false;
  }
}

static void ws_server_conns_establish(ws_server_t *s, struct sockaddr *sockaddr,
                                      socklen_t *socklen) {
  unsigned int now = ws_server_time(s);
  // how many conns should we try to accept in total
  size_t accepts = 1024; // we do a batch of 1024 accepts at a time

  int sockopt_on = 1;
  while (accepts--) {
    if (s->fd == -1) {
      return;
    }
    int fd = accept4(s->fd, sockaddr, socklen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return; // done
      } else if (ws_server_accept_err_recoverable(errno)) {
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
    } else {
      if (s->open_conns + 1 <= s->max_conns) {
        // accept callback can return -1 to reject the connection
        if (s->on_ws_accept &&
            s->on_ws_accept(s, (struct sockaddr_storage *)sockaddr, fd) == -1) {
          if (close(fd) == -1) {
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
        if (setsockopt(fd, SOL_TCP, TCP_NODELAY, &sockopt_on,
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

        ws_server_new_conn(s, fd, now);
      } else {
        if (s->on_ws_accept_err) {
          s->on_ws_accept_err(s, 0);
        }
        // we are at max connections
        close(fd);
      }
    }
  }
}

static void ws_server_register_timer(ws_server_t *s, int *tfd) {
  struct itimerspec timer = {.it_interval =
                                 {
                                     .tv_nsec = 0,
                                     .tv_sec = SECONDS_PER_TICK,
                                 },
                             .it_value = {
                                 .tv_nsec = 0,
                                 .tv_sec = SECONDS_PER_TICK,
                             }};

  timerfd_settime(*tfd, 0, &timer, NULL);

  (void)timer;

  struct epoll_event ev = {
      .events = EPOLLIN,
      .data = {.ptr = tfd},
  };

  ws_server_epoll_ctl(s, EPOLL_CTL_ADD, *tfd, &ev);
  s->internal_polls++;
}

static int ws_server_listen_and_serve(ws_server_t *s, int backlog) {
  int ret = listen(s->fd, backlog);
  if (ret < 0) {
    return ret;
  }

  struct epoll_event ev = {
      .events = EPOLLIN,
      .data = {.ptr = s},
  };

  ws_server_epoll_ctl(s, EPOLL_CTL_ADD, s->fd, &ev);
  s->internal_polls++;

  return 0;
}

static int ws_server_do_epoll_wait(ws_server_t *s, int epfd) {

  if (likely(s->internal_polls > 0)) {
    int n_evs;
    for (;;) {
      n_evs = epoll_wait(epfd, s->events, 1024, -1);
      if (likely(n_evs >= 0)) {
        break;
      } else {
        if (errno == EINTR) {
          continue;
        } else {
          if (s->on_ws_err) {
            int err = errno;
            s->on_ws_err(s, err);
          } else {
            perror("epoll_wait");
            exit(EXIT_FAILURE);
          }
          return -1;
        }
      }
    }
    return n_evs;
  } else {
    return 0;
  }
}

static void ws_server_async_runner_run_pending_callbacks(
    ws_server_t *s, struct ws_server_async_runner *arptr);

static inline void timer_consume(int tfd) {
  uint64_t _;
  read(tfd, &_, 8);
  (void)_;
};

int ws_server_start(ws_server_t *s, int backlog) {
  int ret = ws_server_listen_and_serve(s, backlog);
  if (ret < 0) {
    return ret;
  }

  size_t timer_check_counter = TIMER_CHECK_TICKS;
  struct ws_server_async_runner *arptr = s->async_runner;

  int epfd = s->epoll_fd;
  int tfd = s->tfd;

  int tqfd = s->tq->timer_fd;

  ws_server_register_timer_queue(s, &tqfd); // High-Res Timer
  ws_server_register_timer(s, &tfd);        // io Timer
  ws_server_time_update(s);

  struct sockaddr_storage client_sockaddr;
  socklen_t client_socklen;
  client_socklen = sizeof client_sockaddr;

  struct epoll_event ev = {
      0,
  };

  for (;;) {
    s->active_events = 0;
    int n_evs = ws_server_do_epoll_wait(s, epfd);
    if (unlikely((n_evs == 0) | (n_evs == -1))) {
      return n_evs;
    }

    s->active_events = n_evs;

    bool check_user_epoll = false;
    int *user_epoll_ptr = &s->user_epoll;
    // loop over events
    for (int i = 0; i < n_evs; ++i) {
      if ((s->events[i].data.ptr == &tfd) |
          (s->events[i].data.ptr == user_epoll_ptr) |
          (s->events[i].data.ptr == arptr) | (s->events[i].data.ptr == &tqfd)) {

        if (s->events[i].data.ptr == &tqfd) {
          timer_consume(tqfd);
          timer_queue_run_expired_callbacks(s->tq, s);

        } else if (s->events[i].data.ptr == &tfd) {
          timer_consume(tfd);

          // we update the time every time tfd is readable (every second)
          ws_server_time_update(s);

          timer_check_counter--;
          if (!timer_check_counter) {
            server_check_pending_timers(s);
            timer_check_counter = TIMER_CHECK_TICKS;
          }

          server_do_mirrored_buf_pool_gc(s);

        } else if (s->events[i].data.ptr == arptr) {
          ws_server_async_runner_run_pending_callbacks(s, arptr);
        } else {
          check_user_epoll = true;
        }

      } else if (s->events[i].data.ptr == s) {
        ws_server_conns_establish(s, (struct sockaddr *)&client_sockaddr,
                                  &client_socklen);
      } else {
        ws_conn_t *c = s->events[i].data.ptr;
        if (s->events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
          c->fragments_len = 0; // EOF
          ws_conn_destroy(s->events[i].data.ptr, WS_ERR_READ);
        } else {
          if (s->events[i].events & EPOLLOUT) {
            if (!is_closed(c)) {
              int ret = conn_drain_write_buf(c);
              if (ret == 1) {
                if (!is_write_shutdown(c)) {
                  if (s->on_ws_drain) {
                    s->on_ws_drain(c);
                  }

                  if (!is_upgraded(c)) {
                    set_upgraded(c);
                    c->needed_bytes = 2;
                    c->read_timeout = ws_server_time(s) + READ_TIMEOUT;
                    s->on_ws_open(c);
                  }
                } else {
                  // we wrote everything we needed to write
                  // we can shutdown our write end
                  if (conn_shutdown_wr(c) == -1) {
                    continue; // skip arming EPOLLIN if this fails
                  };
                }

                // we must recheck if the connection is still writeable
                // it may be that more back pressure was built when
                // s->on_ws_drain was called if that's the case we want to keep
                // waiting on EPOLLOUT before resuming reads
                if (is_writeable(c)) {
                  ev.events = EPOLLIN | EPOLLRDHUP;
                  ev.data.ptr = c;
                  ws_server_epoll_ctl(s, EPOLL_CTL_MOD, c->fd, &ev);

                  clear_read_paused(c);
                }
              }
            }
          }
          if (s->events[i].events & EPOLLIN) {
            if (!is_closed(c)) {
              if (!c->recv_buf) {
                c->recv_buf = mirrored_buf_get(s->buffer_pool);
              }

              if (is_upgraded(c)) {
                set_processing(c);
                ws_conn_proccess_frames(c);
                clear_processing(c);
              } else {
                ws_conn_do_handshake(c);
              }
            }
          }
        }
      }
    }

    if (check_user_epoll) {
      int count;
      for (;;) {
        count = epoll_wait(*user_epoll_ptr, s->events, 1024, 0);
        if (unlikely(count == 0 || count == -1)) {
          if (count == -1 && errno == EINTR) {
            continue;
          } else {
            break;
          }
        } else {
          break;
        }
      }

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

    // drain all outgoing before calling epoll_wait
    server_writeable_conns_drain(s);
  }

  return 0;
}

static struct ws_server_async_runner_buf *
ws_server_async_runner_buf_create(size_t init_cap) {
  struct ws_server_async_runner_buf *arb;
  arb = calloc(1, sizeof *arb);
  if (arb == NULL) {
    perror("calloc");
    exit(EXIT_FAILURE);
  }

  arb->cbs = calloc(init_cap, sizeof arb->cbs);
  if (arb->cbs == NULL) {
    perror("calloc");
    exit(EXIT_FAILURE);
  }

  arb->cap = init_cap;

  return arb;
}

static void ws_server_async_runner_create(ws_server_t *s, size_t init_cap) {
  if (!init_cap) {
    init_cap = 1;
  }
  struct ws_server_async_runner *ar =
      calloc(1, sizeof(struct ws_server_async_runner));

  if (ar == NULL) {
    perror("calloc");
    exit(EXIT_FAILURE);
  }

  ar->pending = ws_server_async_runner_buf_create(init_cap);
  ar->ready = ws_server_async_runner_buf_create(init_cap);

  ar->chanfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (ar->chanfd == -1) {
    perror("eventfd");
    exit(EXIT_FAILURE);
  }

  int ret = pthread_mutex_init(&ar->mu, NULL);
  if (ret == -1) {
    perror("pthread_mutex_init");
    exit(EXIT_FAILURE);
  }
  struct epoll_event ev = {
      .events = EPOLLIN,
      .data = {.ptr = ar},
  };

  ws_server_epoll_ctl(s, EPOLL_CTL_ADD, ar->chanfd, &ev);
  s->internal_polls++;
  s->async_runner = ar;
}

static void ws_server_async_runner_destroy(ws_server_t *s) {
  free(s->async_runner->pending->cbs);
  free(s->async_runner->ready->cbs);
  free(s->async_runner->pending);
  free(s->async_runner->ready);
  pthread_mutex_destroy(&s->async_runner->mu);
  free(s->async_runner);
  s->async_runner = NULL;
}

int ws_server_sched_callback(ws_server_t *s, struct async_cb_ctx *cb_info) {
  if (cb_info != NULL) {
    struct ws_server_async_runner *ar = s->async_runner;

    pthread_mutex_lock(&ar->mu);
    // mu start

    // if we are shutting down chanfd will be -1
    if (unlikely(ar->chanfd == -1)) {
      // mu early end
      pthread_mutex_unlock(&ar->mu);
      return -1;
    }

    struct ws_server_async_runner_buf *q = ar->pending;

    size_t new_len = q->len + 1;

    // make sure we have/get the space needed
    if (new_len > q->cap) {
      struct async_cb_ctx **new_cbs =
          realloc(q->cbs, sizeof q->cbs * (q->cap + q->cap));
      if (new_cbs != NULL) {
        q->cbs = new_cbs;
        q->cap = q->cap + q->cap;
      } else {
        // mu early end
        pthread_mutex_unlock(&ar->mu);
        return -1;
      }
    }

    q->cbs[q->len++] = cb_info;

    // mu end
    pthread_mutex_unlock(&ar->mu);

    // notify
    register int fd = ar->chanfd;
    for (;;) {
      if (likely(eventfd_write(fd, 1) == 0)) {
        break;
      } else {
        if (errno == EINTR) {
          continue;
        } else {
          // if EAGAIN it will be processed eventually
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          } else {
            return -1;
          };
        }
      }
    }

    return 0;
  }

  return -1;
}

static void ws_server_async_runner_run_pending_callbacks(
    ws_server_t *s, struct ws_server_async_runner *ar) {
  uint64_t val;
  read(ar->chanfd, &val, 8);

  (void)val;

  // grab the lock to swap the buffers
  // and get the count of ready callbacks
  pthread_mutex_lock(&ar->mu);
  // mu start

  struct ws_server_async_runner_buf *ready = ar->pending;
  ar->pending = ar->ready;
  ar->ready = ready;
  size_t len = ready->len;

  // mu end
  pthread_mutex_unlock(&ar->mu);

  // run all ready callbacks (no lock)
  struct async_cb_ctx **cbs = ready->cbs;
  for (size_t i = 0; i < len; ++i) {
    // run all callbacks
    cbs[i]->cb(s, cbs[i]);
  }

  ready->len = 0;
}

size_t ws_server_pending_async_callbacks(ws_server_t *s) {
  struct ws_server_async_runner *ar = s->async_runner;
  size_t count = 0;

  pthread_mutex_lock(&ar->mu);
  // mu start
  count = ar->pending->len;
  // mu end
  pthread_mutex_unlock(&ar->mu);

  return count;
}

int ws_server_shutdown(ws_server_t *s) {
  if (s->internal_polls <= 0) {
    return -1;
  }

  // remove and close listner fd
  struct epoll_event ev = {
      .events = 0,
      .data = {.ptr = NULL},
  };

  eventfd_write(s->async_runner->chanfd, 1);

  close(s->fd);
  epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, s->fd, &ev);
  s->internal_polls--;
  s->fd = -1;

  // go over all connections and shut them down
  for (size_t i = 0; i < s->conn_pool->cap; ++i) {
    ws_conn_t *c = &s->conn_pool->base[i];
    if (c->fd == 0 || is_closed(c)) {
      continue;
    }

    if (is_upgraded(c)) {
      ws_conn_close(c, NULL, 0, WS_CLOSE_GOAWAY);
    } else {
      ws_conn_destroy(c, WS_CLOSE_GOAWAY);
    }
  }

  // close user epoll
  if (s->user_epoll > 0) {
    epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, s->user_epoll, &ev);
    close(s->user_epoll);
    s->user_epoll = -1;
    s->internal_polls--;
  }

  // close timer fd
  if (s->tfd) {
    epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, s->tfd, &ev);
    close(s->tfd);
    s->tfd = -1;
    s->internal_polls--;
  }

  // close event fd
  if (s->async_runner->chanfd && s->internal_polls) {
    s->internal_polls--;
  }

  return 0;
}

inline bool ws_server_shutting_down(ws_server_t *s) {
  return s->internal_polls <= 0;
}

inline void ws_server_set_max_per_read(ws_server_t *s, size_t max_per_read) {
  if (max_per_read && s->buffer_pool->buf_sz >= max_per_read) {
    s->max_per_read = max_per_read;
  } else {
    s->max_per_read = s->buffer_pool->buf_sz;
  }
}

inline int ws_server_active_events(ws_server_t *s) { return s->active_events; }

int ws_epoll_create1(ws_server_t *s) {
  if (!s->user_epoll) {
    s->user_epoll = epoll_create1(O_CLOEXEC);
    if (s->user_epoll == -1) {
      return -1;
    } else {
      struct epoll_event ev = {
          .events = EPOLLIN,
          .data = {.ptr = &s->user_epoll},
      };

      ws_server_epoll_ctl(s, EPOLL_CTL_ADD, s->user_epoll, &ev);
      s->internal_polls++;
      return 0;
    }
  }

  return s->user_epoll ? s->user_epoll : -1;
}

int ws_epoll_ctl_add(ws_server_t *s, int fd, ws_poll_cb_ctx_t *cb_ctx,
                     unsigned int events) {
  if (!s->user_epoll || !cb_ctx) {
    return -1;
  }

  struct epoll_event ev = {
      .events = events,
      .data = {.ptr = cb_ctx},
  };

  return epoll_ctl(s->user_epoll, EPOLL_CTL_ADD, fd, &ev);
}

int ws_epoll_ctl_del(ws_server_t *s, int fd) {
  if (!s->user_epoll) {
    return -1;
  }

  struct epoll_event ev = {
      .events = 0,
      .data = {.ptr = NULL},
  };
  return epoll_ctl(s->user_epoll, EPOLL_CTL_DEL, fd, &ev);
}

int ws_epoll_ctl_mod(ws_server_t *s, int fd, ws_poll_cb_ctx_t *cb_ctx,
                     unsigned int events) {
  if (!s->user_epoll || !cb_ctx) {
    return -1;
  }

  struct epoll_event ev = {
      .events = events,
      .data = {.ptr = cb_ctx},
  };

  return epoll_ctl(s->user_epoll, EPOLL_CTL_MOD, fd, &ev);
}

int ws_server_destroy(ws_server_t *s) {
  // this one was used to wake up from epoll_wait when we shut down
  // this is why we didn't close it in ws_server_shutdown

  struct epoll_event ev = {0};

  epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, s->async_runner->chanfd, &ev);
  close(s->async_runner->chanfd);
  s->async_runner->chanfd = -1;

  if (s->user_epoll > 0) {
    epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, s->user_epoll, &ev);
    close(s->user_epoll);
    s->user_epoll = -1;
  }

  if (s->fd > 0) {
    epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, s->fd, &ev);
    close(s->fd);
    s->fd = -1;
  }

  if (s->tfd > 0) {
    epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, s->tfd, &ev);
    close(s->tfd);
    s->tfd = -1;
  }

  server_ws_conn_pool_destroy(s);
  server_mirrored_buf_pool_destroy(s);
  ws_server_async_runner_destroy(s);

  close(s->epoll_fd);
  s->epoll_fd = -1;

#ifdef WITH_COMPRESSION

  if (s->istrm) {
    inflateEnd(s->istrm);
    free(s->istrm);
  }

  if (s->dstrm) {
    deflateEnd(s->dstrm);
    free(s->dstrm);
  }

#endif /* WITH_COMPRESSION */

  free(s->hs);
  free(s->writeable_conns.conns);
  free(s->pending_timers.conns);
  free(s);

  return 0;
}

/************** High Resolution Timers **************/

static int timer_queue_cmp(timer_queue *a, timer_queue *b) {
  return (*a)->expiry_ns < (*b)->expiry_ns;
}

static inline uint64_t timespec_ns(struct timespec *tp) {
  return (tp->tv_sec * 1000000000) + tp->tv_nsec;
}


// updates the current time of the timer queue
// if timeout_after is not NULL it will return 
// the expiration time of the timeout in relation to the current time
// if timeout_after is NULL it will return 0
static uint64_t timer_queue_get_expiration(struct timer_queue *tq,
                                       struct timespec *timeout_after) {
  struct timespec tp;
  int ret = clock_gettime(CLOCK_BOOTTIME, &tp);
  assert(ret == 0);
  (void)ret;

  tq->cur_time = timespec_ns(&tp);


  if (timeout_after != NULL) {
    // add the timeout to the current time
    tp.tv_sec += timeout_after->tv_sec;
    tp.tv_nsec += timeout_after->tv_nsec;

    // calculate the total time in nano seconds
    uint64_t exp = timespec_ns(&tp);
    // printf("current time %zu timeout after %zu\n", tq->cur_time, exp);
    return exp;
  } else {
    return 0;
  }
}

static struct timer_queue *timer_queue_init(struct timer_queue *tq) {
  pqu_timer_queue timer_queue = pqu_timer_queue_init(timer_queue_cmp);
  pqu_timer_queue *p = calloc(1, sizeof(timer_queue));
  memcpy(p, &timer_queue, sizeof(timer_queue));
  tq->pqu_tq = p;

  tq->next_expiration = 0;

  tq->timer_fd = timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK | TFD_CLOEXEC);
  if (tq->timer_fd == -1) {
    pqu_timer_queue_free(tq->pqu_tq);
    free(p);
    tq->pqu_tq = NULL;
    perror("timerfd_create");
    exit(EXIT_FAILURE);
  }

  return tq;
}

static void timer_queue_destroy(struct timer_queue *tq) {

  while (!pqu_timer_queue_empty(tq->pqu_tq)) {
    ws_timer_t **p = pqu_timer_queue_top(tq->pqu_tq);
    ws_timer_t *t = *p;
    pqu_timer_queue_pop(tq->pqu_tq);
    free(t);
  }

  pqu_timer_queue_free(tq->pqu_tq);
  free(tq->pqu_tq);
  close(tq->timer_fd);
  memset(tq, 0, sizeof(*tq));
}

static uint64_t timer_queue_get_soonest_expiration(struct timer_queue *tq) {
  if (!pqu_timer_queue_empty(tq->pqu_tq)) {
    ws_timer_t **p = pqu_timer_queue_top(tq->pqu_tq);
    return (*p)->expiry_ns;
  } else {
    return 0;
  }
}

static inline uint64_t
timer_queue_should_update_expiration(struct timer_queue *tq,
                                     uint64_t maybe_soonest) {
  // if we don't have a next expiration or the new expiration is sooner than the
  if ((tq->next_expiration == 0 || tq->next_expiration < tq->cur_time) ||
      (tq->next_expiration > maybe_soonest)) {
    return maybe_soonest != 0;
  }

  return 0;
}

static void timer_queue_tfd_set_soonest_expiration(struct timer_queue *tq,
                                                   uint64_t maybe_soonest) {
  if (timer_queue_should_update_expiration(tq, maybe_soonest)) {
    uint64_t ns = maybe_soonest - tq->cur_time;
    struct itimerspec timeout = {
        .it_value =
            {
                .tv_nsec = ns % 1000000000,
                .tv_sec = ns / 1000000000,
            },
        .it_interval =
            {
                .tv_nsec = 0,
                .tv_sec = 0,
            },
    };

    tq->next_expiration = maybe_soonest;
    printf("[WS_INFO] next expiration in = %zuus\n", ns / 1000);
    int ret = timerfd_settime(tq->timer_fd, 0, &timeout, NULL);
    assert(ret == 0);
  } 
}

static void timer_queue_run_expired_callbacks(struct timer_queue *tq,
                                              ws_server_t *s) {
  timer_queue_get_expiration(tq, NULL); // used to update the time
  printf("---------------------- timer_queue_run_expired_callbacks\n");
  while (!pqu_timer_queue_empty(tq->pqu_tq)) {
    ws_timer_t **p = pqu_timer_queue_top(tq->pqu_tq);
    if ((*p)->expiry_ns < tq->cur_time) {
      tq->next_expiration = 0;
      ws_timer_t *t = *p;
      pqu_timer_queue_pop(tq->pqu_tq);
      if ((t->cancelled == 0) & (t->cb != NULL)) {
        t->cb(s, t->ctx);
      }
      free(t);
    } else {
      printf("breaking at next expire %zu current %zu\n", (*p)->expiry_ns, tq->cur_time);
      break;
    }
  }


  uint64_t soonest = timer_queue_get_soonest_expiration(tq);
  if (soonest != 0 && soonest != tq->next_expiration){
    printf("[WS_INFO] new soonest = %zuus\n", (soonest - tq->cur_time) / 1000);
    timer_queue_tfd_set_soonest_expiration(tq, soonest);
  }

  printf("---------------------- \n");
}


static uint64_t timer_queue_add(struct timer_queue *tq, ws_timer_t *t) {
  pqu_timer_queue_push(tq->pqu_tq, t);
  assert(tq->cur_time < t->expiry_ns);
  timer_queue_tfd_set_soonest_expiration(tq, t->expiry_ns);
  return t->expiry_ns;
}

int ws_server_set_timeout(ws_server_t *s, struct timespec *tp, void *ctx,
                          timeout_cb_t cb) {

  if (tp == NULL || (tp->tv_nsec < 0 || tp->tv_nsec > 999999999)) {
    return -1;
  }
  // TODO (sah): use a pool and recycle these instead of malloc
  ws_timer_t *timeout = malloc(sizeof(ws_timer_t));
  timeout->cancelled = 0;
  timeout->ctx = ctx;
  timeout->cb = cb;
  timeout->expiry_ns = timer_queue_get_expiration(s->tq, tp);
  timer_queue_add(s->tq, timeout);

  return 0;
}

/*************** Per Message Deflate *************/

#ifdef WITH_COMPRESSION

static z_stream *inflation_stream_init() {
  z_stream *istrm = calloc(1, sizeof(z_stream));
  if (istrm == NULL) {
    perror("calloc");
    exit(EXIT_FAILURE);
  }

  inflateInit2(istrm, -15);
  return istrm;
}

static ssize_t inflation_stream_inflate(z_stream *istrm, char *input,
                                        unsigned in_len, char *out,
                                        unsigned out_len,
                                        bool no_ctx_takeover) {
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
  unsigned total = 0;
  do {
    // printf("inflating...\n");
    istrm->next_out = (Bytef *)out + total;
    istrm->avail_out = out_len - total;
    err = inflate(istrm, Z_SYNC_FLUSH);
    if ((err == Z_OK) & (istrm->avail_out != 0)) {
      total += out_len - istrm->avail_out;
      break;
    } else {
      memcpy(tail_addr, pre_tail, 4);
      fprintf(stderr, "inflate(): %d %s\n", err, istrm->msg);
      inflateReset(istrm);
      return 0;
    }

  } while ((istrm->avail_out == 0) & (total <= out_len));

  if (no_ctx_takeover) {
    inflateReset(istrm);
  }

  // DON'T FORGET TO DO THIS
  memcpy(tail_addr, pre_tail, 4);

  if ((err < 0) || total > out_len) {
    fprintf(stderr, "Decompression error or payload too large %d %u %u\n", err,
            total, out_len);

    return err < 0 ? err : -1;
  }

  return total;
}

static z_stream *deflation_stream_init() {
  z_stream *dstrm = calloc(1, sizeof(z_stream));
  if (dstrm == NULL) {
    perror("calloc");
    exit(EXIT_FAILURE);
  }

  deflateInit2(dstrm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
               Z_DEFAULT_STRATEGY);
  return dstrm;
}

static ssize_t deflation_stream_deflate(z_stream *dstrm, char *input,
                                        unsigned in_len, char *out,
                                        unsigned out_len,
                                        bool no_ctx_takeover) {

  dstrm->next_in = (Bytef *)input;
  dstrm->avail_in = (unsigned int)in_len;

  int err;
  unsigned total = 0;

  do {
    // printf("deflating...\n");
    assert((ssize_t)out_len - total >= 6);
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

  if (err == Z_OK) {
    return total > (unsigned)4 ? total - (unsigned)4 : 0;
  } else {
    return err;
  }
}

static z_stream *inflation_stream_init();

static ssize_t inflation_stream_inflate(z_stream *istrm, char *input,
                                        unsigned in_len, char *out,
                                        unsigned out_len, bool no_ctx_takeover);

static z_stream *deflation_stream_init();

static struct dyn_buf *conn_dyn_buf_get(ws_conn_t *c) {
  if (!c->pmd_buf) {
    c->pmd_buf = malloc(sizeof(struct dyn_buf) +
                        (c->base->buffer_pool->buf_sz * 2) + 16);
    c->pmd_buf->len = 0;
    c->pmd_buf->cap = (c->base->buffer_pool->buf_sz * 2) + 16;
    if (c->pmd_buf == NULL) {
      perror("malloc");
      exit(EXIT_FAILURE);
    }

    return c->pmd_buf;
  } else {
    return c->pmd_buf;
  }
}

static void conn_dyn_buf_dispose(ws_conn_t *c) {
  if (c->pmd_buf) {
    free(c->pmd_buf);
    c->pmd_buf = NULL;
  }
}

static int ws_conn_handle_compressed_frame(ws_conn_t *conn, uint8_t *data,
                                           size_t payload_len) {
  ws_server_t *s = conn->base;

  struct dyn_buf *inflated_buf = conn_dyn_buf_get(conn);
  assert(inflated_buf->len == 0);

  bool was_fragmented = conn->fragments_len != 0;

  char *msg = was_fragmented ? (char *)buf_peek(conn->recv_buf) : (char *)data;
  payload_len = was_fragmented ? conn->fragments_len : payload_len;

  ssize_t inflated_sz = inflation_stream_inflate(
      s->istrm, (char *)msg, (unsigned)payload_len, inflated_buf->data,
      (unsigned)inflated_buf->cap, true);
  if (unlikely(inflated_sz <= 0)) {
    printf("inflate error\n");
    ws_conn_destroy(conn, WS_ERR_INFLATE);
    return -1;
  }
  inflated_buf->len += (size_t)inflated_sz;

  // non fragmented frame
  if (!was_fragmented) {
    // don't call buf_consume it's already done
    s->on_ws_msg(conn, inflated_buf->data, (size_t)inflated_sz,
                 is_bin(conn) ? OP_BIN : OP_TXT);
    conn->needed_bytes = 2;
    clear_bin(conn);
    inflated_buf->len -= (size_t)inflated_sz;

    if (!inflated_buf->len)
      conn_dyn_buf_dispose(conn);

  } else {
    // fragmented frame
    clear_fragment_compressed(conn);

    buf_consume(conn->recv_buf, conn->fragments_len);

    s->on_ws_msg(conn, inflated_buf->data, (size_t)inflated_sz,
                 is_bin(conn) ? OP_BIN : OP_TXT);

    inflated_buf->len -= (size_t)inflated_sz;

    if (!inflated_buf->len)
      conn_dyn_buf_dispose(conn);

    conn->fragments_len = 0;
    clear_fragmented(conn);
    clear_bin(conn);
    conn->needed_bytes = 2;
  }

  return 0;
}

#endif /* WITH_COMPRESSION */

/*
SHA-1 in C
By Steve Reid <steve@edmweb.com>
100% Public Domain

Test Vectors (from FIPS PUB 180-1)
"abc"
  A9993E36 4706816A BA3E2571 7850C26C 9CD0D89D
"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
  84983E44 1C3BD26E BAAE4AA1 F95129E5 E54670F1
A million repetitions of "a"
  34AA973C D4C4DAA4 F61EEB2B DBAD2731 6534016F
*/

/* #define LITTLE_ENDIAN * This should be #define'd already, if true. */
/* #define SHA1HANDSOFF * Copies data before messing with it. */

typedef struct {
  uint32_t state[5];
  uint32_t count[2];
  unsigned char buffer[64];
} SHA1_CTX;

#define SHA1HANDSOFF

#include <stdio.h>
#include <string.h>

/* for uint32_t */
#include <stdint.h>

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/* blk0() and blk() perform the initial expand. */
/* I got the idea of expanding during the round function from SSLeay */
#if BYTE_ORDER == LITTLE_ENDIAN
#define blk0(i)                                                                \
  (block->l[i] = (rol(block->l[i], 24) & 0xFF00FF00) |                         \
                 (rol(block->l[i], 8) & 0x00FF00FF))
#elif BYTE_ORDER == BIG_ENDIAN
#define blk0(i) block->l[i]
#else
#error "Endianness not defined!"
#endif
#define blk(i)                                                                 \
  (block->l[i & 15] = rol(block->l[(i + 13) & 15] ^ block->l[(i + 8) & 15] ^   \
                              block->l[(i + 2) & 15] ^ block->l[i & 15],       \
                          1))

/* (R0+R1), R2, R3, R4 are the different operations used in SHA1 */
#define R0(v, w, x, y, z, i)                                                   \
  z += ((w & (x ^ y)) ^ y) + blk0(i) + 0x5A827999 + rol(v, 5);                 \
  w = rol(w, 30);
#define R1(v, w, x, y, z, i)                                                   \
  z += ((w & (x ^ y)) ^ y) + blk(i) + 0x5A827999 + rol(v, 5);                  \
  w = rol(w, 30);
#define R2(v, w, x, y, z, i)                                                   \
  z += (w ^ x ^ y) + blk(i) + 0x6ED9EBA1 + rol(v, 5);                          \
  w = rol(w, 30);
#define R3(v, w, x, y, z, i)                                                   \
  z += (((w | x) & y) | (w & x)) + blk(i) + 0x8F1BBCDC + rol(v, 5);            \
  w = rol(w, 30);
#define R4(v, w, x, y, z, i)                                                   \
  z += (w ^ x ^ y) + blk(i) + 0xCA62C1D6 + rol(v, 5);                          \
  w = rol(w, 30);

/* Hash a single 512-bit block. This is the core of the algorithm. */

void SHA1Transform(uint32_t state[5], const unsigned char *buffer) {
  uint32_t a, b, c, d, e;

  typedef union {
    unsigned char c[64];
    uint32_t l[16];
  } CHAR64LONG16;

#ifdef SHA1HANDSOFF
  CHAR64LONG16 block[1]; /* use array to appear as a pointer */

  memcpy(block, buffer, 64);
#else
  /* The following had better never be used because it causes the
   * pointer-to-const buffer to be cast into a pointer to non-const.
   * And the result is written through.  I threw a "const" in, hoping
   * this will cause a diagnostic.
   */
  CHAR64LONG16 *block = (const CHAR64LONG16 *)buffer;
#endif
  /* Copy context->state[] to working vars */
  a = state[0];
  b = state[1];
  c = state[2];
  d = state[3];
  e = state[4];
  /* 4 rounds of 20 operations each. Loop unrolled. */
  R0(a, b, c, d, e, 0);
  R0(e, a, b, c, d, 1);
  R0(d, e, a, b, c, 2);
  R0(c, d, e, a, b, 3);
  R0(b, c, d, e, a, 4);
  R0(a, b, c, d, e, 5);
  R0(e, a, b, c, d, 6);
  R0(d, e, a, b, c, 7);
  R0(c, d, e, a, b, 8);
  R0(b, c, d, e, a, 9);
  R0(a, b, c, d, e, 10);
  R0(e, a, b, c, d, 11);
  R0(d, e, a, b, c, 12);
  R0(c, d, e, a, b, 13);
  R0(b, c, d, e, a, 14);
  R0(a, b, c, d, e, 15);
  R1(e, a, b, c, d, 16);
  R1(d, e, a, b, c, 17);
  R1(c, d, e, a, b, 18);
  R1(b, c, d, e, a, 19);
  R2(a, b, c, d, e, 20);
  R2(e, a, b, c, d, 21);
  R2(d, e, a, b, c, 22);
  R2(c, d, e, a, b, 23);
  R2(b, c, d, e, a, 24);
  R2(a, b, c, d, e, 25);
  R2(e, a, b, c, d, 26);
  R2(d, e, a, b, c, 27);
  R2(c, d, e, a, b, 28);
  R2(b, c, d, e, a, 29);
  R2(a, b, c, d, e, 30);
  R2(e, a, b, c, d, 31);
  R2(d, e, a, b, c, 32);
  R2(c, d, e, a, b, 33);
  R2(b, c, d, e, a, 34);
  R2(a, b, c, d, e, 35);
  R2(e, a, b, c, d, 36);
  R2(d, e, a, b, c, 37);
  R2(c, d, e, a, b, 38);
  R2(b, c, d, e, a, 39);
  R3(a, b, c, d, e, 40);
  R3(e, a, b, c, d, 41);
  R3(d, e, a, b, c, 42);
  R3(c, d, e, a, b, 43);
  R3(b, c, d, e, a, 44);
  R3(a, b, c, d, e, 45);
  R3(e, a, b, c, d, 46);
  R3(d, e, a, b, c, 47);
  R3(c, d, e, a, b, 48);
  R3(b, c, d, e, a, 49);
  R3(a, b, c, d, e, 50);
  R3(e, a, b, c, d, 51);
  R3(d, e, a, b, c, 52);
  R3(c, d, e, a, b, 53);
  R3(b, c, d, e, a, 54);
  R3(a, b, c, d, e, 55);
  R3(e, a, b, c, d, 56);
  R3(d, e, a, b, c, 57);
  R3(c, d, e, a, b, 58);
  R3(b, c, d, e, a, 59);
  R4(a, b, c, d, e, 60);
  R4(e, a, b, c, d, 61);
  R4(d, e, a, b, c, 62);
  R4(c, d, e, a, b, 63);
  R4(b, c, d, e, a, 64);
  R4(a, b, c, d, e, 65);
  R4(e, a, b, c, d, 66);
  R4(d, e, a, b, c, 67);
  R4(c, d, e, a, b, 68);
  R4(b, c, d, e, a, 69);
  R4(a, b, c, d, e, 70);
  R4(e, a, b, c, d, 71);
  R4(d, e, a, b, c, 72);
  R4(c, d, e, a, b, 73);
  R4(b, c, d, e, a, 74);
  R4(a, b, c, d, e, 75);
  R4(e, a, b, c, d, 76);
  R4(d, e, a, b, c, 77);
  R4(c, d, e, a, b, 78);
  R4(b, c, d, e, a, 79);
  /* Add the working vars back into context.state[] */
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  /* Wipe variables */
  a = b = c = d = e = 0;
#ifdef SHA1HANDSOFF
  memset(block, '\0', sizeof(block));
#endif
}

/* SHA1Init - Initialize new context */

void SHA1Init(SHA1_CTX *context) {
  /* SHA1 initialization constants */
  context->state[0] = 0x67452301;
  context->state[1] = 0xEFCDAB89;
  context->state[2] = 0x98BADCFE;
  context->state[3] = 0x10325476;
  context->state[4] = 0xC3D2E1F0;
  context->count[0] = context->count[1] = 0;
}

/* Run your data through this. */

void SHA1Update(SHA1_CTX *context, const unsigned char *data, uint32_t len) {
  uint32_t i;

  uint32_t j;

  j = context->count[0];
  if ((context->count[0] += len << 3) < j)
    context->count[1]++;
  context->count[1] += (len >> 29);
  j = (j >> 3) & 63;
  if ((j + len) > 63) {
    memcpy(&context->buffer[j], data, (i = 64 - j));
    SHA1Transform(context->state, context->buffer);
    for (; i + 63 < len; i += 64) {
      SHA1Transform(context->state, &data[i]);
    }
    j = 0;
  } else
    i = 0;
  memcpy(&context->buffer[j], &data[i], len - i);
}

/* Add padding and return the message digest. */

void SHA1Final(unsigned char digest[20], SHA1_CTX *context) {
  unsigned i;

  unsigned char finalcount[8];

  unsigned char c;

#if 0 /* untested "improvement" by DHR */
    /* Convert context->count to a sequence of bytes
     * in finalcount.  Second element first, but
     * big-endian order within element.
     * But we do it all backwards.
     */
    unsigned char *fcp = &finalcount[8];

    for (i = 0; i < 2; i++)
    {
        uint32_t t = context->count[i];

        int j;

        for (j = 0; j < 4; t >>= 8, j++)
            *--fcp = (unsigned char) t}
#else
  for (i = 0; i < 8; i++) {
    finalcount[i] = (unsigned char)((context->count[(i >= 4 ? 0 : 1)] >>
                                     ((3 - (i & 3)) * 8)) &
                                    255); /* Endian independent */
  }
#endif
  c = 0200;
  SHA1Update(context, &c, 1);
  while ((context->count[0] & 504) != 448) {
    c = 0000;
    SHA1Update(context, &c, 1);
  }
  SHA1Update(context, finalcount, 8); /* Should cause a SHA1Transform() */
  for (i = 0; i < 20; i++) {
    digest[i] =
        (unsigned char)((context->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
  }
  /* Wipe variables */
  memset(context, '\0', sizeof(*context));
  memset(&finalcount, '\0', sizeof(finalcount));
}

void SHA1(char *hash_out, const char *str, uint32_t len) {
  SHA1_CTX ctx;
  unsigned int ii;

  SHA1Init(&ctx);
  for (ii = 0; ii < len; ii += 1)
    SHA1Update(&ctx, (const unsigned char *)str + ii, 1);
  SHA1Final((unsigned char *)hash_out, &ctx);
}

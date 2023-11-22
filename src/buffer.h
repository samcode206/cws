#ifndef __X_BUFFPOOL_LIB_14
#define __X_BUFFPOOL_LIB_14
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

typedef struct {
  size_t rpos;
  size_t wpos;
  size_t buf_sz;
  uint8_t *buf;
} buf_t;

static inline size_t buf_len(buf_t *r) { return r->wpos - r->rpos; }

static inline void buf_reset(buf_t *r) {
  // we can do this because indexes are in the beginning
  memset(r, 0, sizeof(size_t) * 2);
}

static inline size_t buf_space(buf_t *r) {
  return r->buf_sz - (r->wpos - r->rpos);
}

static inline int buf_put(buf_t *r, const void *data, size_t n) {
  if (buf_space(r) < n) {
    return -1;
  }
  memcpy(r->buf + r->wpos, data, n);
  r->wpos += n;
  return 0;
}

static inline uint8_t *buf_peek(buf_t *r) { return r->buf + r->rpos; }

static inline uint8_t *buf_peek_at(buf_t *r, size_t at) { return r->buf + at; }

static inline uint8_t *buf_peekn(buf_t *r, size_t n) {
  if (buf_len(r) < n) {
    return NULL;
  }

  return r->buf + r->rpos;
}

// static inline void buf_reset(buf_t *r) {
//   size_t eq_mask = -((r->rpos ^ r->wpos) == 0);
//   r->rpos &= ~eq_mask;
//   r->wpos &= ~eq_mask;
// }

static inline int buf_consume(buf_t *r, size_t n) {
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

static inline void buf_move(buf_t *src_b, buf_t *dst_b, size_t n) {
  buf_put(dst_b, src_b->buf + src_b->rpos, n);
  buf_consume(src_b, n);
}

static inline void buf_debug(buf_t *r, const char *label) {
  printf("%s rpos=%zu wpos=%zu\n", label, r->rpos, r->wpos);
}

static inline ssize_t buf_recv(buf_t *r, int fd, size_t len, int flags) {
  ssize_t n = recv(fd, r->buf + r->wpos, len, flags);

  r->wpos += (n > 0) * n;
  return n;
}

static inline ssize_t buf_send(buf_t *r, int fd, int flags) {
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

/*
 * writes two io vectors first is a header and the second is a payload
 * returns total written and copies any leftover if we didn't drain the buffer
 */
static inline ssize_t buf_write2v(buf_t *r, int fd, struct iovec const *iovs,
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

static inline ssize_t buf_drain_write2v(buf_t *r, struct iovec const *iovs,
                                        size_t const total, buf_t *rem_dst,
                                        int fd) {

  buf_t *dst;
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

struct buf_node {
  void *b;
  struct buf_node *next;
};

struct mbuf_pool {
  size_t avb;
  size_t cap;
  struct buf_pool *pool;
  buf_t **avb_list;
  buf_t *mirrored_bufs;
};

struct mbuf_pool *mbuf_pool_create(uint32_t nmemb, size_t buf_sz);

buf_t *mbuf_get(struct mbuf_pool *bp);

void mbuf_put(struct mbuf_pool *bp, buf_t *buf);

#endif // __X_BUFFPOOL_LIB_14

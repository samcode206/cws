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

#ifndef __X_BUFF_LIB_14
#define __X_BUFF_LIB_14

#include "pool.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
  size_t rpos;
  size_t wpos;
  size_t buf_sz;
  uint8_t *buf;
} buf_t;

static inline int buf_init(struct buf_pool *p, buf_t *r) {

  r->buf = (uint8_t *)buf_pool_alloc(p);
  if (r->buf == NULL) {
    return -1;
  }

  memset(r, 0, sizeof(buf_t) - offsetof(buf_t, buf_sz));
  r->buf_sz = p->buf_sz;

  return 0;
}

static inline size_t buf_len(buf_t *r) { return r->wpos - r->rpos; }

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
    r->rpos = 0;
    r->wpos = 0;
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

static inline ssize_t buf_recv(buf_t *r, int fd, int flags) {
  ssize_t n = recv(fd, r->buf + r->wpos, buf_space(r), flags);
  r->wpos += (n > 0) * n;
  return n;
}

static inline ssize_t buf_send(buf_t *r, int fd, int flags) {
  ssize_t n = send(fd, r->buf + r->rpos, buf_len(r), flags);
  r->rpos += (n > 0) * n;

    if (r->rpos == r->wpos) {
    r->rpos = 0;
    r->wpos = 0;
  } else {
    int ovf = (r->rpos > r->buf_sz) * r->buf_sz;
    r->rpos -= ovf;
    r->wpos -= ovf;
  }



  return n;
}

#endif

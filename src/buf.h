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

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define RBUF_SIZE 8192

typedef struct {
  size_t tail;
  size_t head;
  int fd;
  uint8_t *buf;
} buf_t;

static inline int buf_init(buf_t *r) {
  if (memset(r, 0, sizeof *r) == NULL) {
    return -1;
  };

  r->fd = memfd_create("buf", 0);
  if (r->fd == -1) {
    return -1;
  }

  if (ftruncate(r->fd, RBUF_SIZE) == -1) {
    return -1;
  };

  void *base =
      mmap(NULL, RBUF_SIZE * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) {
    return -1;
  }
  r->buf = (uint8_t *)base;

  if ((base = mmap(r->buf, RBUF_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_FIXED, r->fd, 0)) == MAP_FAILED) {
    return -1;
  };


  if (base != r->buf) {
    return -1;
  }

  if ((base = mmap(r->buf + RBUF_SIZE, RBUF_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_FIXED, r->fd, 0)) == MAP_FAILED) {
    return -1;
  };

  if (base != r->buf + RBUF_SIZE) {
    return -1;
  }

  return 0;
}

static inline size_t buf_len(buf_t *r) { return r->head - r->tail; }

static inline int buf_put(buf_t *r, const void *data, size_t n) {
  if (RBUF_SIZE - (r->head - r->tail) < n) {
    return -1;
  }
  memcpy(r->buf + r->head, data, n);
  r->head += n;
  return 0;
}

static inline int buf_cpy(buf_t *r, void *data, size_t n) {
  if (r->head - r->tail < n) {
    return -1;
  }
  memcpy(data, r->buf + r->tail, n);
  r->tail += n;
  if (r->tail > RBUF_SIZE) {
    r->tail -= RBUF_SIZE;
    r->head -= RBUF_SIZE;
  }

  return 0;
}

static inline uint8_t *buf_peek(buf_t *r) { return r->buf + r->tail; }

static inline uint8_t *buf_peekn(buf_t *r, size_t n) {
  if (r->head - r->tail < n) {
    return NULL;
  }

  return r->buf + r->tail;
}

static inline int buf_consume(buf_t *r, size_t n) {
  if (r->head - r->tail < n) {
    return -1;
  }

  r->tail += n;
  if (r->tail > RBUF_SIZE) {
    r->tail -= RBUF_SIZE;
    r->head -= RBUF_SIZE;
  }

  return 0;
}

static inline ssize_t buf_recv(buf_t *r, int fd, int flags) {
  ssize_t n =
      recv(fd, r->buf + r->head, RBUF_SIZE - (r->head - r->tail), flags);
  r->head += (n > 0) * n;
  return n;
}

static inline ssize_t buf_send(buf_t *r, int fd, int flags) {
  ssize_t n = send(fd, r->buf + r->tail, r->head - r->tail, flags);
  r->tail += (n > 0) * n;
  if (r->tail > RBUF_SIZE) {
    r->tail -= RBUF_SIZE;
    r->head -= RBUF_SIZE;
  }
  return n;
}

#endif

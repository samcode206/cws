#ifndef __X_BUFFPOOL_LIB_14
#define __X_BUFFPOOL_LIB_14

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

struct buf_node {
  void *b;
  struct buf_node *next;
};

struct buf_pool {
  int fd;
  uint32_t nmemb;
  size_t buf_sz;
  void *base;
  struct buf_node *head;
  struct buf_node _buf_nodes[];
};

struct buf_pool *buf_pool_create(uint32_t nmemb, size_t buf_sz);

void *buf_pool_alloc(struct buf_pool *p);

void buf_pool_free(struct buf_pool *p, void *buf);

void buf_pool_destroy(struct buf_pool *p);

static inline size_t buf_pool_file_offset(struct buf_pool const *p,
                                          void const *buf) {
  return ((uintptr_t)buf - (uintptr_t)p->base) / (p->buf_sz + p->buf_sz) * p->buf_sz;
}

#endif // __X_BUFFPOOL_LIB_14

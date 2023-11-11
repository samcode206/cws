#define _GNU_SOURCE
#include "pool.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static inline size_t aligned_size(size_t sz, size_t pow2_alignment) {
  if (pow2_alignment & (pow2_alignment - 1)) {
    fprintf(stderr,
            "aligned_size(): alignment must be a power of two got: %zu\n",
            pow2_alignment);
    exit(EXIT_FAILURE);
  }

  return (sz + pow2_alignment - 1) & ~(pow2_alignment - 1);
}

struct buf_pool *buf_pool_init(uint32_t nmemb, size_t buf_sz) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size == -1) {
    fprintf(stderr, "sysconf(_SC_PAGESIZE): failed to determine page size\n");
    exit(1);
  }

  if (buf_sz % page_size) {
    return NULL;
  }

  size_t pool_sz = (sizeof(struct buf_pool) +
                    (nmemb * sizeof(struct buf_node)) + page_size - 1) &
                   ~(page_size - 1);
  size_t buf_pool_sz = buf_sz * nmemb * 2; // size of buffers

  printf("buf_pool size = %zu\n", pool_sz + buf_pool_sz);
  void *pool_mem = mmap(NULL, pool_sz + buf_pool_sz, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (pool_mem == MAP_FAILED) {
    perror("mmap()");
    return NULL;
  }

  if (mprotect(pool_mem, pool_sz, PROT_READ | PROT_WRITE) == -1) {
    perror("mprotect()");
    return NULL;
  };

  struct buf_pool *pool = pool_mem;

  pool->fd = memfd_create("buf", 0);
  pool->buf_sz = buf_sz;
  pool->nmemb = nmemb;
  pool->base = ((uint8_t *)pool_mem) + pool_sz;

  if (ftruncate(pool->fd, buf_sz * nmemb) == -1) {
    perror("ftruncate()");
    return NULL;
  };

  uint32_t i;

  uint8_t *pos = pool->base;
  size_t offset = 0;

  for (i = 0; i < nmemb; ++i) {
    if (mmap(pos, buf_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
             pool->fd, offset) == MAP_FAILED) {
      perror("mmap()");
      close(pool->fd);
      return NULL;
    };

    if (mmap(pos + buf_sz, buf_sz, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_FIXED, pool->fd, offset) == MAP_FAILED) {
      perror("mmap()");
      close(pool->fd);
      return NULL;
    };

    pool->_buf_nodes[i].b = pos;

    offset += buf_sz;
    pos = pos + buf_sz + buf_sz;
  }

  for (i = 0; i < nmemb - 1; i++) {
    pool->_buf_nodes[i].next = &pool->_buf_nodes[i + 1];
  }

  pool->_buf_nodes[nmemb - 1].next = NULL;

  pool->head = &pool->_buf_nodes[0];

  return pool;
}

void *buf_pool_alloc(struct buf_pool *p) {
  if (p->head) {
    struct buf_node *bn = p->head;
    p->head = p->head->next;
    bn->next = NULL; // unlink the buf_node mainly useful for debugging
    return bn->b;
  } else {
    return NULL;
  }
}

void buf_pool_destroy(struct buf_pool *p) {
  long page_size = sysconf(_SC_PAGESIZE);

  size_t pool_sz = (sizeof(struct buf_pool) +
                    (p->nmemb * sizeof(struct buf_node)) + page_size - 1) &
                   ~(page_size - 1);

  size_t buf_pool_sz = p->buf_sz * p->nmemb * 2;

  void *pool_mem_addr = ((uint8_t *)p->base) - pool_sz;

  assert(close(p->fd) == 0);
  assert(munmap(pool_mem_addr, pool_sz + buf_pool_sz) == 0);
}

void buf_pool_free(struct buf_pool *p, void *buf) {
  uintptr_t diff =
      ((uintptr_t)buf - (uintptr_t)p->base) / (p->buf_sz + p->buf_sz);

  p->_buf_nodes[diff].next = p->head;
  p->head = &p->_buf_nodes[diff];
}

struct conn_pool *conn_pool_init(uint32_t nmemb, size_t conn_sz) {
  long page_size = sysconf(_SC_PAGESIZE);
  size_t aligned_pool_structure_sz = aligned_size(
      sizeof(struct conn_pool) + (nmemb * sizeof(struct buf_node)), page_size);
  size_t aligned_conn_sz = aligned_size(conn_sz, 64);

  assert(aligned_conn_sz == conn_sz);

  size_t aligned_total_conns_sz =
      aligned_size(aligned_conn_sz * nmemb, page_size);

  size_t aligned_total_size = aligned_size(
      aligned_total_conns_sz + aligned_pool_structure_sz, page_size);

  printf("aligned_pool_structure_sz = %zu\n", aligned_pool_structure_sz);
  printf("aligned_conn_sz = %zu\n", aligned_conn_sz);
  printf("aligned_total_conns_sz = %zu\n", aligned_total_conns_sz);
  printf("aligned_total_size = %zu\n", aligned_total_size);

  void *pool_mem = mmap(NULL, aligned_total_size, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

  if (pool_mem == MAP_FAILED) {
    perror("mmap()");
    exit(EXIT_FAILURE);
  }
  struct conn_pool *pool = pool_mem;

  pool->base = ((uint8_t *)pool_mem) + aligned_pool_structure_sz;

  pool->item_size = aligned_conn_sz;

  uint8_t *pos = pool->base;
  size_t offset = aligned_pool_structure_sz;

  for (size_t i = 0; i < nmemb - 1; ++i) {
    pool->_buf_nodes[i].next = &pool->_buf_nodes[i + 1];
    pool->_buf_nodes[i].b = pos;
    pos = pos + aligned_conn_sz;
    // printf("pos = %zu\n", (uintptr_t)pos);
    // printf("aligned = %s\n", ((uintptr_t)pos % 64) == 0 ? "Yes" : "No");

    offset += aligned_conn_sz;
  }

  pool->_buf_nodes[nmemb - 1].next = NULL;
  pool->_buf_nodes[nmemb - 1].b = pos;
  pool->head = &pool->_buf_nodes[0];

  if (offset >= aligned_total_size) {
    fprintf(stderr, "exceeded connection pool size %zu >= %zu \n",
            offset + aligned_conn_sz, aligned_total_size);
    exit(EXIT_FAILURE);
  }

  return pool;
}

void *conn_pool_alloc(struct conn_pool *p) {
  if (p->head) {
    struct buf_node *bn = p->head;
    p->head = p->head->next;
    bn->next = NULL; // unlink the buf_node mainly useful for debugging
    return bn->b;
  } else {
    return NULL;
  }
}

void conn_pool_free(struct conn_pool *p, void *c) {
  uintptr_t diff = ((uintptr_t)c - (uintptr_t)p->base) / (p->item_size);
  p->_buf_nodes[diff].next = p->head;
  p->head = &p->_buf_nodes[diff];
}

#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// #define BUF_POOL_DIAGNOSE

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


  void *pool_mem = mmap(NULL, pool_sz + buf_pool_sz, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (pool_mem == MAP_FAILED) {
    return NULL;
  }

  if (mprotect(pool_mem, pool_sz, PROT_READ | PROT_WRITE) == -1) {
    return NULL;
  };

  struct buf_pool *pool = pool_mem;


  pool->fd = memfd_create("buf", 0);
  pool->buf_sz = buf_sz;
  pool->nmemb = nmemb;
  pool->base = ((uint8_t *)pool_mem) + pool_sz;


  if (ftruncate(pool->fd, buf_sz * nmemb) == -1) {
    return NULL;
  };

  uint32_t i;

  uint8_t *pos = pool->base;
  size_t offset = 0;

  for (i = 0; i < nmemb; ++i) {
    if (mmap(pos, buf_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_POPULATE,
             pool->fd, offset) == MAP_FAILED) {
      close(pool->fd);
      return NULL;
    };

    if (mmap(pos + buf_sz, buf_sz, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_FIXED | MAP_POPULATE, pool->fd, offset) == MAP_FAILED) {
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

#define BUF_SIZE 1024 * 8
int main() {
  struct buf_pool *p = buf_pool_init(4, BUF_SIZE);
  assert(p != NULL);

  uint8_t *b1 = buf_pool_alloc(p);
  uint8_t *b2 = buf_pool_alloc(p);

  uint8_t *b3 = buf_pool_alloc(p);
  uint8_t *b4 = buf_pool_alloc(p);
  uint8_t *b5 = buf_pool_alloc(p);

  assert(b5 == NULL);

  assert(((uintptr_t)b2 - (uintptr_t)b1) == BUF_SIZE + BUF_SIZE);
  assert(((uintptr_t)b4 - (uintptr_t)b3) == BUF_SIZE + BUF_SIZE);

  b1[0] = 'a';
  b1[BUF_SIZE + 1] = 'b';

  b2[0] = 'c';
  b2[BUF_SIZE + 1] = 'd';

  b3[BUF_SIZE] = 'e';
  b3[BUF_SIZE + 1] = 'f';

  b4[BUF_SIZE] = 'g';
  b4[BUF_SIZE + 1] = 'h';

  assert(!strcmp((const char *)b1, "ab"));
  assert(!strcmp((const char *)b2, "cd"));
  assert(!strcmp((const char *)b3, "ef"));
  assert(!strcmp((const char *)b4, "gh"));
  buf_pool_free(p, b4);

  assert(b4 == buf_pool_alloc(p));

  assert(buf_pool_alloc(p) == NULL);

  buf_pool_destroy(p);


}

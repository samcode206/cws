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
  int nmemb;
  size_t buf_sz;
  void *base;
  struct buf_node *head;
  struct buf_node _buf_nodes[];
};

struct buf_pool *buf_pool_init(int nmemb, size_t buf_sz) {
  struct buf_pool *pool =
      malloc(sizeof(struct buf_pool) + (nmemb * sizeof(struct buf_node)));

  pool->fd = memfd_create("buf", 0);
  pool->buf_sz = buf_sz;
  pool->nmemb = nmemb;

  if (ftruncate(pool->fd, buf_sz * nmemb) == -1) {
    return NULL;
  };

  pool->base = mmap(NULL, buf_sz * nmemb * 2, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (pool->base == MAP_FAILED) {
    close(pool->fd);
    return NULL;
  }

  int i;

  uint8_t *pos = pool->base;
  size_t offset = 0;

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

void buf_pool_free(struct buf_pool *p) {
  assert(munmap(p->base, (p->buf_sz * p->nmemb) * 2) == 0);
  assert(close(p->fd) == 0);
  free(p);
}

void buf_pool_free_buf(struct buf_pool *p, void *buf) {
#ifdef BUF_POOL_DIAGNOSE
  assert(buf != NULL);
#endif

  // find corresponding buf_node based on buf's address
  // once found make that buf_node's next
  // the current head and then make buf_node p->head
  uintptr_t diff =
      ((uintptr_t)buf - (uintptr_t)p->base) / (p->buf_sz + p->buf_sz);

#ifdef BUF_POOL_DIAGNOSE
  assert((uintptr_t)buf > (uintptr_t)p->base);
  assert(p->_buf_nodes[diff].b == buf);
  assert(p->_buf_nodes[diff].next == NULL);
#endif

  p->_buf_nodes[diff].next = p->head;
  p->head = &p->_buf_nodes[diff];
}

#define BUF_SIZE 4096
int main() {
  struct buf_pool *p = buf_pool_init(4, BUF_SIZE);

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
  buf_pool_free_buf(p, b4);

  assert(b4 == buf_pool_alloc(p));

  assert(buf_pool_alloc(p) == NULL);

  buf_pool_free(p);

  printf("[SUCCESS]\n");
}

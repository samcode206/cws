#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "buffer.h"



struct buf_pool {
  int fd;
  uint32_t nmemb;
  size_t buf_sz;
  void *base;
  struct buf_node *head;
  struct buf_node _buf_nodes[];
};


static struct buf_pool *buf_pool_create(uint32_t nmemb, size_t buf_sz) {
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

static void *buf_pool_alloc(struct buf_pool *p) {
  if (p->head) {
    struct buf_node *bn = p->head;
    p->head = p->head->next;
    bn->next = NULL; // unlink the buf_node mainly useful for debugging
    return bn->b;
  } else {
    return NULL;
  }
}

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

// static void buf_pool_free(struct buf_pool *p, void *buf) {
//   uintptr_t diff =
//       ((uintptr_t)buf - (uintptr_t)p->base) / (p->buf_sz + p->buf_sz);


//   p->_buf_nodes[diff].next = p->head;
//   p->head = &p->_buf_nodes[diff];
// }



static inline int buf_init(struct buf_pool *p, buf_t *r) {

  r->buf = (uint8_t *)buf_pool_alloc(p);
  if (r->buf == NULL) {
    return -1;
  }

  memset(r, 0, sizeof(buf_t) - offsetof(buf_t, buf_sz));
  r->buf_sz = p->buf_sz;

  return 0;
}






struct mbuf_pool *mbuf_pool_create(uint32_t nmemb, size_t buf_sz) {

  struct mbuf_pool *p = (struct mbuf_pool*)calloc(1, sizeof(struct mbuf_pool));
  p->avb = nmemb;
  p->cap = nmemb;
  assert(posix_memalign((void **)&p->mirrored_bufs, 64, sizeof(buf_t) * nmemb) ==
         0);
  p->avb_list = (buf_t **)calloc(nmemb, sizeof(buf_t *));
  assert(p->avb_list != NULL);
  p->pool = buf_pool_create(nmemb, buf_sz);
  assert(p->pool != NULL);

  size_t i = nmemb;

  while (i--) {
    assert(buf_init(p->pool, &p->mirrored_bufs[i]) == 0);
  }

  i = nmemb;

  while (i--) {
    p->avb_list[i] = &p->mirrored_bufs[i];
  }

  return p;
}

buf_t *mbuf_get(struct mbuf_pool *bp) {
  while (bp->avb) {
    return bp->avb_list[--bp->avb];
  }

  return NULL;
}


void mbuf_put(struct mbuf_pool *bp, buf_t *buf){
  buf->rpos = 0;
  buf->wpos = 0; 
  bp->avb_list[bp->avb++] = buf;
}


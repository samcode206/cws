#include "../../src/ws.c"

#include "test.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

int TEST_MIRRORED_BUF_POOL_CREATE_DESTROY(const char *name) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size == -1) {
    fprintf(stderr, "sysconf(_SC_PAGESIZE): failed to determine page size\n");
    exit(1);
  }

  size_t max_size = page_size * 8;

  for (size_t buf_sz = page_size; buf_sz < max_size; buf_sz += page_size) {

    for (size_t nmemb = 1; nmemb < 1024; nmemb++) {
      ws_server_t *s = malloc(sizeof(ws_server_t));
      assert(s != NULL);
      struct mirrored_buf_pool *p = mirrored_buf_pool_create(nmemb, buf_sz, 1);
      assert(p != NULL);
      s->buffer_pool = p;
      server_mirrored_buf_pool_destroy(s);

      free(s);
    }
  }

  return EXIT_SUCCESS;
}

int TEST_MIRRORED_BUF_POOL_USE_ALL_BUFS(const char *name) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size == -1) {
    fprintf(stderr, "sysconf(_SC_PAGESIZE): failed to determine page size\n");
    exit(1);
  }

  size_t max_size = page_size * 8;

  for (size_t buf_sz = page_size; buf_sz < max_size; buf_sz += page_size) {

    for (size_t nmemb = 1; nmemb < 1024; nmemb++) {
      ws_server_t *s = malloc(sizeof(ws_server_t));
      assert(s != NULL);
      struct mirrored_buf_pool *p = mirrored_buf_pool_create(nmemb, buf_sz, 1);
      assert(p != NULL);

      mirrored_buf_t **bufs = malloc(sizeof(mirrored_buf_t *) * nmemb);

      for (size_t j = 0; j < nmemb; j++) {
        mirrored_buf_t *b = mirrored_buf_get(p);
        bufs[j] = b;
        assert(b != NULL);
        assert(b->buf != NULL);
        assert(b->buf_sz == buf_sz);
        assert(b->rpos == 0);
        assert(b->wpos == 0);
        assert(p->avb == nmemb - j - 1);
      }

      assert(p->avb == 0);

      for (size_t j = 0; j < nmemb; j++) {
        bufs[j]->rpos = 14;
        bufs[j]->wpos = 314;
        mirrored_buf_put(p, bufs[j]);
        assert(bufs[j]->rpos == 0);
        assert(bufs[j]->wpos == 0);
        assert(p->avb == j + 1);
      }

      assert(p->avb == nmemb);
      free(bufs);

      s->buffer_pool = p;
      server_mirrored_buf_pool_destroy(s);

      free(s);
    }
  }

  return EXIT_SUCCESS;
}

int TEST_MIRRORED_BUF_RING_BUFFER_BEHAVIOR(const char *name) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size == -1) {
    fprintf(stderr, "sysconf(_SC_PAGESIZE): failed to determine page size\n");
    exit(1);
  }

  ws_server_t *s = malloc(sizeof(ws_server_t));
  assert(s != NULL);
  struct mirrored_buf_pool *p = mirrored_buf_pool_create(1, page_size, 1);
  assert(p != NULL);
  s->buffer_pool = p;

  mirrored_buf_t *b = mirrored_buf_get(p);
  assert(b->buf_sz == page_size);
  assert(b->buf != NULL);

  b->buf[page_size] = 'a';
  b->buf[page_size + 1] = 'b';
  b->buf[page_size + 2] = 'c';
  b->buf[page_size + 3] = '\0';

  // we should be able to read the data from the start of buffer
  assert(strcmp((char *)b->buf, "abc") == 0);
    


  server_mirrored_buf_pool_destroy(s);

  free(s);

  return EXIT_SUCCESS;
}

#define NUM_TESTS 2

struct test_table BUF_POOL_TESTSUITE[NUM_TESTS] = {
    {"Creating/Destroying Mirrored buffer pools",
     TEST_MIRRORED_BUF_POOL_CREATE_DESTROY},
    {
        "Using all buffers in a mirrored buffer pool",
        TEST_MIRRORED_BUF_POOL_USE_ALL_BUFS,
    }};

int main(void) { RUN_TESTS("buffer pool", BUF_POOL_TESTSUITE, NUM_TESTS); }

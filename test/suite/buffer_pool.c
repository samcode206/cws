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




#define NUM_TESTS 1

struct test_table BUF_POOL_TESTSUITE[NUM_TESTS] = {
    {"Creating/Destroying Mirrored buffer pools",
     TEST_MIRRORED_BUF_POOL_CREATE_DESTROY},
};

int main(void) { RUN_TESTS("buffer pool", BUF_POOL_TESTSUITE, NUM_TESTS); }

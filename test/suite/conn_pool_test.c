#include "../../src/ws.c"

#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

int TEST_WS_CONN_POOL_CREATE(const char *name) {
  size_t nmemb = 1;

  for (; nmemb < 1024; nmemb++) {
    struct ws_conn_pool *p = ws_conn_pool_create(nmemb);
    if (p == NULL) {
      fprintf(stderr, "[FAIL] %s : expected to have allocate a connection pool",
              name);
      return EXIT_FAILURE;
    }

    ws_conn_t *conns[nmemb];
    memset(conns, 0, sizeof(ws_conn_t *) * nmemb);

    size_t i = 0;
    while (i < nmemb) {
      ws_conn_t *c = ws_conn_get(p);
      if (!c) {
        fprintf(stderr,
                "%s : expected to have gotten a connection from the "
                "connection pool",
                name);
        return EXIT_FAILURE;
      }
      conns[i++] = c;
    }

    if (p->avb != 0) {
      fprintf(stderr,
              "%s : expected to have 0 available connections"
              "connection pool",
              name);
      return EXIT_FAILURE;
    }

    i = 0;
    while (i < nmemb) {
      ws_conn_put(p, conns[i++]);
    }

    if (p->avb != nmemb) {
      fprintf(stderr,
              "%s expected to have %zu available connections"
              "connection pool",
              name, nmemb);
      return EXIT_FAILURE;
    }

    // should be able to get 8 connections after putting all back
    i = 0;
    while (i < nmemb) {
      ws_conn_t *c = ws_conn_get(p);
      if (!c) {
        fprintf(stderr,
                "%s : expected to have gotten a connection from the "
                "connection pool",
                name);
        return EXIT_FAILURE;
      }
      conns[i++] = c;
    }

    ws_server_t *s = malloc(sizeof(ws_server_t));
    s->conn_pool = p;

    server_ws_conn_pool_destroy(s);
    free(s);
  }

  return EXIT_SUCCESS;
}

int TEST_WS_CONN_POOL_EMPTY(const char *name) {
  ws_server_t *s = calloc(1, sizeof(ws_server_t));

  size_t nmemb = 8;
  struct ws_conn_pool *p = ws_conn_pool_create(nmemb);

  s->conn_pool = p;

  size_t i = 0;
  while (i < nmemb) {
    ws_conn_t *c = ws_conn_get(p);
    (void)c;
    i++;
  }

  if (ws_conn_get(p) != NULL) {
    fprintf(stderr,
            "[FAIL] %s : expected to conn to be null after getting all "
            "connections from pool"
            "connection pool",
            name);
    return EXIT_FAILURE;
  }

  server_ws_conn_pool_destroy(s);
  free(s);

  return EXIT_SUCCESS;
}

#define NUM_TESTS 2

struct test_table CONN_POOL_TESTSUITE[NUM_TESTS] = {
    {"ws_conn_pool_create", TEST_WS_CONN_POOL_CREATE},
    {"ws_conn_pool_empty", TEST_WS_CONN_POOL_EMPTY},
};

int main(void) { RUN_TESTS("ws_conn_pool", CONN_POOL_TESTSUITE, NUM_TESTS); }

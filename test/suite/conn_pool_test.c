#include "../../src/ws.c"

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

int test_ws_conn_pool_create(const char *name) {
  size_t nmemb = 1;

  void *pool_mem = malloc(1024 * 1024);

  for (; nmemb < 1024; nmemb++) {
    struct ws_conn_pool *p = ws_conn_pool_create(pool_mem, nmemb);
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
                "[FAIL] %s : expected to have gotten a connection from the "
                "connection pool",
                name);
        return EXIT_FAILURE;
      }
      conns[i++] = c;
    }

    if (p->avb != 0) {
      fprintf(stderr,
              "[FAIL] %s : expected to have 0 available connections"
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
              "[FAIL] %s : expected to have %zu available connections"
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
                "[FAIL] %s : expected to have gotten a connection from the "
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

  fprintf(stdout, "[PASS] %s\n", name);

  return EXIT_SUCCESS;
}

int test_ws_conn_pool_too_many_gets(const char *name) {
  void *pool_mem = malloc(1024 * 1024);
  size_t nmemb = 8;
  struct ws_conn_pool *p = ws_conn_pool_create(pool_mem,nmemb);

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

  fprintf(stdout, "[PASS] %s\n", name);
  return EXIT_SUCCESS;
}

void result(int *ret, int cur) {
  if (*ret == EXIT_SUCCESS) {
    if (cur != EXIT_SUCCESS) {
      *ret = cur;
    }
  }
}

int main(void) {
  int ret = 0;

  result(&ret, test_ws_conn_pool_create(
                   "creating/deleting/getting/putting connection in the "
                   "connection pool of different sizes"));

  result(&ret,
         test_ws_conn_pool_too_many_gets("getting a connection from the pool "
                                         "when empty should return NULL"));

  exit(ret);
}

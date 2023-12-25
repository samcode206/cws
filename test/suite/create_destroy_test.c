#include "../../src/ws.c"

#include "test.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void onOpen(ws_conn_t *conn) {}

void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {}

void onDisconnect(ws_conn_t *conn, int err) {}

int TEST_WS_CREATE_BAD_ARGS(const char *name) {
  struct ws_server_params p = {0};

  if (ws_server_create(&p) != NULL) {
    fprintf(stderr,
            "%s : expect server to be null when no valid arguments "
            "are provided to ws_server_create\n",
            name);
    return EXIT_FAILURE;
  } else {
  };

  return EXIT_SUCCESS;
}

int TEST_WS_CREATE_MISSING_CBS(const char *name) {
  struct ws_server_params p = {
      .port = 9919,
      .addr = "::1",
      .on_ws_msg = NULL,
      .on_ws_open = NULL,
      .on_ws_disconnect = NULL,
      .max_conns = 1,
      .max_buffered_bytes = 1,
  };

  ws_server_t *s;

  if ((s = ws_server_create(&p)) != NULL) {
    fprintf(stderr,
            "%s : expect server to be null when a mandatory callback is "
            "missing"
            "\n",
            name);
    return EXIT_FAILURE;
  }

  p.on_ws_open = onOpen;
  if ((s = ws_server_create(&p)) != NULL) {
    fprintf(stderr,
            "%s : expect server to be null when a mandatory callback is "
            "missing"
            "\n",
            name);
    return EXIT_FAILURE;
  }

  p.on_ws_msg = onMsg;
  if ((s = ws_server_create(&p)) != NULL) {
    fprintf(stderr,
            "%s : expect server to be null when a mandatory callback is "
            "missing"
            "\n",
            name);
    return EXIT_FAILURE;
  }

  p.on_ws_disconnect = onDisconnect;
  if ((s = ws_server_create(&p)) == NULL) {
    fprintf(stderr,
            "%s : expect server to have been created when no mandatory is"
            "missing"
            "\n",
            name);
    return EXIT_FAILURE;
  } else {
    assert(ws_server_destroy(s) == 0);
    return EXIT_SUCCESS;
  }
}

int TEST_WS_CREATE_IPV4_IPV6(const char *name) {
  struct ws_server_params p = {
      .port = 9917,
      .addr = "127.0.0.1",
      .on_ws_msg = onMsg,
      .on_ws_open = onOpen,
      .on_ws_disconnect = onDisconnect,
      .max_conns = 1,
      .max_buffered_bytes = 1,
  };

  ws_server_t *s;

  if ((s = ws_server_create(&p)) == NULL) {
    fprintf(stderr,
            "%s : expected server to be created with ipv4 address"
            "\n",
            name);
    return EXIT_FAILURE;
  }

  assert(ws_server_destroy(s) == 0);

  p.port = 9919;
  p.addr = "::1";

  if ((s = ws_server_create(&p)) == NULL) {
    fprintf(stderr,
            "%s : expected server to be created with ipv6 address"
            "\n",
            name);
    return EXIT_FAILURE;
  }

  assert(ws_server_destroy(s) == 0);

  return EXIT_SUCCESS;
}

int TEST_WS_CREATE_MAX_BUFFERED_BYTES(const char *name) {
  struct ws_server_params p = {
      .port = 9919,
      .addr = "::1",
      .on_ws_msg = onMsg,
      .on_ws_open = onOpen,
      .on_ws_disconnect = onDisconnect,
      .max_conns = 1,
      .max_buffered_bytes = 1,
  };

  ws_server_t *s = ws_server_create(&p);
  if (s == NULL) {
    fprintf(stderr,
            "%s : expected server to be created"
            "\n",
            name);
    return EXIT_FAILURE;
  }

  if (s->buffer_pool->buf_sz != getpagesize()) {
    fprintf(stderr,
            "%s : expected buffer size to be %d got %zu"
            "\n",
            name, getpagesize(), s->buffer_pool->buf_sz);
    return EXIT_FAILURE;
  }

  assert(ws_server_destroy(s) == 0);

  return EXIT_SUCCESS;
}

#define NUM_TESTS 4

struct test_table WS_CREATE_TESTSUITE[NUM_TESTS] = {
    {"ws_server_create bad args", TEST_WS_CREATE_BAD_ARGS},
    {"ws_server_create missing callbacks", TEST_WS_CREATE_MISSING_CBS},
    {"ws_server_create ipv4 ipv6", TEST_WS_CREATE_IPV4_IPV6},
    {"ws_server_create max buffered bytes", TEST_WS_CREATE_MAX_BUFFERED_BYTES},
};

int main(void) {
  RUN_TESTS("ws create/destroy", WS_CREATE_TESTSUITE, NUM_TESTS);
}

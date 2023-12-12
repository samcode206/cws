#include "../../src/ws.c"

#include <stdio.h>
#include <stdlib.h>

void onOpen(ws_conn_t *conn) {}

void onMsg(ws_conn_t *conn, void *msg, size_t n, bool bin) {}

void onDisconnect(ws_conn_t *conn, int err) {}

int test_ws_server_create_bad_args(const char *name) {
  struct ws_server_params p = {0};

  if (ws_server_create(&p) != NULL) {
    fprintf(stderr,
            "[FAIL] %s : expect server to be null when no valid arguments "
            "are provided to ws_server_create\n",
            name);
    return EXIT_FAILURE;
  } else {
    fprintf(stdout, "[PASS] %s\n", name);
  };

  return EXIT_SUCCESS;
}

int test_ws_server_create_no_mandatory_cbs(const char *name) {
  struct ws_server_params p = {
      .port = 9919,
      .addr = "::1",
      .on_ws_msg = NULL,
      .on_ws_open = NULL,
      .on_ws_disconnect = NULL,
      .max_conns = 1,
      .max_buffered_bytes = 1,
  };

  if (ws_server_create(&p) != NULL) {
    fprintf(stderr,
            "[FAIL] %s : expect server to be null when a mandatory callback is "
            "missing"
            "\n",
            name);
    return EXIT_FAILURE;
  }

  p.on_ws_open = onOpen;
  if (ws_server_create(&p) != NULL) {
    fprintf(stderr,
            "[FAIL] %s : expect server to be null when a mandatory callback is "
            "missing"
            "\n",
            name);
    return EXIT_FAILURE;
  }

  p.on_ws_msg = onMsg;
  if (ws_server_create(&p) != NULL) {
    fprintf(stderr,
            "[FAIL] %s : expect server to be null when a mandatory callback is "
            "missing"
            "\n",
            name);
    return EXIT_FAILURE;
  }

  p.on_ws_disconnect = onDisconnect;
  if (ws_server_create(&p) == NULL) {
    fprintf(stderr,
            "[FAIL] %s : expect server to be null when a mandatory callback is "
            "missing"
            "\n",
            name);
    return EXIT_FAILURE;
  } else {
    fprintf(stdout, "[PASS] %s\n", name);
    return EXIT_SUCCESS;
  }
}

int test_ws_server_create_ipv4_ipv6(const char *name) {
  struct ws_server_params p = {
      .port = 9917,
      .addr = "127.0.0.1",
      .on_ws_msg = onMsg,
      .on_ws_open = onOpen,
      .on_ws_disconnect = onDisconnect,
      .max_conns = 1,
      .max_buffered_bytes = 1,
  };

  if (ws_server_create(&p) == NULL) {
    fprintf(stderr,
            "[FAIL] %s : expected server to be created with ipv4 address"
            "\n",
            name);
    return EXIT_FAILURE;
  }

  p.port = 9919;
  p.addr = "::1";

  if (ws_server_create(&p) == NULL) {
    fprintf(stderr,
            "[FAIL] %s : expected server to be created with ipv6 address"
            "\n",
            name);
    return EXIT_FAILURE;
  }

  fprintf(stdout, "[PASS] %s\n", name);

  return EXIT_SUCCESS;
}

int test_ws_create_buffer_sizing(const char *name) {
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
            "[FAIL] %s : expected server to be created"
            "\n",
            name);
    return EXIT_FAILURE;
  }

  if (s->buffer_pool->buf_sz != getpagesize()) {
    fprintf(stderr,
            "[FAIL] %s : expected buffer size to be %d got %zu"
            "\n",
            name, getpagesize(), s->buffer_pool->buf_sz);
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

  result(&ret, test_ws_server_create_bad_args("ws_server_create empty params"));
  result(&ret, test_ws_server_create_no_mandatory_cbs(
                   "ws_server_create no mandatory callbacks"));

  result(&ret, test_ws_server_create_ipv4_ipv6(
                   "ws_server_create binds to ipv4 or ipv6"));

  result(&ret, test_ws_create_buffer_sizing(
                   "test_ws_create_buffer_size to next page size multiple"));

  exit(ret);
}

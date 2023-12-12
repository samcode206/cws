#include "../../src/ws.c"

#include <stdio.h>
#include <stdlib.h>

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

void onOpen(ws_conn_t *conn) {}

void onMsg(ws_conn_t *conn, void *msg, size_t n, bool bin) {}

void onDisconnect(ws_conn_t *conn, int err) {}

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

int main(void) {
  int ret = test_ws_server_create_bad_args("ws_server_create empty params");

  ret = test_ws_server_create_no_mandatory_cbs(
      "ws_server_create no mandatory callbacks");


  exit(ret);
}

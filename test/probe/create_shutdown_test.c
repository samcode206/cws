#include "../../src/ws.c"
#include <stdio.h>

void test_ws_server_create_bad_args(const char *name) {
  struct ws_server_params p = {0};

  if (ws_server_create(&p) != NULL) {
    fprintf(stderr,
            "[FAIL] %s : expect server to be null when no valid arguments "
            "are provided to ws_server_create\n",
            name);
  } else {
    fprintf(stdout, "[PASS] %s\n", name);
  };
}

int main(void) {
  test_ws_server_create_bad_args("ws_server_create empty params");
}

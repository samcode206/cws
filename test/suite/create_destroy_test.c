#include "../../src/ws.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void onOpen(ws_conn_t *conn) {}

void onMsg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {}

void onDisconnect(ws_conn_t *conn, int err) {}

int test_ws_server_create_destroy_different_max_conns(const char *name) {

  for (size_t i = 1; i < 1000; i++) {
    struct ws_server_params p = {
        .port = 9919,
        .addr = "::1",
        .on_ws_msg = onMsg,
        .on_ws_open = onOpen,
        .on_ws_disconnect = onDisconnect,
        .max_conns = i,
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

    ws_conn_t **conns = malloc(sizeof(ws_conn_t *) * i);

    for (size_t j = 0; j < i; j++) {
      ws_conn_t *c = ws_conn_get(s->conn_pool);
      conns[j] = c;
      assert(c != NULL);
    }

    for (size_t j = 0; j < i; j++) {
      ws_conn_t *c = conns[j];
      assert(c != NULL);
      c->recv_buf = mirrored_buf_get(s->buffer_pool);
      c->send_buf = mirrored_buf_get(s->buffer_pool);

      buf_put(c->recv_buf, "hello", 5);
      buf_put(c->send_buf, "World", 5);

      assert(c->recv_buf != NULL);
      assert(c->send_buf != NULL);
    }

    assert(s->conn_pool->avb == 0);
    assert(s->buffer_pool->avb == 0);

    for (size_t j = 0; j < i; j++) {
      ws_conn_t *c = conns[j];
      assert(c != NULL);
      mirrored_buf_put(s->buffer_pool, c->recv_buf);
      mirrored_buf_put(s->buffer_pool, c->send_buf);
      c->recv_buf = NULL;
      c->send_buf = NULL;

      ws_conn_put(s->conn_pool, c);
      conns[j] = NULL;
    }

    assert(s->conn_pool->avb == i);
    assert(s->buffer_pool->avb == i * 2);



    int ret = ws_server_destroy(s);
    assert(ret == 0);
    free(conns);
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

  result(&ret, test_ws_server_create_destroy_different_max_conns(
                   "test_ws_server_create_destroy_different_max_conns"));

  exit(ret);
}

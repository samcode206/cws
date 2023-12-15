#include "ws.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


void on_open(ws_conn_t *c) {
  // ws_conn_set_ctx(c, calloc(1, 8));
}


void on_msg(ws_conn_t *c, void *msg, size_t n, bool bin) {
  if (bin) {
    ws_conn_send(c, msg, n, 0);
  } else {
    if (ws_conn_compression_allowed(c)) {
      ws_conn_send_txt(c, msg, n, true);
    } else {
      ws_conn_send_txt(c, msg, n, 0);
    }
  }

}


void on_close(ws_conn_t *ws_conn, int code, const void *reason) {
  printf("on_close, code: %d \n", code);
  if ((code == WS_CLOSE_ABNORMAL) | (code == WS_CLOSE_NO_STATUS)) {
    ws_conn_close(ws_conn, (void *)reason, 0, WS_CLOSE_NORMAL);
  } else {
    ws_conn_close(ws_conn, (void *)reason, 0, code);
  }
}

void on_disconnect(ws_conn_t *ws_conn, int err) {
  if (ws_conn_ctx(ws_conn)) {
    free(ws_conn_ctx(ws_conn));
    ws_conn_set_ctx(ws_conn, NULL);
  };

}

void on_drain(ws_conn_t *ws_conn) { printf("on_drain\n"); }

void on_server_err(ws_server_t *s, int err) {
  fprintf(stderr, "on_server_err: %s\n", strerror(err));
}

int on_accept(ws_server_t *s, struct sockaddr_storage *caddr, int fd) {
  char client_ip[INET6_ADDRSTRLEN];
  switch (caddr->ss_family) {
  case AF_INET: {
    // It's IPv4
    struct sockaddr_in *s = (struct sockaddr_in *)caddr;
    inet_ntop(AF_INET, &s->sin_addr, client_ip, sizeof(client_ip));
    printf("%s:%d\n", client_ip, ntohs(s->sin_port));
    break;
  }
  case AF_INET6: {
    // It's IPv6
    struct sockaddr_in6 *s = (struct sockaddr_in6 *)caddr;
    inet_ntop(AF_INET6, &s->sin6_addr, client_ip, sizeof(client_ip));
    printf("client connected %s:%d\n", client_ip, ntohs(s->sin6_port));
    break;
  }
  default:
    printf("Unknown address family.\n");
    break;
  }

  return 0;
}

void on_accept_err(ws_server_t *s, int err) {
  printf("accept4(): %s\n", strerror(err));
  printf("open_conns = %zu \n", ws_server_open_conns(s));
  printf("is_paused=%d\n", ws_server_accept_paused(s));
}

void on_timeout(ws_conn_t *c, unsigned timeout_kind) {
  if (timeout_kind == WS_ERR_READ_TIMEOUT) {
    printf("read timeout on %d\n", ws_conn_fd(c));
  } else if (timeout_kind == WS_ERR_WRITE_TIMEOUT) {
    printf("write timeout on  %d\n", ws_conn_fd(c));
  } else if (timeout_kind == WS_ERR_RW_TIMEOUT){
    printf("read/write timeout on %d\n", ws_conn_fd(c));
  }

  ws_conn_destroy(c, WS_ERR_READ_TIMEOUT);
}

void *start_server() {
  const uint16_t port = 9919;
  const int backlog = 1024;
  struct ws_server_params sp = {
      .addr = "::1",
      .port = port,
      .on_ws_open = on_open,
      .on_ws_msg = on_msg,
      .on_ws_disconnect = on_disconnect,
      .max_buffered_bytes = 1024 * 1024 * 32,
      .on_ws_accept_err = on_accept_err,
      .on_ws_conn_timeout = on_timeout,
      .verbose = 1,
  };

  ws_server_t *s = ws_server_create(&sp);

  ws_server_start(s, backlog);

  return NULL;
}

#define NUM_THREADS 1
int main(void) {
  printf("%d\n", getpid());
  start_server();
}

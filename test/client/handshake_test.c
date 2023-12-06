#include "sock_util.h"
#include <stdio.h>
#include <stdlib.h>

void do_handshake_test() {
  int ipv6 = 1;
  int fd = sock_new(ipv6);

  sock_connect(fd, 9919, "::1", ipv6);
  
  ssize_t sent = sock_sendall(fd, EXAMPLE_REQUEST, sizeof EXAMPLE_REQUEST - 1);
  if (sent != sizeof EXAMPLE_REQUEST - 1) {
    fprintf(stderr, "failed to send upgrade request\n");
    exit(EXIT_FAILURE);
  }

  char buf[4096] = {0};

  ssize_t read = sock_recv(fd, buf, 4096);
  if (read == 0) {
    fprintf(stderr, "connection dropped before receiving upgrade response\n");
    exit(EXIT_FAILURE);
  } else if (read == -1) {
    perror("recv");
    exit(EXIT_FAILURE);
  }

  if (strstr(buf, EXAMPLE_REQUEST_EXPECTED_ACCEPT_KEY) != NULL) {
    printf("[SUCCESS] received response of length = %zi\n", read);
  } else {
    fprintf(stderr, "unexpected response\n");
  }

  printf("-------------------------------\n");
  printf("%s\n", buf);
  printf("-------------------------------\n");
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  do_handshake_test();
  return EXIT_SUCCESS;
}
#include "sock_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PING "ping"
#define PONG "pong"
#define TXT "txt"
#define BIN "bin"
#define CLOSE "close"


#define PORT 9919
#define ADDR "::1"

size_t write_frame(char *dst, const char *src, size_t len, unsigned opcode) {
  uint8_t header[8] = {0};
  unsigned hlen;
  if (len > 125) {
    header[0] = 0x80 | opcode;
    header[1] = 126;
    header[2] = (len >> 8) & 0xFF;
    header[3] = len & 0xFF;
    hlen = 4;
  } else {
    header[0] = 0x80 | opcode;
    header[1] = (uint8_t)len;
    hlen = 2;
  }

  uint8_t masking_key[4];
  for (int i = 0; i < 4; ++i) {
    masking_key[i] = rand() % 256;
  }

  // masked frame
  header[1] |= 0x80;

  memcpy(dst, header, hlen);
  memcpy(dst + hlen, masking_key, 4);
  hlen += 4;
  memcpy(dst + hlen, src, len);

  char *payload = dst + hlen;

  for (size_t i = 0; i < len; ++i) {
    payload[i] ^= masking_key[i % 4];
  }

  return hlen + len;
}

size_t write_close_frame(char *dst, unsigned code) {
  uint8_t frame[8];
  frame[0] = 0x80 | OP_CLOSE;
  frame[1] = 0x80 | 2;

  uint8_t masking_key[4];
  for (int i = 0; i < 4; ++i) {
    masking_key[i] = rand() % 256;
  }
  memcpy(frame + 2, masking_key, 4);

  uint8_t status_code[2];
  status_code[0] = (code >> 8) & 0xFF;
  status_code[1] = code & 0xFF;

  for (int i = 0; i < 2; ++i) {
    status_code[i] ^= masking_key[i % 4];
  }

  memcpy(frame + 6, status_code, 2);

  memcpy(dst, frame, 8);

  return 8;
}

void handle_echo_cmd(int fd, char *in_data, char *out_data, unsigned op) {
  size_t len = strlen(in_data);
  size_t frame_sz = write_frame(out_data, in_data, len, op);
  printf("frame_sz = %zu\n", frame_sz);
  ssize_t sent = sock_sendall(fd, out_data, frame_sz);
  printf("%zu\n", sent);

  // wait for frame_sz -4 bytes because server won't include the 4 byte mask
  ssize_t read = sock_recvall(fd, out_data, frame_sz - 4);

  printf("received ");

  unsigned opcode = frame_get_opcode((uint8_t *)out_data);
  if (opcode == OP_TXT) {
    printf("Text Data: ");
  } else if (opcode == OP_BIN) {
    printf("Binary Data: ");
  } else if (opcode == OP_PING) {
    printf("ping: ");
  } else if (opcode == OP_PONG) {
    printf("pong: ");
  } else if (opcode == OP_CLOSE) {
    printf("close\n");
    return;
  }

  if (memcmp(out_data + (frame_sz - len - 4), in_data, len) == 0) {
    printf("%.*s\n", (int)len, out_data + (frame_sz - len - 4));
  } else {
    printf("[Warn]: Data sent doesn't matched received: sent: %s recv: %.*s\n",
           in_data, (int)len, out_data + (frame_sz - len - 4));
  }
}

int main(void) {
  char in_cmd[64];
  char in_data[8192];
  char out_data[12288];


  int fd = sock_new_connect(PORT, ADDR);
  sock_upgrade_ws(fd);


  const char delim[] = " ";

  for (;;) {
    // scanf sucks change this later
    int n = scanf("%s %s", in_cmd, in_data);
    if (n != 2) {
      printf("Expected format: <CMD> <DATA>\n");
    }

    if (!strcasecmp(in_cmd, TXT)) {
      handle_echo_cmd(fd, in_data, out_data, OP_TXT);
    } else if (!strcasecmp(in_cmd, BIN)) {
      handle_echo_cmd(fd, in_data, out_data, OP_BIN);
    } else if (!strcasecmp(in_cmd, PING)) {
      handle_echo_cmd(fd, in_data, out_data, OP_PING);
    } else if (!strcasecmp(in_cmd, PONG)) {
      handle_echo_cmd(fd, in_data, out_data, OP_PONG);
    } else if (!strcasecmp(in_cmd, CLOSE)) {
      int status = atoi(in_data);
      write_close_frame(out_data, status);
      sock_sendall(fd, out_data, 8);

      exit(EXIT_SUCCESS);
    } else {
      printf("unkown command %s\n", in_cmd);
    }
  }

  return 0;
}

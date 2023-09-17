#ifndef WS_PROTOCOL_PARSING23_H
#define WS_PROTOCOL_PARSING23_H

#include "base64.h"
#include <errno.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
// Handshake Utils

#define GET_RQ "GET"
#define SEC_WS_KEY_HDR "Sec-WebSocket-Key"

#define SWITCHING_PROTOCOLS                                                    \
  "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "     \
  "Upgrade\r\nSec-WebSocket-Accept: "
#define SWITCHING_PROTOCOLS_HDRS_LEN                                           \
  sizeof(SWITCHING_PROTOCOLS) - 1 // -1 to ignore the nul

#define WS_VERSION 13

#define ERR_HDR_NOT_FOUND -1
#define ERR_HDR_MALFORMED -2
#define ERR_HDR_TOO_LARGE -3

#define SPACE 0x20
#define CRLF "\r\n"
#define CRLF2 "\r\n\r\n"

static inline int get_header(const char *headers, const char *key, char *val,
                             size_t n) {
  const char *header_start = strstr(headers, key);
  if (header_start) {
    header_start =
        strchr(header_start,
               ':'); // skip colon symbol, if not found header is malformed
    if (header_start == NULL) {
      return ERR_HDR_MALFORMED;
    }

    ++header_start; // skipping colon symbol happens here after validating it's
                    // existence in the buffer
    // skip spaces
    while (*header_start == SPACE) {
      ++header_start;
    }

    const char *header_end =
        strstr(header_start, CRLF); // move to the end of the header value
    if (header_end) {
      // if string is larger than n, return to caller with ERR_HDR_TOO_LARGE
      if ((header_end - header_start) + 1 > n) {
        return ERR_HDR_TOO_LARGE;
      }
      memcpy(val, header_start, (header_end - header_start));
      val[header_end - header_start + 1] =
          '\0'; // nul terminate the header value
      return header_end - header_start +
             1; // we only add one here because of adding '\0'
    } else {
      return ERR_HDR_MALFORMED; // if no CRLF is found the headers is malformed
    }
  }

  return ERR_HDR_NOT_FOUND; // header isn't found
}

static inline ssize_t ws_build_upgrade_headers(const char *accept_key,
                                               size_t keylen,
                                               char *resp_headers) {
  memcpy(resp_headers, SWITCHING_PROTOCOLS, SWITCHING_PROTOCOLS_HDRS_LEN);
  keylen -= 1;
  memcpy(resp_headers + SWITCHING_PROTOCOLS_HDRS_LEN, accept_key, keylen);
  memcpy(resp_headers + SWITCHING_PROTOCOLS_HDRS_LEN + keylen, CRLF2,
         sizeof(CRLF2));
  return SWITCHING_PROTOCOLS_HDRS_LEN + keylen + sizeof(CRLF2);
}

static const char magic_str[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static inline int ws_derive_accept_hdr(const char *akhdr_val, char *derived_val,
                                       size_t len) {
  unsigned char buf[64] = {0};
  memcpy(buf, akhdr_val, strlen(akhdr_val));
  strcat((char *)buf, magic_str);
  len += sizeof magic_str;
  len -= 1;

  unsigned char hash[20] = {0};
  SHA1(buf, len, hash);

  return Base64encode(derived_val, (const char *)hash, sizeof hash);
}

// Frame Utils

#define FIN_MORE 0
#define FIN_DONE 1

#define OP_CONT 0x0
#define OP_TXT 0x1
#define OP_BIN 0x2
#define OP_PING 0x9
#define OP_PONG 0xA
#define OP_CLOSE 0x8

#define PAYLOAD_LEN_16 126
#define PAYLOAD_LEN_64 127

static inline uint8_t frame_get_fin(const unsigned char *buf) {
  return (buf[0] >> 7) & 0x01;
}

static inline uint8_t frame_get_opcode(const unsigned char *buf) {
  return buf[0] & 0x0F;
}

static inline size_t frame_payload_get_len126(const unsigned char *buf) {
  return (buf[2] << 8) | buf[3];
}

static inline size_t frame_payload_get_len127(const unsigned char *buf) {
  return ((uint64_t)buf[2] << 56) | ((uint64_t)buf[3] << 48) |
         ((uint64_t)buf[4] << 40) | ((uint64_t)buf[5] << 32) |
         ((uint64_t)buf[6] << 24) | ((uint64_t)buf[7] << 16) |
         ((uint64_t)buf[8] << 8) | (uint64_t)buf[9];
}

static inline size_t frame_payload_get_len(const unsigned char *buf) {
  return buf[1] & 0X7F;
}

static inline uint32_t frame_is_masked(const unsigned char *buf) {
  return (buf[1] >> 7) & 0x01;
}

static inline size_t frame_get_mask_offset(size_t n) {
  return 2 + ((n > 125) * 2) + ((n > 0xFFFF) * 6);
}

static inline void frame_payload_unmask(const unsigned char *src,
                                        unsigned char *dst, uint8_t *mask,
                                        size_t len) {
  size_t mask_idx = 0;
  for (size_t i = 0; i < len; ++i) {
    dst[i] = src[i] ^ mask[mask_idx];
    mask_idx = (mask_idx + 1) & 3;
  }
}

// server types

typedef struct ws_conn_t ws_conn_t;

typedef struct server ws_server_t;

typedef void (*ws_open_cb_t)(
    ws_conn_t *ws_conn); /* called after a connection is upgraded */

typedef void (*ws_msg_cb_t)(
    ws_conn_t *c, void *msg, uint8_t *mask, size_t n,
    bool bin); /* called when a websocket msg is available */

typedef void (*ws_ping_cb_t)(ws_conn_t *c, void *msg, uint8_t *mask, size_t n,
                             bool bin); /* called when a client sends a PING */

typedef void (*ws_close_cb_t)(
    ws_conn_t *ws_conn, int reason); /* called when a close frame is received */

typedef void (*ws_destroy_cb_t)(
    ws_conn_t *ws_conn); /* called after the connection is closed, use for user
                            data clean up */

typedef void (*ws_drain_cb_t)(
    ws_conn_t *ws_conn); /* called after send buffer is drained (after some back
                            pressure buildup) */

int ws_conn_pong(ws_conn_t *c, void *msg, size_t n, bool bin);
int ws_conn_ping(ws_conn_t *c, void *msg, size_t n, bool bin);
int ws_conn_close(ws_conn_t *c, void *msg, size_t n, int reason);
int ws_conn_destroy(ws_conn_t *c);
int ws_conn_send(ws_conn_t *c, void *msg, size_t n, bool bin);

struct ws_server_params {
  in_addr_t addr;
  uint16_t port;
  size_t max_events; // defaults to 1024
  ws_open_cb_t on_ws_open;
  ws_msg_cb_t on_ws_msg;
  ws_ping_cb_t on_ws_ping;
  ws_drain_cb_t on_ws_drain;
  ws_close_cb_t on_ws_close;
  ws_destroy_cb_t on_ws_destroyed;
};

#define WS_ESYS -1        // system error call should check errno
#define WS_EINVAL_ARGS -2 // invalid argument/arguments provided

#define WS_CREAT_EBAD_PORT -3
#define WS_CREAT_ENO_CB -4

static inline void ws_write_err(int fd, int err) {
  switch (err) {
  case WS_EINVAL_ARGS:
    write(fd, "invalid arguments\n", sizeof("invalid arguments\n"));
    fsync(fd);
    return;
  case WS_CREAT_EBAD_PORT:
    write(fd, "invalid port provided\n", sizeof("invalid port provided\n"));
    fsync(fd);
    return;
  case WS_CREAT_ENO_CB:
    write(fd, "required callback not provided\n",
          sizeof("required callback not provided\n"));
    fsync(fd);
    return;
  case WS_ESYS: {
    char *str = strerror(errno);
    write(fd, str, sizeof str);
    fsync(fd);
    return;
  }
  }

  write(fd, "unknown error\n", sizeof "unknown error\n");
  fsync(fd);
  return;
}

ws_server_t *ws_server_create(struct ws_server_params *params,
                              int *ret); // allocates server resources

int ws_server_start(ws_server_t *s, int backlog); // start serving connections

#endif /* WS_PROTOCOL_PARSING23_H */

#ifndef HANDSHAKE_HTTP_PARSING_H
#define HANDSHAKE_HTTP_PARSING_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "base64.h"
#include <openssl/sha.h>


#define PROTOCOL "HTTP/1.1"
#define GET_RQ "GET"
#define UPGRADE_HDR "Upgrade"
#define CONNECTION_HDR "Connection"
#define HOST_HDR "Host"
#define SEC_WS_KEY_HDR "Sec-WebSocket-Key"
#define SEC_WS_VERSION_HDR "Sec-WebSocket-Version"

#define SWITCHING_PROTOCOLS "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
#define SWITCHING_PROTOCOLS_HDRS_LEN sizeof(SWITCHING_PROTOCOLS) - 1

#define SEC_WS_ACCEPT_HDR "Sec-WebSocket-Accept"

#define WS_VERSION 13

#define ERR_HDR_NOT_FOUND -1
#define ERR_HDR_MALFORMED -2
#define ERR_HDR_TOO_LARGE -3


static int get_header(const char *headers, const char *key, char *val, size_t n) {
  const char *header_start = strstr(headers, key);
  if (header_start) {
    header_start = strchr(header_start, ':');
    if (header_start == NULL) {
      return ERR_HDR_MALFORMED;
    }

    ++header_start;
    while (*header_start == ' ') {
      ++header_start;
    }

    const char *header_end = strstr(header_start, "\r\n");
    if (header_end) {
      if ((header_end - header_start) + 1 > n) {
        return ERR_HDR_TOO_LARGE;
      }
      memcpy(val, header_start, (header_end - header_start));
      val[header_end - header_start + 1] = '\0';
      return header_end - header_start + 1;
    } else {
      return ERR_HDR_MALFORMED;
    }
  }

  return ERR_HDR_NOT_FOUND;
}


static ssize_t ws_build_upgrade_headers(const char *accept_key, size_t keylen,  char * resp_headers){
  memcpy(resp_headers, SWITCHING_PROTOCOLS, SWITCHING_PROTOCOLS_HDRS_LEN);
  keylen-=1;
  memcpy(resp_headers + SWITCHING_PROTOCOLS_HDRS_LEN, accept_key, keylen);
  memcpy(resp_headers + SWITCHING_PROTOCOLS_HDRS_LEN + keylen, "\r\n\r\n", sizeof("\r\n\r\n"));
  return SWITCHING_PROTOCOLS_HDRS_LEN + keylen + sizeof("\r\n\r\n");
}


static const char magic_str[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static inline int ws_derive_accept_hdr(const char *akhdr_val, char *derived_val, size_t len) {
  unsigned char buf[128] = {0};
  memcpy(buf, akhdr_val, strlen(akhdr_val));
  strcat((char *)buf, magic_str);
  len += sizeof magic_str;
  len -= 1;

  unsigned char hash[20] = {0};
  SHA1(buf, len, hash);
  
  return Base64encode(derived_val, (const char *)hash, sizeof hash);
}


#endif /* HANDSHAKE_HTTP_PARSING_H */
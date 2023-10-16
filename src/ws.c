/* The MIT License

   Copyright (c) 2008, 2009, 2011 by Sam H

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

#define _GNU_SOURCE
#include "ws.h"
#include "buf.h"
#include "pool.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/uio.h>

#define BUFFER_SIZE 1024 * 1024 * 4

#define STATE_UPGRADING 0
#define STATE_PARSING_HDR 1
#define STATE_PARSING_MSG 2
#define STATE_PARSING_PING 3
#define STATE_PARSING_PONG 4
#define STATE_PARSING_CLOSE 5

typedef int (*ws_handler)(ws_server_t *s, struct ws_conn_t *conn);

typedef struct {
  bool bin;             // is binary message?
  bool writeable;       // can we write to the socket?
  bool upgraded;        // are we even upgraded?
  size_t fragments_len; // size of the data portion of the frames across
                        // fragmentation
  size_t needed_bytes; // bytes needed before we can do something with the frame
} ws_state_t;

struct ws_conn_t {
  int fd;
  ws_state_t state;
  ws_server_t *base;
  void *ctx; // user data ptr
  buf_t read_buf;
  buf_t write_buf;
};

typedef struct server {
  int fd;            // server file descriptor
  int resv;          // unused
  size_t open_conns; // open websocket connections
  struct buf_pool *buffer_pool;
  ws_open_cb_t on_ws_open;
  ws_msg_cb_t on_ws_msg;
  ws_ping_cb_t on_ws_ping;
  ws_pong_cb_t on_ws_pong;
  ws_drain_cb_t on_ws_drain;
  ws_close_cb_t on_ws_close;
  ws_disconnect_cb_t on_ws_disconnect;
  ws_err_cb_t on_ws_err;
  int epoll_fd;
  buf_t shared_recv_buffer;
  buf_t shared_send_buffer;
  struct epoll_event ev;
  struct epoll_event events[1024];
} ws_server_t;

static inline size_t min(size_t a, size_t b) {
  size_t diff = a - b;
  size_t mask = (diff >> (sizeof(size_t) * 8 - 1));
  return a + diff * mask;
}

// Frame Utils
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

static inline size_t frame_payload_get_raw_len(const unsigned char *buf) {
  return buf[1] & 0X7F;
}

int frame_decode_payload_len(uint8_t *buf, size_t rbuf_len, size_t *res) {
  size_t raw_len = frame_payload_get_raw_len(buf);
  *res = raw_len;
  if (raw_len == PAYLOAD_LEN_16) {
    if (rbuf_len > 3) {
      *res = frame_payload_get_len126(buf);
    } else {
      return 4;
    }
  } else if (raw_len == PAYLOAD_LEN_64) {
    if (rbuf_len > 9) {
      *res = frame_payload_get_len127(buf);
    } else {
      return 10;
    }
  }

  return 0;
}

static inline int frame_has_reserved_bits_set(uint8_t const *buf) {
  return (buf[0] & 0x70) != 0;
}

static inline uint32_t frame_is_masked(const unsigned char *buf) {
  return (buf[1] >> 7) & 0x01;
}

static inline size_t frame_get_mask_offset(size_t const n) {
  return 2 + ((n > 125) * 2) + ((n > 0xFFFF) * 6);
}

static void msg_unmask(uint8_t *src, uint8_t const *mask, size_t const n) {

  size_t i = 0;
  size_t left_over = n & 3;

  for (; i < left_over; ++i) {
    src[i] = src[i] ^ mask[i & 3];
  }

  while (i < n) {
    src[i] = src[i] ^ mask[i & 3];
    src[i + 1] = src[i + 1] ^ mask[(i + 1) & 3];
    src[i + 2] = src[i + 2] ^ mask[(i + 2) & 3];
    src[i + 3] = src[i + 3] ^ mask[(i + 3) & 3];
    i += 4;
  }
}

// HTTP & Handshake Utils
#define WS_VERSION 13

#define SPACE 0x20
#define CRLF "\r\n"
#define CRLF2 "\r\n\r\n"

#define GET_RQ "GET"
#define SEC_WS_KEY_HDR "Sec-WebSocket-Key"

#define SWITCHING_PROTOCOLS                                                    \
  "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "     \
  "Upgrade\r\nServer: cws\r\nSec-WebSocket-Accept: "
#define SWITCHING_PROTOCOLS_HDRS_LEN                                           \
  sizeof(SWITCHING_PROTOCOLS) - 1 // -1 to ignore the nul

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

const char magic_str[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

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

// generic send function (used for upgrade)
static int conn_send(ws_server_t *s, ws_conn_t *conn, const void *data,
                     size_t n);

static void ws_conn_handle(ws_server_t *s, struct ws_conn_t *conn);

static int handle_http(ws_server_t *s, struct ws_conn_t *conn);

static void conn_destroy(ws_server_t *s, struct ws_conn_t *conn, int epfd,
                         int err, struct epoll_event *ev);

static inline int conn_read(ws_server_t *s, struct ws_conn_t *conn, buf_t *buf);

static int conn_drain_write_buf(struct ws_conn_t *conn, buf_t *wbuf);

static int conn_write_frame(ws_server_t *s, ws_conn_t *conn, void *data,
                            size_t len, uint8_t op);

ws_server_t *ws_server_create(struct ws_server_params *params, int *ret) {
  if (ret == NULL) {
    return NULL;
  };

  if (params->port <= 0) {
    *ret = WS_CREAT_EBAD_PORT;
    return NULL;
  }

  if (!params->on_ws_close || !params->on_ws_msg || !params->on_ws_close ||
      !params->on_ws_drain || !params->on_ws_disconnect ||
      !params->on_ws_ping || !params->on_ws_pong || !params->on_ws_err) {
    *ret = WS_CREAT_ENO_CB;
    return NULL;
  }

  ws_server_t *s;

  // allocate memory for server and events for epoll
  s = (ws_server_t *)calloc(1, (sizeof *s));
  if (s == NULL) {
    *ret = WS_ESYS;
    return NULL;
  }

  // socket init
  struct sockaddr_in6 srv_addr;

  s->fd =
      socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  if (s->fd < 0) {
    *ret = WS_ESYS;
    free(s);
    return NULL;
  }

  // socket config
  int on = 1;
  *ret = setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &on,
                    sizeof(int));
  if (*ret < 0) {
    *ret = WS_ESYS;
    free(s);
    return NULL;
  }

  int off = 0;
  *ret = setsockopt(s->fd, SOL_IPV6, IPV6_V6ONLY, &off, sizeof(int));
  if (*ret < 0) {
    *ret = WS_ESYS;
    free(s);
    return NULL;
  }

  // fill in addr info
  memset(&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin6_family = AF_INET6;
  srv_addr.sin6_port = htons(params->port);
  inet_pton(AF_INET6, params->addr, &srv_addr.sin6_addr);

  // bind server socket
  *ret = bind(s->fd, (const struct sockaddr *)&srv_addr, sizeof(srv_addr));
  if (*ret < 0) {
    *ret = WS_ESYS;
    close(s->fd);
    free(s);
    return NULL;
  }

  // init epoll
  s->epoll_fd = epoll_create1(0);
  if (s->epoll_fd < 0) {
    *ret = WS_ESYS;
    close(s->fd);
    free(s);
    return NULL;
  }

  s->on_ws_open = params->on_ws_open;
  s->on_ws_ping = params->on_ws_ping;
  s->on_ws_pong = params->on_ws_pong;
  s->on_ws_msg = params->on_ws_msg;
  s->on_ws_drain = params->on_ws_drain;
  s->on_ws_close = params->on_ws_close;
  s->on_ws_disconnect = params->on_ws_disconnect;
  s->on_ws_err = params->on_ws_err;

  s->buffer_pool = buf_pool_init(1024 * 2, BUFFER_SIZE);

  buf_init(s->buffer_pool, &s->shared_recv_buffer);
  buf_init(s->buffer_pool, &s->shared_send_buffer);

  assert(s->buffer_pool != NULL);
  // server resources all ready
  return s;
}

int ws_server_start(ws_server_t *s, int backlog) {
  int ret = listen(s->fd, backlog);
  if (ret < 0) {
    return ret;
  }
  int fd = s->fd;
  int epfd = s->epoll_fd;

  struct sockaddr_storage client_sockaddr;
  socklen_t client_socklen;
  client_socklen = sizeof client_sockaddr;

  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.data.ptr = s;
  ev.events = EPOLLIN;

  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
    int err = errno;
    s->on_ws_err(s, err);
    return -1;
  };
  int one = 1;

  for (;;) {
    int n_evs = epoll_wait(epfd, s->events, 1024, -1);
    if (n_evs < 0) {
      int err = errno;
      s->on_ws_err(s, err);
      return -1;
    }

    // loop over events
    for (int i = 0; i < n_evs; ++i) {
      if (s->events[i].data.ptr == s) {
        int accepts = 1024; // accept upto 1024 connections per server event
        while (accepts--) {
          int client_fd =
              accept4(fd, (struct sockaddr *)&client_sockaddr, &client_socklen,
                      SOCK_NONBLOCK | SOCK_CLOEXEC);

          if ((client_fd < 0)) {
            if (!(errno == EAGAIN)) {
              int err = errno;
              s->on_ws_err(s, err);
            }
            break;
          }

          assert(setsockopt(client_fd, SOL_TCP, TCP_NODELAY, &one,
                            sizeof(one)) == 0);

          ev.events = EPOLLIN | EPOLLRDHUP;
          struct ws_conn_t *conn = calloc(1, sizeof(struct ws_conn_t));
          assert(conn != NULL);
          conn->fd = client_fd;
          conn->base = s;
          conn->state.writeable = 1;
          conn->state.needed_bytes = 2;
          ev.data.ptr = conn;

          assert(buf_init(s->buffer_pool, &conn->read_buf) == 0);
          assert(buf_init(s->buffer_pool, &conn->write_buf) == 0);
          if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
            int err = errno;
            s->on_ws_err(s, err);
          };
        }

      } else {
        if (s->events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
          conn_destroy(s, s->events[i].data.ptr, epfd, WS_CLOSE_ABNORM, &ev);
          s->events[i].data.ptr = NULL;
        } else if (s->events[i].events & EPOLLOUT) {
          ws_conn_t *c = s->events[i].data.ptr;
          int ret = conn_drain_write_buf(c, &c->write_buf);
          if (ret == 1) {
            ws_conn_t *c = s->events[i].data.ptr;
            s->on_ws_drain(c);
            ev.data.ptr = c;
            ev.events = EPOLLIN | EPOLLRDHUP;
            if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev) == -1) {
              int err = errno;
              s->on_ws_err(s, err);
            };
          }
        } else if (s->events[i].events & EPOLLIN) {
          ws_conn_t *c = s->events[i].data.ptr;
          if (!c->state.upgraded) {
            handle_http(s, c);
          } else {
            ws_conn_handle(s, c);
          }
        }
      }
    }
  }

  return 0;
}

static ssize_t handle_upgrade(const char *buf, char *res_hdrs, size_t n) {
  int ret = get_header(buf, SEC_WS_KEY_HDR, res_hdrs, n);
  if (ret < 0) {
    printf("error parsing http headers: %d\n", ret);
    return -1;
  }

  char accept_key[64];
  int len = ws_derive_accept_hdr(res_hdrs, accept_key, ret - 1);

  return ws_build_upgrade_headers(accept_key, len, res_hdrs);
}

static int conn_read(ws_server_t *s, struct ws_conn_t *conn, buf_t *buf) {
  ssize_t n = buf_recv(buf, conn->fd, 0);
  int epfd = s->epoll_fd;
  if (n == -1) {
    if ((errno == EAGAIN || errno == EWOULDBLOCK)) {
      return 0;
    }
    conn_destroy(s, conn, epfd, errno, &s->ev);
    return -1;
  } else if (n == 0) {
    conn_destroy(s, conn, epfd, WS_CLOSE_ABNORM, &s->ev);
    return -1;
  }

  return 0;
}

static int handle_http(ws_server_t *s, struct ws_conn_t *conn) {
  buf_t *buf;
  if (buf_len(&conn->read_buf)) {
    buf = &conn->read_buf;
  } else {
    buf = &s->shared_recv_buffer;
  }

  if (conn_read(s, conn, buf) == -1) {
    return -1;
  };

  uint8_t *headers = buf_peek(buf);
  size_t rbuf_len = buf_len(buf);
  if (!strncmp((char *)headers, GET_RQ, sizeof GET_RQ - 1)) {
    char res_hdrs[1024] = {0};
    ssize_t ret = handle_upgrade((char *)headers, res_hdrs, sizeof res_hdrs);

    int n = conn_send(s, conn, res_hdrs, ret - 1);
    if (n == ret - 1) {
      s->on_ws_open(conn); // websocket connection is upgraded
    } else {
      // TODO(sah): buffer up the remainder of the response and send more when
      // EPOLLOUT is triggered
      fprintf(
          stderr,
          "[Warn]: unimplemented logic around partial send during upgrade\n");
    }
    // printf("Res --------------------------------\n");
    // printf("%s\n", res_hdrs);
    buf_consume(buf, rbuf_len);
    conn->state.upgraded = 1;
    conn->state.needed_bytes = 2;
    return 0;
  }

  // TODO(sah): handle errors
  return -1;
}

static inline buf_t *ws_conn_choose_read_buf(struct ws_conn_t *conn) {

  if ((buf_len(&conn->read_buf) != 0) &&
      !(conn->state.fragments_len + conn->read_buf.rpos ==
        conn->read_buf.wpos)) {
    // buf_debug(&conn->read_buf, "conn buffer chosen");

    return &conn->read_buf;
  } else {
    return &conn->base->shared_recv_buffer;
  }
}

static size_t ws_conn_readable_len(ws_conn_t *conn, buf_t *buf) {
  if (buf != &conn->read_buf) {
    return buf->wpos - buf->rpos;
  } else {
    return buf->wpos - buf->rpos - conn->state.fragments_len;
  }
}

static inline void ws_conn_handle(ws_server_t *s, struct ws_conn_t *conn) {
  buf_t *buf = ws_conn_choose_read_buf(conn);

  // total frame header bytes trimmed
  size_t total_trimmed = 0;

  if (conn_read(s, conn, buf) == 0) {
    while (ws_conn_readable_len(conn, buf) - total_trimmed >=
           conn->state.needed_bytes) {
      // payload start
      uint8_t *frame_buf = buf_peek_at(
          buf, buf->rpos + ((buf == &conn->read_buf) *
                            (conn->state.fragments_len + total_trimmed)));

      uint8_t fin = frame_get_fin(frame_buf);
      uint8_t opcode = frame_get_opcode(frame_buf);
      // printf("fragments=%zu\n", conn->state.fragments_len);
      // run general validation checks on the header
      if (((fin == 0) & ((opcode > 2) & (opcode != OP_CONT))) |
          (frame_has_reserved_bits_set(frame_buf) == 1) |
          (frame_is_masked(frame_buf) == 0)) {
        printf("invalid frame closing\n");
        printf("opcode=%d\n", opcode);
        printf("fin=%d\n", fin);
        conn_destroy(s, conn, s->epoll_fd, WS_CLOSE_EPROTO, &s->ev);
        return;
      }

      // make sure we can get the full msg
      size_t payload_len = 0;
      size_t frames_buffer_len = buf_len(buf);
      if (&conn->read_buf == buf) {
        frames_buffer_len =
            frames_buffer_len - conn->state.fragments_len - total_trimmed;
      }
      // check if we need to do more reads to get the msg length
      int missing_header_len =
          frame_decode_payload_len(frame_buf, frames_buffer_len, &payload_len);
      if (missing_header_len) {
        // wait for atleast remaining of the header
        conn->state.needed_bytes = missing_header_len;
        goto clean_up_buffer;
      }

      // Todo(sah): add validation to payload_len
      // ....
      // ....

      size_t mask_offset = frame_get_mask_offset(payload_len);
      size_t full_frame_len = payload_len + 4 + mask_offset;

      // check that we have atleast the whole frame, otherwise
      // set needed_bytes and exit waiting for more reads from the socket
      if (frames_buffer_len < full_frame_len) {
        conn->state.needed_bytes = full_frame_len;
        goto clean_up_buffer;
      }

      uint8_t *msg = frame_buf + mask_offset + 4;
      msg_unmask(msg, frame_buf + mask_offset, payload_len);
      // printf("buf_len=%zu frame_len=%zu opcode=%d fin=%d\n",
      // frames_buffer_len,
      //        full_frame_len, opcode, fin);

      switch (opcode) {
      case OP_TXT:
      case OP_BIN:
        conn->state.bin = opcode == OP_BIN;
        // fin and never fragmented
        // this handles both text and binary hence the fallthrough
        if (fin & (conn->state.fragments_len == 0)) {
          if (!conn->state.bin && !utf8_is_valid(msg, payload_len)) {
            printf("failed validation\n");
            conn_destroy(s, conn, s->epoll_fd, WS_CLOSE_EPROTO, &s->ev);
            return; // TODO(sah): send a Close frame, & call close callback
          }
          s->on_ws_msg(conn, msg, payload_len, conn->state.bin);
          buf_consume(buf, full_frame_len);
          conn->state.bin = 0;
          conn->state.needed_bytes = 2;

          break; /* OP_BIN don't fall through to fragmented msg */
        } else if (fin & (conn->state.fragments_len > 0)) {
          // this is invalid because we expect continuation not text or binary
          // opcode
          conn_destroy(s, conn, s->epoll_fd, WS_CLOSE_EPROTO, &s->ev);
          return;
        }

      case OP_CONT:
        // accumulate bytes and increase fragments_len

        // move bytes over
        // call the callback
        // reset

        // can't send cont as first fragment
        if ((opcode == OP_CONT) & (conn->state.fragments_len == 0)) {
          conn_destroy(s, conn, s->epoll_fd, WS_CLOSE_EPROTO, &s->ev);
          return;
        }

        // we are using the shared buffer
        if (buf != &conn->read_buf) {
          // trim off the header
          buf_consume(buf, mask_offset + 4);
          buf_move(buf, &conn->read_buf, payload_len);
          conn->state.fragments_len += payload_len;
          conn->state.needed_bytes = 2;
        } else {
          // place back at the frame_buf start which contains the header & mask
          // we want to get rid of but ensure to subtract by the frame_gap to
          // fill it if it isn't zero
          memmove(frame_buf - total_trimmed, msg, payload_len);
          conn->state.fragments_len += payload_len;
          total_trimmed += mask_offset + 4;
          conn->state.needed_bytes = 2;
        }

        if (fin) {
          if (!conn->state.bin && !utf8_is_valid(buf_peek(&conn->read_buf),
                                                 conn->state.fragments_len)) {
            printf("failed validation\n");
            conn_destroy(s, conn, s->epoll_fd, WS_CLOSE_EPROTO, &s->ev);
            return; // TODO(sah): send a Close frame, & call close callback
          }
          s->on_ws_msg(conn, buf_peek(&conn->read_buf),
                       conn->state.fragments_len, conn->state.bin);
          buf_consume(&conn->read_buf, conn->state.fragments_len);

          conn->state.fragments_len = 0;
          conn->state.needed_bytes = 2;
          conn->state.bin = 0;
        }
        break;
      case OP_PING:
        if (payload_len > 125) {
          conn_destroy(s, conn, s->epoll_fd, WS_CLOSE_EPROTO, &s->ev);
          return;
        }
        s->on_ws_ping(conn, msg, payload_len);
        if ((conn->state.fragments_len != 0) & (buf == &conn->read_buf)) {
          total_trimmed += full_frame_len;
          conn->state.needed_bytes = 2;
        } else {
          // printf("here\n");
          buf_consume(buf, full_frame_len);
          conn->state.needed_bytes = 2;
        }

        break;
      case OP_PONG:
        if (payload_len > 125) {
          conn_destroy(s, conn, s->epoll_fd, WS_CLOSE_EPROTO, &s->ev);
          return;
        }
        s->on_ws_pong(conn, msg, payload_len);
        if ((conn->state.fragments_len != 0) & (buf == &conn->read_buf)) {
          total_trimmed += total_trimmed;
          conn->state.needed_bytes = 2;
        } else {
          buf_consume(buf, full_frame_len);
          conn->state.needed_bytes = 2;
        }
        break;
      case OP_CLOSE:
        if (!payload_len) {
          s->on_ws_close(conn, WS_CLOSE_NORMAL, NULL);
          return;
        } else if (payload_len < 2) {
          s->on_ws_close(conn, WS_CLOSE_EPROTO, NULL);
          return;
        }

        else {
          uint16_t code = WS_CLOSE_NOSTAT;
          if (payload_len > 125) {
            // close frames can be more but this is the most that will be
            // supported for various reasons close frames generally should
            // contain just the 16bit code and a short string for the reason at
            // most
            conn_destroy(s, conn, s->epoll_fd, WS_CLOSE_EPROTO, &s->ev);
            return;
          }

          if (!utf8_is_valid(msg + 2, payload_len - 2)) {
            conn_destroy(s, conn, s->epoll_fd, WS_CLOSE_EPROTO, &s->ev);
            return;
          };

          code = (msg[0] << 8) | msg[1];
          if (code < 1000 || code == 1004 || code == 1100 || code == 1005 ||
              code == 1006 || code == 1015 || code == 1016 || code == 2000 ||
              code == 2999) {
            s->on_ws_close(conn, WS_CLOSE_EPROTO, NULL);
            return;
          }

          s->on_ws_close(conn, code, msg + 2);

          return;
        }

        break;
      default:
        conn_destroy(s, conn, s->epoll_fd, WS_CLOSE_EPROTO, &s->ev);
        return;
      }
    } /* loop end */

    if (buf_len(&s->shared_send_buffer)) {
      conn_drain_write_buf(conn, &s->shared_send_buffer);
    } else {
      conn_drain_write_buf(conn, &conn->write_buf);
    }

  clean_up_buffer:
    if ((buf == &s->shared_recv_buffer) && (buf_len(buf) > 0)) {
      // move to connection specific buffer
      // printf("moving from shared to socket buffer: %zu\n", buf_len(buf));
      buf_move(buf, &conn->read_buf, buf_len(buf));
    } else {

      memmove(buf->buf + buf->rpos + conn->state.fragments_len,
              buf->buf + buf->rpos + conn->state.fragments_len + total_trimmed,
              buf->wpos - buf->rpos + conn->state.fragments_len +
                  total_trimmed);
      buf->wpos =
          buf->rpos + conn->state.fragments_len +
          (buf->wpos - buf->rpos - conn->state.fragments_len - total_trimmed);
    }
  }
}

static void conn_destroy(ws_server_t *s, struct ws_conn_t *conn, int epfd,
                         int err, struct epoll_event *ev) {
  int ret;
  ev->data.ptr = conn;
  ret = epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, ev);
  if (ret == -1) {
    int err = errno;
    s->on_ws_err(s, err);
  }
  ret = close(conn->fd);
  if (ret == -1) {
    int err = errno;
    s->on_ws_err(s, err);
  } else {
    s->on_ws_disconnect(conn,
                        err); // call the user's callback to allow clean up
                              // on data associated with this connection
  }

  buf_pool_free(s->buffer_pool, conn->write_buf.buf);
  buf_pool_free(s->buffer_pool, conn->read_buf.buf);
  conn->write_buf.wpos = 0;
  conn->write_buf.rpos = 0;
  s->shared_recv_buffer.rpos = 0;
  s->shared_recv_buffer.wpos = 0;
  s->shared_send_buffer.rpos = 0;
  s->shared_send_buffer.wpos = 0;
  free(conn);
}

static int conn_drain_write_buf(struct ws_conn_t *conn, buf_t *wbuf) {
  size_t to_write = buf_len(wbuf);
  ssize_t n = 0;

  if (!to_write) {
    return 0;
  }

  if (to_write < 65535) {
    n = buf_send(wbuf, conn->fd, 0);
  } else {
    n = buf_sendfile(conn->base->buffer_pool, wbuf, conn->fd);
  }

  if ((n == -1 && errno != EAGAIN) | (n == 0)) {
    ws_server_t *s = ws_conn_server(conn);
    conn_destroy(ws_conn_server(conn), conn, s->epoll_fd, WS_CLOSE_ABNORM,
                 &s->ev);
    return -1;
  }

  if (to_write == n) {
    conn->state.writeable = 1;
    return 1;
  } else {
    if (&conn->base->shared_send_buffer == wbuf) {
      // worst case
      buf_move(&conn->base->shared_send_buffer, &conn->write_buf,
               buf_len(&conn->base->shared_send_buffer));
    }
    conn->state.writeable = 0;
    conn->base->ev.data.ptr = conn;
    conn->base->ev.events = EPOLLOUT | EPOLLRDHUP;
    n = epoll_ctl(conn->base->epoll_fd, EPOLL_CTL_MOD, conn->fd,
                  &conn->base->ev);
    if (n == -1) {
      int err = errno;
      conn->base->on_ws_err(conn->base, err);
      // might wanna close the socket?
    }
  }

  return 0;
}

/**
* returns:
     1 data was completely written

     0 part of the data was written caller should wait for on_drain event to
start sending more data or an error occurred in which the corresponding callback
will be called
*/
inline int ws_conn_pong(ws_server_t *s, ws_conn_t *c, void *msg, size_t n) {
  int stat = conn_write_frame(s, c, msg, n, OP_PONG);
  if (stat == -1) {
    conn_destroy(s, c, s->epoll_fd, errno, &s->ev);
  }
  return stat == 1;
}
/**
* returns:
     1 data was completely written

     0 part of the data was written caller should wait for on_drain event to
start sending more data or an error occurred in which the corresponding callback
will be called
*/
inline int ws_conn_ping(ws_server_t *s, ws_conn_t *c, void *msg, size_t n) {
  int stat = conn_write_frame(s, c, msg, n, OP_PING);
  if (stat == -1) {
    conn_destroy(s, c, s->epoll_fd, errno, &s->ev);
  }
  return stat == 1;
}

/**
* returns:
     1 data was completely written

     0 part of the data was written caller should wait for on_drain event to
start sending more data or an error occurred in which the corresponding callback
will be called
*/
inline int ws_conn_send(ws_server_t *s, ws_conn_t *c, void *msg, size_t n) {
  int stat = conn_write_frame(s, c, msg, n, OP_BIN);
  if (stat == -1) {
    conn_destroy(s, c, s->epoll_fd, errno, &s->ev);
  }
  return stat == 1;
}

/**
* returns:
     1 data was completely written

     0 part of the data was written caller should wait for on_drain event to
start sending more data or an error occurred in which the corresponding callback
will be called
*/
inline int ws_conn_send_txt(ws_server_t *s, ws_conn_t *c, void *msg, size_t n) {
  int stat = conn_write_frame(s, c, msg, n, OP_TXT);
  if (stat == -1) {
    conn_destroy(s, c, s->epoll_fd, errno, &s->ev);
  }
  return stat == 1;
}

void ws_conn_close(ws_server_t *s, ws_conn_t *conn, void *msg, size_t len,
                   uint16_t code) {
  // reason string must be less than 124
  // this isn't a websocket protocol restriction but it will be here for now
  if (len > 124) {
    return;
  }

  if (conn->state.writeable) {
    // reason msg was provided
    struct iovec iovs[2];
    if (len) {
      uint8_t buf1[4] = {0, 0};
      buf1[0] = FIN | OP_CLOSE;
      buf1[1] = len + 2;
      buf1[2] = (code >> 8) & 0xFF;
      buf1[3] = code & 0xFF;
      iovs[0].iov_len = 4;
      iovs[0].iov_base = buf1;
      iovs[1].iov_len = len;
      iovs[1].iov_base = msg;

      writev(conn->fd, iovs, 2);
    } else {
      uint8_t buf[4] = {0, 0};
      buf[0] = FIN | OP_CLOSE;
      buf[1] = 2;
      buf[2] = (code >> 8) & 0xFF;
      buf[3] = code & 0xFF;
      send(conn->fd, buf, 4, 0);
    }
  }

  int ret;
  ret = epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, conn->fd, &s->ev);
  if (ret == -1) {
    int err = errno;
    s->on_ws_err(s, err);
  }
  ret = close(conn->fd);
  if (ret == -1) {
    int err = errno;
    s->on_ws_err(s, err);
  }

  if (!ret) {
    s->on_ws_disconnect(conn, WS_CLOSED);
  }

  free(conn);

  s->shared_recv_buffer.rpos = 0;
  s->shared_recv_buffer.wpos = 0;
  s->shared_send_buffer.rpos = 0;
  s->shared_send_buffer.wpos = 0;
}

inline int ws_conn_destroy(ws_server_t *s, ws_conn_t *c) {
  return -1; // TODO
}

/**
* returns:
     1 data was completely written

     0 part of the data was written, caller should wait for on_drain event to
start sending more data

    -1 an error occurred connection should be closed, check errno
*/
static int conn_write_frame(ws_server_t *s, ws_conn_t *conn, void *data,
                            size_t len, uint8_t op) {

  buf_t *wbuf;

  if (buf_len(&conn->write_buf)) {
    wbuf = &conn->write_buf;
  } else {
    wbuf = &s->shared_send_buffer;
  }

  ssize_t n = 0;
  size_t hlen = frame_get_mask_offset(len);
  size_t flen = len + hlen;
  if (buf_space(wbuf) > flen) {
    uint8_t *hbuf = wbuf->buf + wbuf->wpos;
    memset(hbuf, 0, 2);
    hbuf[0] = FIN | op; // Set FIN bit and opcode
    switch (hlen) {
    case 2:
      hbuf[1] = (uint8_t)len;
      wbuf->wpos += hlen;
      break;
    case 4:
      hbuf[1] = PAYLOAD_LEN_16;
      hbuf[2] = (len >> 8) & 0xFF;
      hbuf[3] = len & 0xFF;
      wbuf->wpos += hlen;
      break;
    case 10:
      hbuf[1] = PAYLOAD_LEN_64;
      hbuf[2] = (len >> 56) & 0xFF;
      hbuf[3] = (len >> 48) & 0xFF;
      hbuf[4] = (len >> 40) & 0xFF;
      hbuf[5] = (len >> 32) & 0xFF;
      hbuf[6] = (len >> 24) & 0xFF;
      hbuf[7] = (len >> 16) & 0xFF;
      hbuf[8] = (len >> 8) & 0xFF;
      hbuf[9] = len & 0xFF;
      wbuf->wpos += hlen;
      // if (!buf_len(wbuf)) {
      //   struct iovec vecs[2] = {{
      //                               .iov_base = hbuf,
      //                               .iov_len = hlen,
      //                           },
      //                           {
      //                               .iov_base = data,
      //                               .iov_len = len,
      //                           }};

      //   ssize_t n = writev(conn->fd, vecs, 2);
      //   if (n == 0 || n == -1) {
      //     if (n == -1 && errno == EAGAIN) {
      //       wbuf->wpos += hlen;
      //       buf_put(wbuf, data, len);
      //       return 0;
      //     }
      //     conn_destroy(conn->base, conn, conn->base->epoll_fd,
      //     WS_CLOSE_ABNORM, &conn->base->ev); return -1;
      //   }
      //   if (n == flen) {
      //     return 1;
      //   } else if (n > hlen) {
      //     buf_put(wbuf, (uint8_t *)vecs[1].iov_base + n - hlen,
      //             flen - n - hlen);
      //     return 0;
      //   } else {
      //     buf_put(wbuf, hbuf + n, hlen - n);
      //     buf_put(wbuf, data, len);
      //     return 0;
      //   }
      // } else {
      //   wbuf->wpos += hlen;
      // }
      break;
    }

    return buf_put(wbuf, data, len) == 0;
  }

  // ssize_t n = 0;
  // size_t hlen = frame_get_mask_offset(len);
  // size_t flen = len + hlen;

  // if ((conn->writeable == 1) & (buf_space(&conn->write_buf) > flen)) {
  //   uint8_t hbuf[hlen];
  //   memset(hbuf, 0, hlen);
  //   struct iovec iovs[2];
  //   hbuf[0] = FIN | op; // Set FIN bit and opcode
  //   if (hlen == 2) {
  //     hbuf[1] = (uint8_t)len;
  //   } else if (hlen == 4) {
  //     hbuf[1] = PAYLOAD_LEN_16;
  //     hbuf[2] = (len >> 8) & 0xFF;
  //     hbuf[3] = len & 0xFF;
  //   } else {
  //     hbuf[1] = PAYLOAD_LEN_64;
  //     hbuf[2] = (len >> 56) & 0xFF;
  //     hbuf[3] = (len >> 48) & 0xFF;
  //     hbuf[4] = (len >> 40) & 0xFF;
  //     hbuf[5] = (len >> 32) & 0xFF;
  //     hbuf[6] = (len >> 24) & 0xFF;
  //     hbuf[7] = (len >> 16) & 0xFF;
  //     hbuf[8] = (len >> 8) & 0xFF;
  //     hbuf[9] = len & 0xFF;
  //   }

  //   iovs[0].iov_len = hlen;
  //   iovs[0].iov_base = hbuf;
  //   iovs[1].iov_len = len;
  //   iovs[1].iov_base = data;

  //   n = writev(conn->fd, iovs, 2);
  //   if (n == 0) {
  //     return -1;
  //   } else if (n == -1) {
  //     if (!((errno == EAGAIN) | (errno == EWOULDBLOCK))) {
  //       return -1;
  //     }
  //     n = 0;
  //   }

  //   if (n < flen) {
  //     conn->writeable = 0;
  //     if (n > hlen) {
  //       assert(buf_put(&conn->write_buf, (uint8_t *)data + (n - hlen),
  //                      flen - n) == 0);
  //     } else {
  //       assert(buf_put(&conn->write_buf, hbuf + n, hlen - n) == 0);
  //       assert(buf_put(&conn->write_buf, data, len) == 0);
  //     }

  //     s->ev.events = EPOLLOUT | EPOLLRDHUP;
  //     s->ev.data.ptr = conn;
  //     if (epoll_ctl(s->epoll_fd, EPOLL_CTL_MOD, conn->fd,
  //                   &s->ev) == -1) {
  //       return -1;
  //     };
  //     return 0;
  //   }
  // }

  return n == flen;
}

static int conn_send(ws_server_t *s, ws_conn_t *conn, const void *data,
                     size_t len) {
  ssize_t n = 0;

  if ((conn->state.writeable == 1) & (buf_space(&conn->write_buf) > len)) {
    n = send(conn->fd, data, len, 0);
    if (n == 0) {
      return -1;
    } else if (n == -1) {
      if (!((errno == EAGAIN) | (errno == EWOULDBLOCK))) {
        return -1;
      }
      n = 0;
    }

    if (n < len) {
      s->ev.events = EPOLLOUT | EPOLLRDHUP;
      s->ev.data.ptr = conn;
      conn->state.writeable = 0;
      assert(buf_put(&conn->write_buf, (uint8_t *)data + n, len - n) == 0);
      if (epoll_ctl(s->epoll_fd, EPOLL_CTL_MOD, conn->fd, &s->ev) == -1) {
        return -1;
      };

      return 0;
    }
  }

  return n;
}

int ws_conn_fd(ws_conn_t *c) { return c->fd; }

inline ws_server_t *ws_conn_server(ws_conn_t *c) { return c->base; }

inline void *ws_conn_ctx(ws_conn_t *c) { return c->ctx; }

inline void ws_conn_ctx_attach(ws_conn_t *c, void *ctx) { c->ctx = ctx; }

int utf8_is_valid(uint8_t *s, size_t n) {
  for (uint8_t *e = s + n; s != e;) {
    if (s + 4 <= e) {
      uint32_t tmp;
      memcpy(&tmp, s, 4);
      if ((tmp & 0x80808080) == 0) {
        s += 4;
        continue;
      }
    }

    while (!(*s & 0x80)) {
      if (++s == e) {
        return 1;
      }
    }

    if ((s[0] & 0x60) == 0x40) {
      if (s + 1 >= e || (s[1] & 0xc0) != 0x80 || (s[0] & 0xfe) == 0xc0) {
        return 0;
      }
      s += 2;
    } else if ((s[0] & 0xf0) == 0xe0) {
      if (s + 2 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 ||
          (s[0] == 0xe0 && (s[1] & 0xe0) == 0x80) ||
          (s[0] == 0xed && (s[1] & 0xe0) == 0xa0)) {
        return 0;
      }
      s += 3;
    } else if ((s[0] & 0xf8) == 0xf0) {
      if (s + 3 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 ||
          (s[3] & 0xc0) != 0x80 || (s[0] == 0xf0 && (s[1] & 0xf0) == 0x80) ||
          (s[0] == 0xf4 && s[1] > 0x8f) || s[0] > 0xf4) {
        return 0;
      }
      s += 4;
    } else {
      return 0;
    }
  }
  return 1;
}

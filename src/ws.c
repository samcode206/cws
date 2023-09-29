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
#include "buf.h"
#include "ws.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include "pool.h"

struct ws_conn_t {
  int fd;
  bool writeable;
  bool upgraded;
  bool fmsg;
  bool fmsg_bin;
  size_t fmsg_end;
  size_t
      rlo_watermark; // minimum amount of bytes before beginning frame decoding
  ws_server_t *base;
  void *ctx; // user data ptr
  buf_t read_buf;
  buf_t write_buf;
};

typedef struct io_ctl {
  int epoll_fd;
  size_t ev_cap;
  struct epoll_event ev;
  struct epoll_event events[];
} io_ctl_t;

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
  io_ctl_t io_ctl; // io controller
} ws_server_t;

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

static inline int frame_has_reserved_bits_set(uint8_t *buf) {
  return (buf[0] & 0x70) != 0;
}

static inline uint32_t frame_is_masked(const unsigned char *buf) {
  return (buf[1] >> 7) & 0x01;
}

static inline size_t frame_get_mask_offset(size_t n) {
  return 2 + ((n > 125) * 2) + ((n > 0xFFFF) * 6);
}

// inline void msg_unmask(uint8_t *src, uint8_t *dst, size_t len) {
//   uint8_t *mask = (uint8_t *)src - 4;

//   for (size_t i = 0; i < len; ++i) {
//     dst[i] = src[i] ^ mask[i & 3];
//   }
// }

void msg_unmask(uint8_t *src, size_t n) {
  uint8_t *mask = (uint8_t *)(src - 4);
  size_t i = 0;
  size_t left_over = n & 3;

  for (; i < left_over; ++i) {
    src[i] = src[i] ^ mask[i & 3];
  }

  for (; i < n; i += 4) {
    src[i] = src[i] ^ mask[i & 3];
    src[i + 1] = src[i + 1] ^ mask[(i + 1) & 3];
    src[i + 2] = src[i + 2] ^ mask[(i + 2) & 3];
    src[i + 3] = src[i + 3] ^ mask[(i + 3) & 3];
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
int conn_send(ws_server_t *s, ws_conn_t *conn, const void *data, size_t n);

int handle_ws(ws_server_t *s, struct ws_conn_t *conn);

void handle_ws_fmsg(ws_server_t *s, struct ws_conn_t *conn);

int handle_http(ws_server_t *s, struct ws_conn_t *conn);

void conn_destroy(ws_server_t *s, struct ws_conn_t *conn, int epfd, int err,
                  struct epoll_event *ev);

static inline int conn_read(ws_server_t *s, struct ws_conn_t *conn);

int conn_drain_write_buf(struct ws_conn_t *conn);

int conn_write_frame(ws_server_t *s, ws_conn_t *conn, void *data, size_t len,
                     uint8_t op);

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

  if (params->max_events <= 0) {
    params->max_events = 1024;
  }

  // allocate memory for server and events for epoll
  s = (ws_server_t *)malloc((sizeof *s) +
                            (sizeof(struct epoll_event) * params->max_events));
  if (s == NULL) {
    *ret = WS_ESYS;
    return NULL;
  }

  if (memset(s, 0, (sizeof(struct epoll_event) * params->max_events)) == NULL) {
    *ret = WS_ESYS;
    free(s);
    return NULL;
  };

  s->io_ctl.ev_cap = params->max_events;

  // socket init
  struct sockaddr_in srv_addr;

  s->fd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (s->fd < 0) {
    *ret = WS_ESYS;
    free(s);
    return NULL;
  }

  // socket config
  int on = 1;
  *ret = setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int));
  if (*ret < 0) {
    *ret = WS_ESYS;
    free(s);
    return NULL;
  }

  // fill in addr info
  memset(&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_port = htons(params->port);
  srv_addr.sin_addr.s_addr = htons(params->addr);

  // bind server socket
  *ret = bind(s->fd, (const struct sockaddr *)&srv_addr, sizeof(srv_addr));
  if (*ret < 0) {
    *ret = WS_ESYS;
    close(s->fd);
    free(s);
    return NULL;
  }

  // init epoll
  s->io_ctl.epoll_fd = epoll_create1(0);
  if (s->io_ctl.epoll_fd < 0) {
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

  s->buffer_pool = buf_pool_init(params->max_events, 1024 * 8);
  
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
  int epfd = s->io_ctl.epoll_fd;
  size_t max_events = s->io_ctl.ev_cap;
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

  for (;;) {
    int n_evs = epoll_wait(epfd, s->io_ctl.events, max_events, -1);
    if (n_evs < 0) {
      int err = errno;
      s->on_ws_err(s, err);
      return -1;
    }

    // loop over events
    for (int i = 0; i < n_evs; ++i) {
      if (s->io_ctl.events[i].data.ptr == s) {
        for (;;) {
          int client_fd = accept4(fd, (struct sockaddr *)&client_sockaddr,
                                  &client_socklen, O_NONBLOCK);

          if ((client_fd < 0)) {
            if (!(errno == EAGAIN)) {
              int err = errno;
              s->on_ws_err(s, err);
            }
            break;
          }

          ev.events = EPOLLIN | EPOLLRDHUP;
          struct ws_conn_t *conn = calloc(1, sizeof(struct ws_conn_t));
          assert(conn != NULL);
          conn->fd = client_fd;
          conn->base = s;
          conn->writeable = 1;
          conn->rlo_watermark =
              2; // start at 2 bytes, works even before upgrade
          ev.data.ptr = conn;

          assert(buf_init(s->buffer_pool,&conn->read_buf) == 0);
          assert(buf_init(s->buffer_pool, &conn->write_buf) == 0);

          if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
            int err = errno;
            s->on_ws_err(s, err);
          };
        }

      } else {
        if (s->io_ctl.events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
          conn_destroy(s, s->io_ctl.events[i].data.ptr, epfd, WS_CLOSE_ABNORM,
                       &ev);
          s->io_ctl.events[i].data.ptr = NULL;
        } else {
          if (s->io_ctl.events[i].events & EPOLLOUT) {
            int ret = conn_drain_write_buf(s->io_ctl.events[i].data.ptr);
            if (ret == 1) {
              ws_conn_t *c = s->io_ctl.events[i].data.ptr;
              s->on_ws_drain(c);
              ev.data.ptr = c;
              ev.events = EPOLLIN | EPOLLRDHUP;
              if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev) == -1) {
                int err = errno;
                s->on_ws_err(s, err);
              };
            }
          } else if (s->io_ctl.events[i].events & EPOLLIN) {
            ws_conn_t *c = s->io_ctl.events[i].data.ptr;
            int ret = conn_read(s, c);
            if (ret == 0) {
              if (c->upgraded == 0) {
                handle_http(s, c);
              } else {
                if (c->fmsg) {
                  if (c->read_buf.wpos - c->fmsg_end >= c->rlo_watermark)
                    handle_ws_fmsg(s, c);
                } else {
                  while ((buf_len(&c->read_buf) >= c->rlo_watermark)) {
                    ret = handle_ws(s, c);
                    if (ret == -1)
                      break;
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  return 0;
}

ssize_t handle_upgrade(const char *buf, char *res_hdrs, size_t n) {
  int ret = get_header(buf, SEC_WS_KEY_HDR, res_hdrs, n);
  if (ret < 0) {
    printf("error parsing http headers: %d\n", ret);
    return -1;
  }

  char accept_key[64];
  int len = ws_derive_accept_hdr(res_hdrs, accept_key, ret - 1);

  return ws_build_upgrade_headers(accept_key, len, res_hdrs);
}

static inline int conn_read(ws_server_t *s, struct ws_conn_t *conn) {
  ssize_t n = buf_recv(&conn->read_buf, conn->fd, 0);
  int epfd = s->io_ctl.epoll_fd;

  if (n == -1) {
    if ((errno == EAGAIN || errno == EWOULDBLOCK)) {
      return 0;
    }
    conn_destroy(s, conn, epfd, errno, &s->io_ctl.ev);
    return -1;
  } else if (n == 0) {
    conn_destroy(s, conn, epfd, WS_CLOSE_ABNORM, &s->io_ctl.ev);
    return -1;
  }

  return 0;
}

int handle_http(ws_server_t *s, struct ws_conn_t *conn) {
  uint8_t *buf = buf_peek(&conn->read_buf);
  size_t rbuf_len = buf_len(&conn->read_buf);
  if (!strncmp((char *)buf, GET_RQ, sizeof GET_RQ - 1)) {
    char res_hdrs[1024] = {0};
    ssize_t ret = handle_upgrade((char *)buf, res_hdrs, sizeof res_hdrs);

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
    buf_consume(&conn->read_buf, rbuf_len);
    conn->upgraded = 1;
    return 0;
  }

  // TODO(sah): handle errors
  return -1;
}

void handle_ws_fmsg(ws_server_t *s, struct ws_conn_t *conn) {
  int epfd = s->io_ctl.epoll_fd;

  uint8_t *buf;
  size_t rbuf_len = rbuf_len = conn->read_buf.wpos - conn->fmsg_end;

  int masked;
  uint8_t fin;
  uint8_t opcode;

  size_t msg_len;     // user data portion size
  int missing_len;    // length missing to be able to calculate msg_len
  size_t mask_offset; // mask offset (depends on msg length)
  size_t flen;        // full frame length

  while (1) {
    if (rbuf_len < 2) {
      conn->rlo_watermark = 2;
      return;
    }

    buf = buf_peek_at(&conn->read_buf, conn->fmsg_end);

    assert(rbuf_len < conn->read_buf.buf_sz); // no wrap around test
    masked = frame_is_masked(buf);
    // if mask bit isn't set close the connection
    if ((masked == 0) | (frame_has_reserved_bits_set(buf) == 1)) {
      printf("unmasked or reserved bits set\n");
      conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
      return;
    }
    fin = frame_get_fin(buf);
    opcode = frame_get_opcode(buf);

    // decode msg length, if we can't due to a partial header, stop
    // and wait for it by adjusting the read watermark
    missing_len = frame_decode_payload_len(buf, rbuf_len, &msg_len);
    if (missing_len) {
      conn->rlo_watermark = missing_len;
      return;
    }

    mask_offset = frame_get_mask_offset(msg_len);
    flen = msg_len + mask_offset + 4;

    printf("wpos=%zu rpos=%zu fmsg_read_idx=%zu buf_size=%zu\n",
           conn->read_buf.wpos, conn->read_buf.rpos, conn->fmsg_end, rbuf_len);

    // check to see if the full frame is available
    // it not stop, adjust and adjust the read watermark
    if (rbuf_len < flen) {
      conn->rlo_watermark = flen;
      return;
    }

    if ((opcode == OP_TXT) | (opcode == OP_BIN) | (opcode == OP_CONT)) {
      conn->rlo_watermark = 2;
      msg_unmask(buf + mask_offset + 4, msg_len);
      printf("%.*s\n", (int)msg_len, buf + mask_offset + 4);
      memmove(buf, buf + mask_offset + 4, rbuf_len - mask_offset - 4);
      rbuf_len = conn->read_buf.wpos - flen - conn->fmsg_end;

      conn->fmsg_end += msg_len;
      conn->read_buf.wpos = conn->read_buf.wpos - mask_offset - 4;
      buf = buf_peek_at(&conn->read_buf, conn->fmsg_end);
      printf("flen = %zu\n", flen);
      printf("read fragmented msg: wpos=%zu rpos=%zu fmsg_read_idx=%zu "
             "buf_size=%zu\n",
             conn->read_buf.wpos, conn->read_buf.rpos, conn->fmsg_end,
             rbuf_len);
      printf("opcode= %d fin=%d\n", opcode, fin);
      printf("-----------------------------------------\n");
      if (fin) {
        if (opcode == OP_CONT) {
          conn->fmsg = 0;

          if (!conn->fmsg_bin) {
            if (!utf8_is_valid(buf_peek(&conn->read_buf),
                               conn->fmsg_end - conn->read_buf.rpos)) {
              conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
              return;
            };
          }

          s->on_ws_msg(conn, buf_peek(&conn->read_buf),
                       conn->fmsg_end - conn->read_buf.rpos, conn->fmsg_bin);
          buf_consume(&conn->read_buf, conn->fmsg_end - conn->read_buf.rpos);

          while (buf_len(&conn->read_buf) >= conn->rlo_watermark) {
            int ret = handle_ws(s, conn);
            if (ret == -1)
              break;
          }

          return;
        } else {
          conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
          return;
        }
      }

    } else if ((opcode == OP_PING) | (opcode == OP_PONG)) {
      if (!fin) {
        conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
        return;
      } else {
        // handle interleaved control frame
        if (msg_len > 125) {
          // PINGs must be 125 or less
          conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
          return; // TODO(sah): send a Close frame, & call close callback
        }
        conn->rlo_watermark = 2;
        msg_unmask(buf + mask_offset + 4, msg_len);
        if (opcode == OP_PING) {
          s->on_ws_ping(conn, buf + mask_offset + 4, msg_len);
        } else {
          s->on_ws_pong(conn, buf + mask_offset + 4, msg_len);
        }
        memmove(conn->read_buf.buf + conn->fmsg_end, buf + flen,
                rbuf_len - flen);
        rbuf_len = conn->read_buf.wpos - conn->fmsg_end - flen;
        printf("wpos before: %zu\n", conn->read_buf.wpos);
        conn->read_buf.wpos = conn->read_buf.wpos - flen;

        printf("flen = %zu\n", flen);
        printf("read ctl msg: wpos=%zu rpos=%zu fmsg_read_idx=%zu "
               "buf_size=%zu\n",
               conn->read_buf.wpos, conn->read_buf.rpos, conn->fmsg_end,
               rbuf_len);
        printf("-----------------------------------------\n");
      }
    } else if (opcode == OP_CLOSE) {
      // handle close stuff
      conn->rlo_watermark = 2;
      uint16_t code =
          WS_CLOSE_NOSTAT; // pessimistically assume no code provided
      if (!msg_len) {
        s->on_ws_close(conn, WS_CLOSE_NORMAL, NULL);
        return;
      } else if (msg_len < 2) {
        s->on_ws_close(conn, WS_CLOSE_EPROTO, NULL);
        return;
      }

      else {
        if (msg_len > 125) {
          // close frames can be more but this is the most that will be
          // supported for various reasons close frames generally should
          // contain just the 16bit code and a short string for the reason at
          // most
          conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
          return;
        }

        msg_unmask(buf + mask_offset + 4, msg_len);
        code = (buf[6] << 8) | buf[7];
        if (code < 1000 || code == 1004 || code == 1100 || code == 1005 ||
            code == 1006 || code == 1015 || code == 1016 || code == 2000 ||
            code == 2999) {
          s->on_ws_close(conn, WS_CLOSE_EPROTO, NULL);
          return;
        }

        s->on_ws_close(conn, code, buf + mask_offset + 6);

        return;
      }

    } else {
      // unknown opcode
      conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
      return;
    }
  }

  printf("done\n");
  return;
}

int handle_ws(ws_server_t *s, struct ws_conn_t *conn) {
  int epfd = s->io_ctl.epoll_fd;
  uint8_t *buf = buf_peek(&conn->read_buf);
  int masked = frame_is_masked(buf);
  // if mask bit isn't set close the connection
  if ((masked == 0) | (frame_has_reserved_bits_set(buf) == 1)) {
    conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
    return -1;
  }

  size_t rbuf_len = buf_len(&conn->read_buf);
  printf("%zu\n", rbuf_len);
  uint8_t fin = frame_get_fin(buf);
  uint8_t opcode = frame_get_opcode(buf);
  if (!fin || conn->fmsg) {
    if (opcode == OP_CONT) {
      conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
      return -1;
    }
    conn->fmsg = 1;
    conn->fmsg_bin = opcode == OP_BIN;
    conn->fmsg_end = conn->read_buf.rpos;
    handle_ws_fmsg(s, conn);
    return -1;
  }

  size_t len = 0;
  int missing_len = frame_decode_payload_len(buf, rbuf_len, &len);
  if (missing_len) {
    conn->rlo_watermark = missing_len;
    return 0;
  }

  size_t mask_offset = frame_get_mask_offset(len);
  size_t flen = len + mask_offset + 4;

  if (rbuf_len < flen) {
    conn->rlo_watermark = flen; // assuming payload is at the front
    return 0;
  }

  if ((opcode == OP_BIN) | (opcode == OP_TXT)) {
    msg_unmask(buf + mask_offset + 4, len);
    if (opcode == OP_TXT) {
      if (!utf8_is_valid(buf + mask_offset + 4, len)) {
        conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
        return -1; // TODO(sah): send a Close frame, & call close callback
      }

      s->on_ws_msg(conn, buf + mask_offset + 4, len, 0);
    } else {
      s->on_ws_msg(conn, buf + mask_offset + 4, len, 1);
    }
    buf_consume(&conn->read_buf, flen);
    conn->rlo_watermark = 2;

  } else if ((opcode == OP_PING) | (opcode == OP_PONG)) {
    if (len > 125) {
      // PINGs must be 125 or less
      conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
      return -1; // TODO(sah): send a Close frame, & call close callback
    }

    size_t mask_offset = frame_get_mask_offset(len);

    msg_unmask(buf + mask_offset + 4, len);
    if (opcode == OP_PING) {
      s->on_ws_ping(conn, buf + mask_offset + 4, len);
    } else {
      s->on_ws_pong(conn, buf + mask_offset + 4, len);
    }

    buf_consume(&conn->read_buf, flen);
    conn->rlo_watermark = 2;

  } else if (opcode == OP_CLOSE) {
    // handle close stuff
    uint16_t code = WS_CLOSE_NOSTAT; // pessimistically assume no code provided
    if (!len) {
      s->on_ws_close(conn, WS_CLOSE_NORMAL, NULL);
      return -1;
    } else if (len < 2) {
      s->on_ws_close(conn, WS_CLOSE_EPROTO, NULL);
      return -1;
    }

    else {
      if (len > 125) {
        // close frames can be more but this is the most that will be
        // supported for various reasons close frames generally should
        // contain just the 16bit code and a short string for the reason at
        // most
        conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
        return -1;
      }

      msg_unmask(buf + mask_offset + 4, len);

      if (!utf8_is_valid(buf + mask_offset + 6, len - 2)) {
        conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
        return -1;
      };

      code = (buf[6] << 8) | buf[7];
      if (code < 1000 || code == 1004 || code == 1100 || code == 1005 ||
          code == 1006 || code == 1015 || code == 1016 || code == 2000 ||
          code == 2999) {
        s->on_ws_close(conn, WS_CLOSE_EPROTO, NULL);
        return -1;
      }

      s->on_ws_close(conn, code, buf + mask_offset + 6);

      return -1; // stop with -1 here this is the last frame we will handle
    }

  } else if (opcode == OP_CONT) {
    conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
    return -1;
  } else {
    // unknown opcode
    conn_destroy(s, conn, epfd, WS_CLOSE_EPROTO, &s->io_ctl.ev);
    return -1;
  }

  return 0;
}

void conn_destroy(ws_server_t *s, struct ws_conn_t *conn, int epfd, int err,
                  struct epoll_event *ev) {
  int ret;
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

  free(conn);
}

int conn_drain_write_buf(struct ws_conn_t *conn) {
  size_t to_write = buf_len(&conn->write_buf);
  ssize_t n = 0;

  n = buf_send(&conn->write_buf, conn->fd, 0);
  if (n == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    } else {
      ws_server_t *s = ws_conn_server(conn);
      conn_destroy(ws_conn_server(conn), conn, s->io_ctl.epoll_fd,
                   WS_CLOSE_ABNORM, &s->io_ctl.ev);
      return -1;
    }
  } else if (n == 0) {
    ws_server_t *s = ws_conn_server(conn);
    conn_destroy(ws_conn_server(conn), conn, s->io_ctl.epoll_fd,
                 WS_CLOSE_ABNORM, &s->io_ctl.ev);
    return -1;
  }

  if (to_write == n) {
    conn->writeable = 1;
    return 1;
  };

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
    conn_destroy(s, c, s->io_ctl.epoll_fd, errno, &s->io_ctl.ev);
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
    conn_destroy(s, c, s->io_ctl.epoll_fd, errno, &s->io_ctl.ev);
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
    conn_destroy(s, c, s->io_ctl.epoll_fd, errno, &s->io_ctl.ev);
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
    conn_destroy(s, c, s->io_ctl.epoll_fd, errno, &s->io_ctl.ev);
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

  if (conn->writeable) {
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
  ret = epoll_ctl(s->io_ctl.epoll_fd, EPOLL_CTL_DEL, conn->fd, &s->io_ctl.ev);
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
int conn_write_frame(ws_server_t *s, ws_conn_t *conn, void *data, size_t len,
                     uint8_t op) {
  ssize_t n = 0;
  size_t hlen = frame_get_mask_offset(len);
  size_t flen = len + hlen;

  if ((conn->writeable == 1) & (buf_space(&conn->write_buf) > flen)) {
    uint8_t hbuf[hlen];
    memset(hbuf, 0, hlen);
    struct iovec iovs[2];
    hbuf[0] = FIN | op; // Set FIN bit and opcode
    if (hlen == 2) {
      hbuf[1] = (uint8_t)len;
    } else if (hlen == 4) {
      hbuf[1] = PAYLOAD_LEN_16;
      hbuf[2] = (len >> 8) & 0xFF;
      hbuf[3] = len & 0xFF;
    } else {
      hbuf[1] = PAYLOAD_LEN_64;
      hbuf[2] = (len >> 56) & 0xFF;
      hbuf[3] = (len >> 48) & 0xFF;
      hbuf[4] = (len >> 40) & 0xFF;
      hbuf[5] = (len >> 32) & 0xFF;
      hbuf[6] = (len >> 24) & 0xFF;
      hbuf[7] = (len >> 16) & 0xFF;
      hbuf[8] = (len >> 8) & 0xFF;
      hbuf[9] = len & 0xFF;
    }

    iovs[0].iov_len = hlen;
    iovs[0].iov_base = hbuf;
    iovs[1].iov_len = len;
    iovs[1].iov_base = data;

    n = writev(conn->fd, iovs, 2);
    if (n == 0) {
      return -1;
    } else if (n == -1) {
      if (!((errno == EAGAIN) | (errno == EWOULDBLOCK))) {
        return -1;
      }
      n = 0;
    }

    if (n < flen) {
      conn->writeable = 0;
      if (n > hlen) {
        assert(buf_put(&conn->write_buf, (uint8_t *)data + (n - hlen),
                       flen - n) == 0);
      } else {
        assert(buf_put(&conn->write_buf, hbuf + n, hlen - n) == 0);
        assert(buf_put(&conn->write_buf, data, len) == 0);
      }

      s->io_ctl.ev.events = EPOLLOUT | EPOLLRDHUP;
      s->io_ctl.ev.data.ptr = conn;
      if (epoll_ctl(s->io_ctl.epoll_fd, EPOLL_CTL_MOD, conn->fd,
                    &s->io_ctl.ev) == -1) {
        return -1;
      };
      return 0;
    }
  }

  return n == flen;
}

int conn_send(ws_server_t *s, ws_conn_t *conn, const void *data, size_t len) {
  ssize_t n = 0;

  if ((conn->writeable == 1) & (buf_space(&conn->write_buf) > len)) {
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
      s->io_ctl.ev.events = EPOLLOUT | EPOLLRDHUP;
      s->io_ctl.ev.data.ptr = conn;
      conn->writeable = 0;
      assert(buf_put(&conn->write_buf, (uint8_t *)data + n, len - n) == 0);
      if (epoll_ctl(s->io_ctl.epoll_fd, EPOLL_CTL_MOD, conn->fd,
                    &s->io_ctl.ev) == -1) {
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

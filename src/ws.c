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

struct ws_conn_t {
  int fd;
  bool writeable;
  bool upgraded;
  ws_server_t *base;
  void *ctx; // user data ptr
  struct iovec iov[2];
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
  ws_open_cb_t on_ws_open;
  ws_msg_cb_t on_ws_msg;
  ws_ping_cb_t on_ws_ping;
  ws_drain_cb_t on_ws_drain;
  ws_close_cb_t on_ws_close;
  ws_disconnect_cb_t on_ws_disconnect;
  ws_err_cb_t on_ws_err;
  io_ctl_t io_ctl; // io controller
} ws_server_t;

// generic send function
int conn_send(ws_server_t *s, ws_conn_t *conn, const void *data, size_t n);

int handle_conn(ws_server_t *s, struct ws_conn_t *conn, int nops);

void conn_destroy(ws_server_t *s, struct ws_conn_t *conn, int epfd,
                  struct epoll_event *ev);

int conn_drain_write_buf(struct ws_conn_t *conn, int nops);

int conn_write_frame(ws_server_t *s, ws_conn_t *conn, void *data, size_t len,
                     uint8_t op);

static inline void frame_payload_unmask(unsigned char *src, unsigned char *dst,
                                        uint8_t *mask, size_t len);

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
      !params->on_ws_ping || !params->on_ws_err) {
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
  s->on_ws_msg = params->on_ws_msg;
  s->on_ws_drain = params->on_ws_drain;
  s->on_ws_close = params->on_ws_close;
  s->on_ws_disconnect = params->on_ws_disconnect;
  s->on_ws_err = params->on_ws_err;
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
          ev.data.ptr = conn;

          assert(buf_init(&conn->read_buf) == 0);
          assert(buf_init(&conn->write_buf) == 0);

          if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
            int err = errno;
            s->on_ws_err(s, err);
          };
        }

      } else {
        if (s->io_ctl.events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
          conn_destroy(s, s->io_ctl.events[i].data.ptr, epfd, &ev);
          s->io_ctl.events[i].data.ptr = NULL;
        } else {
          if (s->io_ctl.events[i].events & EPOLLOUT) {
            int ret = conn_drain_write_buf(s->io_ctl.events[i].data.ptr, 8);
            if (ret == -1) {
              conn_destroy(s, s->io_ctl.events[i].data.ptr, epfd, &ev);
              s->io_ctl.events[i].data.ptr = NULL;
            } else if (ret == 1) {
              s->on_ws_drain(s->io_ctl.events[i].data.ptr);
            }
          } else if (s->io_ctl.events[i].events & EPOLLIN) {
            int ret = handle_conn(s, s->io_ctl.events[i].data.ptr, 8);
            if (ret == -1) {
              conn_destroy(s, s->io_ctl.events[i].data.ptr, epfd, &ev);
              s->io_ctl.events[i].data.ptr = NULL;
            }
          }
        }
      }
    }
  }

  return 0;
}

#define would_block(n) (n == -1) & ((errno == EAGAIN) | (errno == EWOULDBLOCK))

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

int handle_conn(ws_server_t *s, struct ws_conn_t *conn, int nops) {
  ssize_t n = buf_recv(&conn->read_buf, conn->fd, 0);

  if (n == -1) {
    if ((errno == EAGAIN || errno == EWOULDBLOCK)) {
      return 0;
    }
    return -1;
  } else if (n == 0) {
    return -1;
  }

  conn->read_buf.buf[buf_len(&conn->read_buf)] = '\0';

  if (!strncmp((char *)buf_peek(&conn->read_buf), GET_RQ, sizeof GET_RQ - 1)) {
    // printf("Req --------------------------------\n");
    // printf("%s", conn->buf_in);
    char res_hdrs[1024] = {0};
    ssize_t ret = handle_upgrade((char *)buf_peek(&conn->read_buf), res_hdrs,
                                 sizeof res_hdrs);

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
    buf_consume(&conn->read_buf, buf_len(&conn->read_buf));
  } else {
    // printf("buffer size: %zu\n", conn->buf_in_len);
    // need at least 2 bytes to read anything

    if (buf_len(&conn->read_buf) >= 2) {
      uint8_t fin = frame_get_fin(buf_peek(&conn->read_buf));
      int masked = frame_is_masked(buf_peek(&conn->read_buf));
      // if mask bit isn't set close the connection
      if (!masked) {
        printf("received unmasked client data\n");
        return -1;
      }

      uint8_t opcode = frame_get_opcode(buf_peek(&conn->read_buf));
      size_t len = frame_payload_get_len(buf_peek(&conn->read_buf));
      // printf("fin: %d\n", fin);
      // printf("opcode: %d\n", opcode);
      // printf("len: %zu\n", len);
      if (len == PAYLOAD_LEN_16) {
        if (buf_len(&conn->read_buf) > 3) {
          len = frame_payload_get_len126(buf_peek(&conn->read_buf));
        } else {
          return 0;
        }
      } else if (len == PAYLOAD_LEN_64) {
        if (buf_len(&conn->read_buf) > 9) {
          len = frame_payload_get_len127(buf_peek(&conn->read_buf));
        } else {
          return 0;
        }
      }

      if ((opcode == OP_BIN) | (opcode == OP_TXT)) {
        size_t mask_offset = frame_get_mask_offset(len);
        size_t hlen = len + mask_offset + 4;
        if (buf_len(&conn->read_buf) >= hlen) {
          s->on_ws_msg(conn, buf_peek(&conn->read_buf) + mask_offset + 4, len,
                       opcode == OP_BIN);

          buf_consume(&conn->read_buf, hlen);
        }
        // msg
      } else if ((opcode == OP_PING) | (opcode == OP_PONG)) {
        // handle ping pong stuff
        if (len > 125) {
          // PINGs must be 125 or less
          return -1; // TODO(sah): send a Close frame, & call close callback
        }
        uint8_t *buf = buf_peek(&conn->read_buf);
        size_t mask_offset = frame_get_mask_offset(len);
        size_t hlen = len + mask_offset + 4;

        if (buf_len(&conn->read_buf) >= hlen) {
          // pings are unmasked automatically
          frame_payload_unmask(buf + mask_offset + 4, buf + mask_offset + 4,
                               buf + mask_offset, len);
          s->on_ws_ping(conn, buf_peek(&conn->read_buf) + mask_offset + 4, len);
          buf_consume(&conn->read_buf, hlen);
        }

      } else if (opcode == OP_CLOSE) {
        // handle close stuff
      } else if (opcode == OP_CONT) {
        return -1; // unsupported
      }
    }
  }

  return 0;
}

void conn_destroy(ws_server_t *s, struct ws_conn_t *conn, int epfd,
                  struct epoll_event *ev) {
  int err = errno;
  s->on_ws_disconnect(conn, err); // call the user's callback to allow clean up
                                  // on data associated with this connection
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
  }

  free(conn);
}

int conn_drain_write_buf(struct ws_conn_t *conn, int nops) {
  int i = 0;
  ssize_t n;
  bool drained;
  do {
    n = buf_send(&conn->write_buf, conn->fd, 0);
    if (n == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      } else {
        return -1;
      }
    } else if (n == 0) {
      return -1;
    }

  } while ((i++ < nops) & (drained = buf_len(&conn->write_buf) > 0));

  if (drained) {
    conn->writeable = 1;
    return 1;
  };

  return 0;
}

inline int ws_conn_close(ws_server_t *s, ws_conn_t *c, void *msg, size_t n,
                         int reason) {
  return -1; // TODO
}

inline int ws_conn_destroy(ws_server_t *s, ws_conn_t *c) {
  return -1; // TODO
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
    conn_destroy(s, c, s->io_ctl.epoll_fd, &s->io_ctl.ev);
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
    conn_destroy(s, c, s->io_ctl.epoll_fd, &s->io_ctl.ev);
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
    conn_destroy(s, c, s->io_ctl.epoll_fd, &s->io_ctl.ev);
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
    conn_destroy(s, c, s->io_ctl.epoll_fd, &s->io_ctl.ev);
  }
  return stat == 1;
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

    conn->iov[0].iov_len = hlen;
    conn->iov[0].iov_base = hbuf;
    conn->iov[1].iov_len = len;
    conn->iov[1].iov_base = data;

    n = writev(conn->fd, conn->iov, 2);
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

static inline void frame_payload_unmask(unsigned char *src, unsigned char *dst,
                                        uint8_t *mask, size_t len) {
  size_t mask_idx = 0;
  for (size_t i = 0; i < len; ++i) {
    dst[i] = src[i] ^ mask[mask_idx];
    mask_idx = (mask_idx + 1) & 3;
  }
}

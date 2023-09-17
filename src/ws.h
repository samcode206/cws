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


#define FIN 0x80

#define OP_CONT 0x0
#define OP_TXT 0x1
#define OP_BIN 0x2
#define OP_PING 0x9
#define OP_PONG 0xA
#define OP_CLOSE 0x8

#define PAYLOAD_LEN_16 126
#define PAYLOAD_LEN_64 127

// errors 
#define ERR_HDR_NOT_FOUND -2
#define ERR_HDR_MALFORMED -3
#define ERR_HDR_TOO_LARGE -4

#define WS_ESYS -5       // system error call should check errno
#define WS_EINVAL_ARGS -6 // invalid argument/arguments provided

#define WS_CREAT_EBAD_PORT -7
#define WS_CREAT_ENO_CB -8

// server types
typedef struct ws_conn_t ws_conn_t;

typedef struct server ws_server_t;

typedef void (*ws_open_cb_t)(
    ws_conn_t *ws_conn); /* called after a connection is upgraded */

typedef void (*ws_msg_cb_t)(
    ws_conn_t *c, void *msg, size_t n,
    bool bin); /* called when a websocket msg is available */

typedef void (*ws_ping_cb_t)(ws_conn_t *c, void *msg,
                             size_t n); /* called when a client sends a PING */

typedef void (*ws_pong_cb_t)(ws_conn_t *c, void *msg,
                             size_t n); /* called when a client sends a PONG */

typedef void (*ws_close_cb_t)(
    ws_conn_t *ws_conn, int reason); /* called when a close frame is received */

typedef void (*ws_disconnect_cb_t)(ws_conn_t *ws_conn,
                                   int err); /* called after the connection is
                                       closed, use for user data clean up */

typedef void (*ws_drain_cb_t)(
    ws_conn_t *ws_conn); /* called after send buffer is drained (after some back
                            pressure buildup) */

typedef void (*ws_err_cb_t)(ws_server_t *s,
                            int err); /* called if an internal error occurs */

struct ws_server_params {
  in_addr_t addr;
  uint16_t port;
  size_t max_events; // defaults to 1024
  ws_open_cb_t on_ws_open;
  ws_msg_cb_t on_ws_msg;
  ws_ping_cb_t on_ws_ping;
  ws_pong_cb_t on_ws_pong;
  ws_drain_cb_t on_ws_drain;
  ws_close_cb_t on_ws_close;
  ws_disconnect_cb_t on_ws_disconnect;
  ws_err_cb_t on_ws_err;
};

int ws_conn_fd(ws_conn_t *c);

int ws_conn_pong(ws_server_t *s, ws_conn_t *c, void *msg, size_t n);
int ws_conn_ping(ws_server_t *s, ws_conn_t *c, void *msg, size_t n);
int ws_conn_close(ws_server_t *s, ws_conn_t *c, void *msg, size_t n,
                  int reason);
int ws_conn_destroy(ws_server_t *s, ws_conn_t *c);
int ws_conn_send_txt(ws_server_t *s, ws_conn_t *c, void *msg, size_t n);
int ws_conn_send(ws_server_t *s, ws_conn_t *c, void *msg, size_t n);

ws_server_t *ws_conn_server(ws_conn_t *c);
void *ws_conn_ctx(ws_conn_t *c);
void ws_conn_ctx_attach(ws_conn_t *c, void *ctx);


ws_server_t *ws_server_create(struct ws_server_params *params,
                              int *ret); // allocates server resources

int ws_server_start(ws_server_t *s, int backlog); // start serving connections


void msg_unmask(unsigned char *src, unsigned char *dst,
                              size_t len);

#endif /* WS_PROTOCOL_PARSING23_H */

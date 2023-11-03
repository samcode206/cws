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


/**
 * WebSocket server connection structure.
 */
typedef struct ws_conn_t ws_conn_t;

/**
 * WebSocket server structure.
 */
typedef struct server ws_server_t;

/**
 * Callback invoked after a WebSocket connection is successfully upgraded.
 *
 * Caller can attach connection-specific context or resources
 * to the connection (see `ws_conn_set_ctx` below). These resources can utilized throughout the lifetime of
 * the WebSocket connection, which is valid until the `ws_disconnect_cb_t` is called (see below).
 *
 * @param ws_conn Pointer to the WebSocket connection (`ws_conn_t`).
 */
typedef void (*ws_open_cb_t)(ws_conn_t *ws_conn);

/**
 * Callback invoked when a complete WebSocket message is available.
 *
 * NOTE: The 'msg' data is provided for use only within this callback.
 * If the caller needs to retain any part of the message beyond this callback,
 * it must be copied to a separate buffer.
 *
 * @param c    Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg  Pointer to the message data.
 * @param n    Size of the message in bytes.
 * @param bin  Boolean indicating if the message is binary (`true`) or text (`false`).
 */
typedef void (*ws_msg_cb_t)(ws_conn_t *c, void *msg, size_t n, bool bin);

/**
 * Callback invoked when a PING frame is received from the client.
 *
 * NOTE: The 'msg' data is provided for use only within this callback.
 * If the caller needs to retain any part of the ping message beyond this callback,
 * it must be copied to a separate buffer.
 *
 * @param c    Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg  Pointer to the ping message data.
 * @param n    Size of the ping message in bytes.
 */
typedef void (*ws_ping_cb_t)(ws_conn_t *c, void *msg, size_t n);

/**
 * Callback invoked when a PONG frame is received from the client.
 *
 * NOTE: The 'msg' data is provided for use only within this callback.
 * If the caller needs to retain any part of the pong message beyond this callback,
 * it must be copied to a separate buffer.
 * This usually occurs in response to a PING frame sent by the server.
 *
 * @param c    Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg  Pointer to the pong message data.
 * @param n    Size of the pong message in bytes.
 */
typedef void (*ws_pong_cb_t)(ws_conn_t *c, void *msg, size_t n);

/**
 * Callback invoked when a close frame is received from the client.
 *
 * NOTE: The 'reason' data is provided for use only within this callback.
 * If the caller needs to retain any part of the closure reason beyond this callback,
 * it must be copied to a separate buffer.
 *
 * @param ws_conn Pointer to the WebSocket connection (`ws_conn_t`).
 * @param code    Status code for the closure.
 * @param reason  Pointer to the reason string for the closure.
 */
typedef void (*ws_close_cb_t)(ws_conn_t *ws_conn, int code, const void *reason);

/**
 * Callback invoked after the WebSocket connection has been closed.
 *
 * This callback must be used to perform cleanup operations for any user data 
 * associated with the connection. 
 
 * IMPORTANT: It is crucial that after this callback is invoked, 
 * no further references to the connection are made, references made to the connection 
 * after this callback are not valid and will lead to undefined behavior
 *
 * @param ws_conn Pointer to the WebSocket connection (`ws_conn_t`).
 * @param err     Error code indicating the reason for disconnection, if any.
 */
typedef void (*ws_disconnect_cb_t)(ws_conn_t *ws_conn, int err);

/**
 * Callback invoked when the connection's send buffer is ready to accept more data.
 *
 * This callback serves as a notification that the connection has alleviated back pressure,
 * allowing for additional write operations. It is typically called
 * after a previous attempt to send data could not transmit all the data, resulting in built-up back pressure.
 *
 * @param ws_conn Pointer to the WebSocket connection (`ws_conn_t`).
 */
typedef void (*ws_drain_cb_t)(ws_conn_t *ws_conn);

/**
 * Callback invoked when an internal server error occurs.
 *
 * @param s   Pointer to the WebSocket server (`ws_server_t`).
 * @param err Error code of the occurred error.
 */
typedef void (*ws_err_cb_t)(ws_server_t *s, int err);

/**
 * Callback invoked upon receiving a fragment of a WebSocket message.
 *
 * This callback pertains to the fragmentation feature of the WebSocket protocol,
 * not to be confused with a partially read message at the socket level. It is
 * generally inadvisable to register this callback unless there is a compelling
 * reason, as it is rarely, if ever, needed for typical application use.
 *
 * Registering this callback places the responsibility on the caller to manage the reassembly
 * of message fragments. The `fin` parameter, when true, signals that the last
 * fragment has been received, thereby indicating message completion.
 *
 * IMPORTANT: Once this callback is registered, `ws_msg_cb_t` will not be called
 * after the last fragment is received. Fragment data is not preserved in the WebSocket parsing
 * buffer after this callback is invoked. Thorough understanding of WebSocket
 * message fragmentation and its management is essential before opting to use this callback.
 *
 * NOTE: Registering this callback does not affect the handling of non-fragmented messages.
 * Non-fragmented messages will continue to be received through `ws_msg_cb_t` as usual (see above).
 *
 * @param c        Pointer to the WebSocket connection (`ws_conn_t`).
 * @param fragment Pointer to the data of the received fragment.
 * @param n        Size of the fragment in bytes.
 * @param fin      Boolean indicating if this fragment is the last in the message.
 *
 * @typedef void (*ws_msg_fragment_cb_t)(ws_conn_t *c, void *fragment, size_t n, bool fin);
 */
typedef void (*ws_msg_fragment_cb_t)(ws_conn_t *c, void *fragment, size_t n,
                                     bool fin);

// Server parameter structure with optional callbacks for various WebSocket events.
struct ws_server_params {
  const char *addr;
  uint16_t port;
  uint64_t max_conns;        // Maximum connections the server is willing to accept.
                             // Defaults to the system's limit for maximum open file descriptors.
  size_t max_buffered_bytes; // Maximum amount of websocket payload data to buffer before the connection
                             // is dropped. Defaults to 16000 bytes.
  ws_open_cb_t on_ws_open;             // Callback for when a WebSocket connection is opened.
  ws_msg_cb_t on_ws_msg;               // Callback for when a complete WebSocket message is received.
  ws_msg_fragment_cb_t on_ws_msg_fragment; // Callback for when a WebSocket message fragment is received.
  ws_ping_cb_t on_ws_ping;             // Callback for when a PING frame is received.
  ws_pong_cb_t on_ws_pong;             // Callback for when a PONG frame is received.
  ws_drain_cb_t on_ws_drain;           // Callback for when the send buffer has been drained.
  ws_close_cb_t on_ws_close;           // Callback for when a close frame is received.
  ws_disconnect_cb_t on_ws_disconnect; // Callback for after the connection has been closed.
  ws_err_cb_t on_ws_err;               // Callback for when an internal error occurs.
};

int ws_conn_fd(ws_conn_t *c);

int ws_conn_pong(ws_conn_t *c, void *msg, size_t n);
int ws_conn_ping(ws_conn_t *c, void *msg, size_t n);
void ws_conn_close(ws_conn_t *c, void *msg, size_t n, uint16_t code);
void ws_conn_destroy(ws_conn_t *c);
int ws_conn_send_txt(ws_conn_t *c, void *msg, size_t n);
int ws_conn_send(ws_conn_t *c, void *msg, size_t n);

ws_server_t *ws_conn_server(ws_conn_t *c);
void *ws_conn_ctx(ws_conn_t *c);
void ws_conn_set_ctx(ws_conn_t *c, void *ctx);

ws_server_t *ws_server_create(struct ws_server_params *params,
                              int *ret); // allocates server resources

int ws_server_start(ws_server_t *s, int backlog); // start serving connections

bool ws_conn_msg_bin(ws_conn_t *c);

int utf8_is_valid(uint8_t *s, size_t n);


/**
 * Normal closure; the purpose for which the connection was
 * established has been fulfilled.
 */
#define WS_CLOSE_NORMAL     1000 

/**
 * Endpoint going away, such as a server shutting down or
 * a browser navigating away from a page.
 */
#define WS_CLOSE_GOAWAY     1001

/**
 * Protocol error encountered.
 */
#define WS_CLOSE_PROTOCOL   1002 

/**
 * Unsupported data; the client expects text but the server
 * sends binary data, for instance.
 */
#define WS_CLOSE_UNSUPPORTED 1003

/**
 * Invalid data; for example, non-UTF-8 data within a text message.
 */
#define WS_CLOSE_INVALID    1007

/**
 * Policy violation.
 */
#define WS_CLOSE_POLICY     1008

/**
 * The message is too large for the server to process.
 */
#define WS_CLOSE_TOO_LARGE  1009

/**
 * Client ending connection due to expected server extension negotiation failure.
 */
#define WS_CLOSE_EXTENSION  1010

/**
 * An unexpected condition prevented the server from fulfilling the request.
 */
#define WS_CLOSE_UNEXPECTED 1011

/**
 * No status code was present in the close frame.
 */
#define WS_CLOSE_NO_STATUS  1005

/**
 * Connection closed abnormally, such as without sending/receiving a close frame.
 */
#define WS_CLOSE_ABNORMAL   1006



// errors
#define ERR_HDR_NOT_FOUND -2
#define ERR_HDR_MALFORMED -3
#define ERR_HDR_TOO_LARGE -4

#define WS_ESYS -5        // system error call should check errno
#define WS_EINVAL_ARGS -6 // invalid argument/arguments provided

#define WS_CREAT_EBAD_PORT -7
#define WS_CREAT_ENO_CB -8



#endif /* WS_PROTOCOL_PARSING23_H */

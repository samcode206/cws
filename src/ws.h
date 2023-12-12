/* The MIT License

   Copyright (c) 2023 by Sam H

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
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef WS_PROTOCOL_PARSING23_H
#define WS_PROTOCOL_PARSING23_H

#include <errno.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// #define WITH_COMPRESSION


/**
 * WebSocket server connection structure.
 */
typedef struct ws_conn_t ws_conn_t;

/**
 * WebSocket server structure.
 */
typedef struct server ws_server_t;



enum ws_send_status {
  /*
    Send/writev call failed, or the connection is closing/closed.
  */
  WS_SEND_FAILED = -1, 

  /*
    Send successful. The user may send more frames.
  */
  WS_SEND_OK = 0,

  /*
    Data placed in send buffer, but there's backpressure. 
    The caller should check available space before more sends or wait for on_ws_drain.
  */
  WS_SEND_OK_BACKPRESSURE = 1, 

  /*
    Frame dropped due to insufficient space. 
    The caller should wait for on_ws_drain before retrying.
  */
  WS_SEND_DROPPED_NEEDS_DRAIN = 2,

  /*
    Frame too large and fragmentation is not allowed. This applies to control
    frames (ping, pong, close) which have a maximum payload limit of 125 bytes.
  */
  WS_SEND_DROPPED_TOO_LARGE = 3,

  /*
    Frame too large, far exceeding the specified max_buffer_bytes set for the server. 
    Such frames require fragmentation. Users should use the fragmented send 
    variant and track progress with on_ws_drain When WS_SEND_OK_BACKPRESSURE is returned.
  */
  WS_SEND_DROPPED_NEEDS_FRAGMENTATION = 4,

  /*
    Sending a complete data frame (text|binary) is not allowed 
    due to ongoing fragmented messages. Wait until the final fragment 
    is sent before sending a complete text/binary frame. 
    Control messages are not affected and can be sent interleaved.
  */
  WS_SEND_DROPPED_NOT_ALLOWED = 5,

  /*
    Compressed message not supported by the client. 
    Triggered in fragmented sends where compression is pre-applied. 
    In normal sends, falls back to no compression.
  */
  WS_SEND_DROPPED_UNSUPPORTED = 6,
};


/**
 * Attempts to send a text message immediately over the WebSocket connection. If the socket 
 * is not in a writable state, the message is queued for later transmission. The caller should 
 * check the return status to monitor the state.
 * @param c        Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg      Pointer to the text message data.
 * @param n        Size of the text message in bytes.
 * @param compress Boolean indicating whether to compress the message.
 * @return         enum ws_send_status indicating the result of the send attempt.
 */
enum ws_send_status ws_conn_send_txt(ws_conn_t *c, void *msg, size_t n, bool compress);

/**
 * Queues a text message for asynchronous sending over the WebSocket connection.
 * Beneficial for batching multiple small messages. Can be sent later either via 'ws_conn_flush_pending'
 * or automatically by the event loop.
 * @param c        Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg      Pointer to the text message data.
 * @param n        Size of the text message in bytes.
 * @param compress Boolean indicating whether to compress the message.
 * @return         enum ws_send_status indicating the queuing result.
 */
enum ws_send_status ws_conn_put_txt(ws_conn_t *c, void *msg, size_t n, bool compress);


/**
 * Attempts to send a binary message immediately over the WebSocket connection. If the socket 
 * is not in a writable state, the message is queued for later transmission. The caller should 
 * check the return status to monitor the state.
 * @param c        Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg      Pointer to the binary message data.
 * @param n        Size of the binary message in bytes.
 * @param compress Boolean indicating whether to compress the message.
 * @return         enum ws_send_status indicating the result of the send attempt.
 */
enum ws_send_status ws_conn_send(ws_conn_t *c, void *msg, size_t n, bool compress);


/**
 * Queues a binary message for asynchronous sending over the WebSocket connection.
 * Useful for batching multiple small messages. Can be sent later either via 'ws_conn_flush_pending'
 * or automatically by the event loop.
 * @param c        Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg      Pointer to the binary message data.
 * @param n        Size of the binary message in bytes.
 * @param compress Boolean indicating whether to compress the message.
 * @return         enum ws_send_status indicating the queuing result.
 */
enum ws_send_status ws_conn_put_bin(ws_conn_t *c, void *msg, size_t n, bool compress);


/**
 * Attempts to send a pong message immediately over the WebSocket connection, responding to a ping.
 * If the socket is not writable, the message is queued. The caller should check the return status.
 * @param c   Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg Pointer to the pong message data.
 * @param n   Size of the pong message in bytes.
 * @return    enum ws_send_status indicating the result of the send attempt.
 */
enum ws_send_status ws_conn_pong(ws_conn_t *c, void *msg, size_t n);

/**
 * Queues a pong message for asynchronous sending over the WebSocket connection.
 * Allows for batching and delayed sending controlled by the event loop.
 * @param c   Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg Pointer to the pong message data.
 * @param n   Size of the pong message in bytes.
 * @return    enum ws_send_status indicating the queuing result.
 */
enum ws_send_status ws_conn_put_pong(ws_conn_t *c, void *msg, size_t n);

/**
 * Attempts to send a ping message immediately over the WebSocket connection.
 * If the socket is not writable, the message is queued. The caller should check the return status.
 * @param c   Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg Pointer to the ping message data.
 * @param n   Size of the ping message in bytes.
 * @return    enum ws_send_status indicating the result of the send attempt.
 */
enum ws_send_status ws_conn_ping(ws_conn_t *c, void *msg, size_t n);

/**
 * Queues a ping message for asynchronous sending over the WebSocket connection.
 * @param c   Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg Pointer to the ping message data.
 * @param n   Size of the ping message in bytes.
 * @return    enum ws_send_status indicating the queuing result.
 */
enum ws_send_status ws_conn_put_ping(ws_conn_t *c, void *msg, size_t n);

/**
 * Closes the WebSocket connection with a "fire and forget" approach. 
 * Sends a close frame with the provided message and status code if the socket is writable; 
 * otherwise, the connection is dropped similar to `ws_conn_destroy`. The user should wait to free 
 * resources until `on_ws_disconnect` is called.
 * @param c    Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg  Pointer to the close message data.
 * @param n    Size of the close message in bytes.
 * @param code Status code for closure.
 */
void ws_conn_close(ws_conn_t *c, void *msg, size_t n, uint16_t code);


/**
 * Destroys the WebSocket connection ungracefully.
 *
 * This function immediately terminates the WebSocket connection without
 * going through the standard WebSocket close handshake. It should be used
 * in scenarios where an immediate disconnection is required. The users should
 * be aware that this abrupt termination might lead to unclean state on the
 * client side.
 *
 * Resources associated with the connection may still need to be cleaned up.
 * Cleanup should typically be handled in the 'on_ws_disconnect' callback, which
 * will be invoked following the destruction of the connection.
 *
 * @param c Pointer to the WebSocket connection (`ws_conn_t`) to be destroyed.
 * @param reason Code reason for the closure
 */
void ws_conn_destroy(ws_conn_t *c, unsigned long reason);



/**
 * Flushes any pending frames in the send buffer of the WebSocket connection.
 * This function may be used after queuing messages with 'put' variants (see Above) to attempt flushing all queued messages.
 * this is useful in cases where multiple small messages are sent to many clients in which it's possible to put all frames to be sent
 * then calling `ws_conn_flush_pending` before moving on to the next client to allow reuse of the buffer.
 * @param c Pointer to the WebSocket connection (`ws_conn_t`).
 */
void ws_conn_flush_pending(ws_conn_t *c);



/**
 * Returns the current maximum sendable length for a single frame on this connection.
 * This considers the largest possible WebSocket header and any existing backpressure.
 * @param c Pointer to the WebSocket connection (`ws_conn_t`).
 * @return  Size of the largest possible frame that can currently be sent.
 */
size_t ws_conn_max_sendable_len(ws_conn_t *c);


/**
* Returns the estimated number of bytes not yet proccess in the Connection's receive buffer
* if called during on_ws_msg and ws_conn_readable_len returns zero this indicates that the receive buffer is drained
* otherwise a new frame may soon be ready for processing
* @param c Pointer to the WebSocket connection (`ws_conn_t`).
* @return  Size of the current number of bytes not yet processed
*/
size_t ws_conn_estimate_readable_len(ws_conn_t *c);

/**
 * Checks if there is enough space in the connection's send buffer for a message of given length.
 * @param c       Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg_len Length of the message in bytes to check.
 * @return        True if there is enough space, false otherwise.
 */
bool ws_conn_can_put_msg(ws_conn_t *c, size_t msg_len);




/**
 * Sends a fragmented message to the client.
 * @param c              Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg            Pointer to the message data.
 * @param len            Length of the fragment in bytes.
 * @param txt            Boolean indicating if the message is text (true) or binary (false).
 * @param final          Boolean indicating if this is the final fragment.
 * @return               enum ws_send_status
 */
enum ws_send_status ws_conn_send_fragment(ws_conn_t *c, void *msg, size_t len, bool txt, bool final);



/**
 * Checks if the connection is currently sending a fragmented message.
 *
 * When this function returns true, it indicates that the WebSocket connection
 * is in the middle of transmitting a fragmented message. According to the WebSocket
 * protocol, no other data frames should be sent until the fragmented message is complete.
 * However, control frames (such as ping, pong, and close) may still be interleaved and sent
 * during this period. if data frames are sent while sending a fragmented message
 * they will fail with WS_SEND_DROPPED_NOT_ALLOWED
 *
 * @param c Pointer to the WebSocket connection (`ws_conn_t`).
 * @return  True if the connection is currently sending message fragments, false otherwise.
 */
bool ws_conn_sending_fragments(ws_conn_t *c);


/**
* @param c Pointer to the WebSocket connection (`ws_conn_t`).
* @return  Pointer to ws_server_t that owns the websocket client
*/
ws_server_t *ws_conn_server(ws_conn_t *c);



/**
* get the connection's ctx (see `ws_conn_set_ctx` below)
* @param c Pointer to the WebSocket connection (`ws_conn_t`).
* @return  Pointer to the client ctx 
*/
void *ws_conn_ctx(ws_conn_t *c);



/**
* set the connection's ctx (see `ws_conn_get_ctx` above)
* @param c Pointer to the WebSocket connection (`ws_conn_t`).
* @param ctx ctx pointer
*/
void ws_conn_set_ctx(ws_conn_t *c, void *ctx);



/**
* @param s Pointer to the WebSocket connection (`ws_conn_t`).
* @return  Pointer to ws_server_t that owns the websocket client
*/
void *ws_server_ctx(ws_server_t *s);



/**
* @param s Pointer to the WebSocket connection (`ws_conn_t`).
* @param ctx ctx pointer
*/
void ws_server_set_ctx(ws_server_t *s, void *ctx);




/**
 * Starts the WebSocket server, Blocks the calling thread
 * 
 * @param s       Pointer to the WebSocket server (`ws_server_t`).
 * @param backlog accept queue size
 * @return        Non-zero on failure, indicating an error in starting the server.
 */
int ws_server_start(ws_server_t *s, int backlog);

/**
 * Retrieves the count of all open WebSocket connections on the server.
 *
 * @param s Pointer to the WebSocket server (`ws_server_t`).
 * @return  The number of open WebSocket connections.
 */
size_t ws_server_open_conns(ws_server_t *s);

/**
 * Determines if the current message is a binary message.
 *
 * This function is valid only when called during `on_ws_msg_fragment` or `on_ws_msg` callbacks.
 * It indicates whether the message being processed is binary.
 *
 * @param c Pointer to the WebSocket connection (`ws_conn_t`) handling the message.
 * @return  True if the current message is binary, false otherwise.
 */
bool ws_conn_msg_bin(ws_conn_t *c);

/**
 * Checks if compression is allowed for the given WebSocket connection.
 *
 * @param c Pointer to the WebSocket connection (`ws_conn_t`).
 * @return  True if compression is allowed, false otherwise.
 */
bool ws_conn_compression_allowed(ws_conn_t *c);


/**
 * Checks if the WebSocket server is currently pausing the acceptance of new connections.
 *
 * @param s Pointer to the WebSocket server (`ws_server_t`).
 * @return  True if the server is pausing new connections, false otherwise.
 */
bool ws_server_accept_paused(ws_server_t *s);



/**
 * Checks if reading from the given WebSocket connection is currently paused.
 *
 * @param c Pointer to the WebSocket connection (`ws_conn_t`).
 * @return  True if reading is paused, false otherwise.
 */
bool ws_conn_is_read_paused(ws_conn_t *c);


/**
 * Pauses reading data from the specified WebSocket connection.
 *
 * @param c Pointer to the WebSocket connection (`ws_conn_t`).
 */
void ws_conn_pause_read(ws_conn_t *c);


/**
 * Resumes reading data from a WebSocket connection that had reading paused.
 *
 * @param c Pointer to the WebSocket connection (`ws_conn_t`).
 */
void ws_conn_resume_reads(ws_conn_t *c);


/**
* Sets the read timeout for the connection if secs is zero read timeouts will be disabled
* @param c Pointer to the WebSocket connection (`ws_conn_t`).
* @param secs Number of seconds until timeout 
*/
void ws_conn_set_read_timeout(ws_conn_t *c, unsigned secs);



/**
* Sets the write timeout for the connection if secs is zero write timeouts will be disabled
* @param c Pointer to the WebSocket connection (`ws_conn_t`).
* @param secs Number of seconds until timeout 
*/
void ws_conn_set_write_timeout(ws_conn_t *c, unsigned secs);


/**
* @param c Pointer to the WebSocket connection (`ws_conn_t`).
* @return  Connection File Descriptor
*/
int ws_conn_fd(ws_conn_t *c);


int ws_server_shutdown(ws_server_t *s);


bool ws_server_shutting_down(ws_server_t *s);


typedef struct ws_poll_cb_ctx_t ws_poll_cb_ctx_t;


typedef void (*poll_ev_cb_t)(ws_server_t *s, ws_poll_cb_ctx_t *ctx, int ev);

/**
 * Structure representing the context for polling callbacks.
 */
typedef struct ws_poll_cb_ctx_t {
  poll_ev_cb_t cb;  /**< Callback function to be invoked when a polling event occurs. */
  void *ctx;        /**< User-defined context passed to the callback function. */
} ws_poll_cb_ctx_t;

/**
 * Creates an epoll instance for the given WebSocket server.
 *
 * @param s Pointer to the WebSocket server (`ws_server_t`).
 * @return  Non-zero on failure.
 */
int ws_epoll_create1(ws_server_t *s);

/**
 * Adds a file descriptor to the epoll instance for monitoring.
 *
 * @param s       Pointer to the WebSocket server (`ws_server_t`).
 * @param fd      File descriptor to be monitored.
 * @param cb_ctx  Pointer to the `ws_poll_cb_ctx_t` structure, containing the callback and context.
 * @param events  Epoll events to monitor (e.g., EPOLLIN, EPOLLOUT).
 * @return        Non-zero on failure.
 */
int ws_epoll_ctl_add(ws_server_t *s, int fd, ws_poll_cb_ctx_t *cb_ctx, int events);

/**
 * Removes a file descriptor from the epoll instance.
 *
 * @param s  Pointer to the WebSocket server (`ws_server_t`).
 * @param fd File descriptor to be removed from monitoring.
 * @return   Non-zero on failure.
 */
int ws_epoll_ctl_del(ws_server_t *s, int fd);

/**
 * Modifies the event subscription for a monitored file descriptor in the epoll instance.
 *
 * @param s       Pointer to the WebSocket server (`ws_server_t`).
 * @param fd      File descriptor with modified event subscription.
 * @param cb_ctx  Pointer to the `ws_poll_cb_ctx_t` structure, containing the updated callback and context.
 * @param events  New set of epoll events to monitor for the file descriptor.
 * @return        Non-zero on failure.
 */
int ws_epoll_ctl_mod(ws_server_t *s, int fd, ws_poll_cb_ctx_t *cb_ctx, int events);




typedef struct async_cb_ctx async_cb_ctx_t;

/**
 *
 * This callback is used in conjunction with `ws_server_sched_async` to execute custom
 * functions asynchronously within the context of the server's thread. 
 *
 * @param s    Pointer to the WebSocket server (`ws_server_t`) where the callback is executed.
 * @param ctx  Pointer to an `async_cb_ctx_t` structure as provided when scheduling (see `ws_server_sched_async` above)
 */
typedef void (*ws_server_async_cb_t)(ws_server_t *s, async_cb_ctx_t *ctx);


/**
 * Structure representing the context for an asynchronous callback.
 */
typedef struct async_cb_ctx {
  void *ctx;                /**< User-defined context passed to the callback function. */
  ws_server_async_cb_t cb;  /**< Callback function to be executed asynchronously. */
} async_cb_ctx;



/**
 * Schedules an asynchronous callback to be executed in the server's thread.
 * This function enables users to run a callback function as part of the server's event loop,
 * which is useful for thread-safe operations and synchronizing with the server's state.
 *
 * The `cb_info` structure must remain valid until the callback is called. After the callback
 * execution, it can either be discarded or reused to schedule another callback. 
 *
 * @param runner   Pointer to the WebSocket server (`ws_server_t`) where the callback is scheduled.
 * @param cb_info  Pointer to the `async_cb_ctx` structure containing the callback and its context.
 * @return         Non-zero on failure, indicating an error in scheduling the callback.
 */
int ws_server_sched_async(ws_server_t *runner, struct async_cb_ctx *cb_info);

/**
 * Returns the number of asynchronous callbacks currently queued in the server's event loop.
 * This can be used to monitor the backlog of scheduled tasks.
 *
 * @param runner Pointer to the WebSocket server (`ws_server_t`) to query.
 * @return       The number of pending asynchronous callbacks.
 */
size_t ws_server_pending_async_callbacks(ws_server_t *runner);


enum ws_conn_err {
    WS_ERR_READ = 990,

    WS_ERR_WRITE = 991,

    WS_ERR_BAD_FRAME = 992,

    WS_ERR_BAD_HANDSHAKE = 993,

    WS_ERR_READ_TIMEOUT = 994,

    WS_ERR_WRITE_TIMEOUT = 995,

    WS_ERR_RW_TIMEOUT = 996,

    WS_UNKNOWN_OPCODE = 997,

    WS_ERR_INFLATE = 998,

    WS_ERR_INVALID_UTF8 = 999,


    /**
    * Normal closure; the purpose for which the connection was
    * established has been fulfilled.
    */
    WS_CLOSE_NORMAL = 1000, 

    /**
    * Endpoint going away, such as a server shutting down or
    * a browser navigating away from a page.
    */
    WS_CLOSE_GOAWAY = 1001,

    /**
    * Protocol error encountered.
    */
    WS_CLOSE_PROTOCOL = 1002, 

    /**
    * Unsupported data; the client expects text but the server
    * sends binary data, for instance.
    */
    WS_CLOSE_UNSUPPORTED = 1003,

    /**
    * No status code was present in the close frame.
    */
    WS_CLOSE_NO_STATUS = 1005,

    /**
    * Connection closed abnormally, such as without sending/receiving a close frame.
    */
    WS_CLOSE_ABNORMAL = 1006,

    /**
    * Invalid data; for example, non-UTF-8 data within a text message.
    */
    WS_CLOSE_INVALID = 1007,

    /**
    * Policy violation.
    */
    WS_CLOSE_POLICY = 1008,

    /**
    * The message is too large for the server to process.
    */
    WS_CLOSE_TOO_LARGE = 1009,

    /**
    * Client ending connection due to expected server extension negotiation failure.
    */
    WS_CLOSE_EXTENSION = 1010,

    /**
    * An unexpected condition prevented the server from fulfilling the request.
    */
    WS_CLOSE_UNEXPECTED = 1011,

  };

/**
 * @param c Pointer to the WebSocket connection (`ws_conn_t`).
 * @return  A constant character pointer to the error message string.
 */
const char *ws_conn_strerror(ws_conn_t *c);




/**
 * Callback invoked after a WebSocket connection is successfully upgraded.
 *
 * Caller can attach connection-specific context or resources
 * to the connection (see `ws_conn_set_ctx` below). These resources can be utilized throughout the lifetime of
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
 * Optional callback invoked when a PING frame is received from the client.
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
 * Optional callback invoked when a PONG frame is received from the client.
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
 * Optional Callback invoked when a close frame is received from the client.
 *
 * NOTE: The 'reason' data is provided for use only within this callback.
 * If the caller needs to retain any part of the closure reason beyond this callback,
 * it must be copied to a separate buffer.
 *
 * @param ws_conn Pointer to the WebSocket connection (`ws_conn_t`).
 * @param reason  Pointer to the reason string for the closure. may be NULL
 * @param rlen    Length of the reason string. if zero reason will be NULL
 * @param code    Websocket close code.
 */
typedef void (*ws_close_cb_t)(ws_conn_t *ws_conn, void *reason, size_t rlen, uint16_t code);
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
 * Optional Callback invoked when the connection's send buffer is ready to accept more data.
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
 * 
 * NOTE: When this callback is not registered the default is to exit and print an error message to stderr 
 */
typedef void (*ws_err_cb_t)(ws_server_t *s, int err);

/**
 * Optional Callback invoked upon receiving a fragment of a WebSocket message.
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


/**
 * Optional Callback invoked when a new connection is accepted through `accept4(2)`.
 *
 * This callback allows the user to inspect and optionally pre-process the incoming
 * connection before the WebSocket handshake commences. Users can perform initial
 * validation or setup as needed.
 *
 * It's important for users to avoid closing the file descriptor (`fd`) directly.
 * Instead, to reject and close the connection, return -1. This ensures that the
 * library is aware that the connection is not proceeding and will handle the
 * closure and cleanup appropriately. Directly closing the `fd` can lead to
 * the library attempting to allocate resources for a socket that is in the
 * process of closing, which may cause erratic behavior.
 *
 *
 * @param s     Pointer to the WebSocket server (`ws_server_t`).
 * @param caddr Pointer to the client's address (`struct sockaddr_storage`).
 * @param fd    File descriptor for the incoming connection.
 *
 * @return An integer result code. A return code of -1 signifies that the
 *         connection should be rejected and closed immediately. The library
 *         will then close the `fd` and no further action is required from the
 *         user in regard to connection cleanup.
 */
typedef int (*ws_accept_cb_t)(ws_server_t *s, struct sockaddr_storage *caddr, int fd);


/**
 * Optional callback for errors during client connection acceptance.
 * an err value of zero indicates that the server reached max_conns open connections and no specific error occurred
 * caller may check `ws_server_accept_paused` to confirm. Accepting will automatically resume once there has been some
 * disconnects which brings the server below the max_conns limit. Accepting new connections will also be paused when 
 * err is EMFILE or ENFILE
 *
 * @param s   Pointer to the WebSocket server (`ws_server_t`).
 * @param err Error code (errno)
 */
typedef void (*ws_err_accept_cb_t)(ws_server_t *s, int err);

/**
 * Optional Callback invoked on receiving a raw WebSocket upgrade request.
 *
 * The callee is provided access to the request data and must craft a valid raw HTTP response
 * adhering to the standards for WebSocket upgrades.
 *
 * The 'accept_key' provided is pre-calculated and should be incorporated into the
 * response as part of the WebSocket handshake protocol. The user has the option
 * to either proceed with or reject the upgrade. In case of rejection, any appropriate
 * HTTP response can be sent back, and the user must set the 'reject' pointer to true (1).
 * In case of proceeding with the upgrade, there is no need to modify the 'reject' flag.
 *
 * @param c            Pointer to the WebSocket connection (`ws_conn_t`).
 * @param request      Pointer to the buffer containing the raw upgrade request data.
 * @param accept_key   Pre-calculated WebSocket accept key.
 * @param max_resp_len Maximum length allowable for the response.
 * @param resp_dst     Destination buffer for the raw HTTP response.
 * @param reject       Pointer to a boolean flag to indicate rejection of the upgrade. Set to true
 *                     if the upgrade is to be rejected, otherwise it should remain unchanged.
 *
 * @return The size of the response written to 'resp_dst'. The size must be within
 *         the bounds of 'max_resp_len' and properly formatted as an HTTP response.
 *         Returning 0, or a size exceeding 'max_resp_len', indicates a failure in processing,
 *         resulting in the handshake being aborted and a 500 status response being sent.
 */
typedef size_t (*ws_on_upgrade_req_cb_t)(ws_conn_t *c, char *request, const char *accept_key, size_t max_resp_len, char *resp_dst, bool *reject);



/**
 * Optional Callback invoked when a timeout occurs on a WebSocket connection.
 *
 * This callback is triggered when the WebSocket connection experiences a timeout.
 * The 'kind' parameter specifies the type of timeout that occurred, allowing
 * specific actions to be taken based on the timeout condition.
 *
 * @param ws_conn Pointer to the WebSocket connection (`ws_conn_t`) experiencing the timeout.
 * @param kind    Specifies the type of timeout. (see `enum ws_conn_err` above)

 * The callback provides an opportunity to handle these timeout conditions, such as
 * closing the connection or resetting the timeout. if this callback is not registered
 * the default action is to drop the connection.
 */
typedef void (*ws_on_timeout_t)(ws_conn_t *ws_conn, unsigned kind);


// Server parameter structure with optional callbacks for various WebSocket events.
struct ws_server_params {
  const char *addr;
  uint16_t port;
  bool verbose;              // logs server config to stdout 
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
  ws_on_timeout_t on_ws_conn_timeout;  // Callback for when a connection times out 
  ws_err_cb_t on_ws_err;               // Callback for when an internal error occurs.
  ws_accept_cb_t on_ws_accept;         // Callback for when a new connection has been accepted
  ws_err_accept_cb_t on_ws_accept_err; // Callback for when accept() fails.
  ws_on_upgrade_req_cb_t on_ws_upgrade_req; // Callback for when a Websocket upgrade request is received
  void *ctx;                                // attaches a pointer to the server
};

/**
 * Allocates and creates a new WebSocket server with the specified parameters.
 *
 * This function initializes a WebSocket server instance with the given configuration parameters.
 * It allocates the necessary resources for the server to operate.
 *
 * @param params Pointer to the structure containing server configuration parameters (`ws_server_params`).
 * @return       Pointer to the newly created WebSocket server (`ws_server_t`), or NULL on failure.
 */
ws_server_t *ws_server_create(struct ws_server_params *params);

// HTTP & Handshake Utils
#define WS_VERSION 13

#define SPACE 0x20
#define CRLF "\r\n"
#define CRLF_LEN (sizeof CRLF - 1)
#define CRLF2 "\r\n\r\n"
#define CRLF2_LEN (sizeof CRLF2 - 1)


#define GET_RQ "GET"
#define GET_RQ_LEN 3

#define SEC_WS_KEY_HDR "Sec-WebSocket-Key"

static const char switching_protocols[111] =
    "HTTP/1.1 101 Switching Protocols" CRLF
    "Upgrade: websocket" CRLF
    "Connection: Upgrade" CRLF
    "Server: cws" CRLF
    "Sec-WebSocket-Accept: ";

#define SWITCHING_PROTOCOLS_HDRS_LEN 110


static const char bad_request[80] =
    "HTTP/1.1 400 Bad Request" CRLF
    "Connection: close" CRLF
    "Server: cws" CRLF
    "Content-Length: 0" CRLF2;

#define BAD_REQUEST_LEN 79


static const char internal_server_error [90] = 
    "HTTP/1.1 500 Internal Server Error" CRLF
    "Server: cws" CRLF
    "Connection: close" CRLF
    "Content-Length: 0" CRLF2;

#define INTERNAL_SERVER_ERROR_LEN 89

#endif /* WS_PROTOCOL_PARSING23_H */

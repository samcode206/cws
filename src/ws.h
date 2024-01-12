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

#ifndef LWS_HDR_H
#define LWS_HDR_H

#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// #define WITH_COMPRESSION


#define OP_TXT 0x1
#define OP_BIN 0x2

#define OP_PING 0x9
#define OP_PONG 0xA


/**
 * WebSocket server connection structure.
 */
typedef struct ws_conn_t ws_conn_t;

/**
 * WebSocket server structure.
 */
typedef struct server ws_server_t;


struct http_header {
  char *name;
  char *val;
};

struct ws_conn_handshake {
  char *path;
  size_t header_count;
  bool per_msg_deflate_requested;
  char sec_websocket_accept[29];
  struct http_header headers[];
};

struct ws_conn_handshake_response {
  bool per_msg_deflate;
  size_t header_count;
  char *status;
  char *body;
  struct http_header *headers;
};


enum ws_send_status {
  /*
    Send/writev call failed, or the connection is closing/closed, or an unkown opcode was provided
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
 * Queues a message for sending over the WebSocket connection.
 * This can handle various message types including ping, pong, text, and binary,
 * based on the provided opcode. The queued message will be sent later, either via 'ws_conn_flush_pending'
 * or automatically by the event loop.
 * @param c            Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg          Pointer to the message data.
 * @param n            Size of the message in bytes.
 * @param opcode       Opcode specifying the type of message (OP_PING | OP_PONG | OP_TXT | OP_BIN).
 * @param hint_compress Boolean hint indicating whether to attempt compression of the message (if supported).
 * @return             enum ws_send_status
 */
enum ws_send_status ws_conn_put_msg(ws_conn_t *c, void *msg, size_t n, uint8_t opcode, bool hint_compress);

/**
 * Attempts to send a message immediately over the WebSocket connection.
 * Similar to 'ws_conn_put_msg', this function handles various types of messages determined by the opcode.
 * It tries to send the message directly, but if the socket is not in a writable state due to backpressure,
 * it falls back to queuing the message for later transmission.
 * @param c            Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg          Pointer to the message data.
 * @param n            Size of the message in bytes.
 * @param opcode       Opcode specifying the type of message (e.g., ping, pong, text, binary).
 * @param hint_compress Boolean hint indicating whether to attempt compression of the message.
 * @return             enum ws_send_status 
 */
enum ws_send_status ws_conn_send_msg(ws_conn_t *c, void *msg, size_t n, uint8_t opcode, bool hint_compress);

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
* Returns true if another frame MAY be soon ready for processing
*/
bool ws_conn_msg_ready(ws_conn_t *c);

/**
 * Checks if there is enough space in the connection's send buffer for a message of given length.
 * @param c       Pointer to the WebSocket connection (`ws_conn_t`).
 * @param msg_len Length of the message in bytes to check.
 * @return        True if there is enough space, false otherwise.
 */
bool ws_conn_can_put_msg(ws_conn_t *c, size_t msg_len);


/**
* Returns the number of bytes currently pending in the connection's send buffer.
*/
size_t ws_conn_pending_bytes(ws_conn_t *c);


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
 * Sends a handshake response over the WebSocket connection.
 *
 * This function is used to send a custom handshake response. It allows for detailed control
 * over the response headers and status code sent back to the client during the WebSocket 
 * handshake process. This is typically used to respond to an incoming WebSocket upgrade request.
 * to switch to the websocket protocol the response status should be set to WS_HANDSHAKE_STATUS_101
 * and the response headers should include the sec_websocket_accept header with the value from the request
 *
 * @param c    Pointer to the WebSocket connection (`ws_conn_t`) handling the handshake.
 * @param resp Pointer to a structure containing the handshake response details (`ws_conn_handshake_response`).
 * @return     enum ws_send_status indicating the result of the handshake response attempt.
 */
enum ws_send_status
ws_conn_handshake_reply(ws_conn_t *c, struct ws_conn_handshake_response *resp);


const struct http_header *ws_conn_handshake_header_find(struct ws_conn_handshake *hs,
                                          const char *name);

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
 * Checks if the current msg was compressed by the sending client
 * only valid when `on_ws_msg` for a OP_TXT or OP_BIN msg
 *
 * @param c Pointer to the WebSocket connection (`ws_conn_t`).
 * @return  True if the msg received was sent compressed and inflated by the server
 */
bool ws_conn_msg_compressed(ws_conn_t *c);


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


int ws_server_destroy(ws_server_t *s);


typedef struct ws_poll_cb_ctx_t ws_poll_cb_ctx_t;


typedef void (*poll_ev_cb_t)(ws_server_t *s, ws_poll_cb_ctx_t *ctx, unsigned int ev);

/**
 * Structure representing the context for polling callbacks.
 */
typedef struct ws_poll_cb_ctx_t {
  poll_ev_cb_t cb;  /**< Callback function to be invoked when a polling event occurs. */
  void *ctx;        /**< User-defined context passed to the callback function. */
} ws_poll_cb_ctx_t;


/**
 * @brief Sets the maximum number of bytes to read per read operation for the WebSocket server.
 *
 * This function allows you to set the maximum number of bytes to read per read operation for the WebSocket server.
 * The `max_per_read` parameter specifies the maximum number of bytes to read per read operation.
 * If the max_per_read value given is zero, the default value of will be used (buffer size)
 
 * @param s The WebSocket server object.
 * @param max_per_read The maximum number of bytes to read per read operation.
 */
void ws_server_set_max_per_read(ws_server_t *s, size_t max_per_read);



/**
*/
/**
 * Returns the number of active events in the current event loop iteration for the WebSocket server.
 *
 * @param s The WebSocket server.
 * @return The number of events.
 */
int ws_server_active_events(ws_server_t *s);




typedef void (*timeout_cb_t)(ws_server_t *s, void *ctx);


uint64_t ws_server_set_timeout(ws_server_t *s, struct timespec *tp,
                            void *ctx, timeout_cb_t cb);

void ws_server_cancel_timeout(ws_server_t *s, uint64_t timer_handle);


typedef void (*ws_server_deferred_cb_t)(ws_server_t *s, void *ctx);
/**
 * Schedules an asynchronous callback to be executed in the server's thread.
 * This function enables users to run a callback function as part of the server's event loop,
 * which is useful for thread-safe operations and synchronizing with the server's state.
 *
 *
 * @param runner   Pointer to the WebSocket server (`ws_server_t`) where the callback is scheduled.
 * @param cb_info  Pointer to the callback function to be executed (`ws_server_deferred_cb_t`).
 * @param ctx      Pointer to the user-defined context to be passed to the callback function.
 * @return         Non-zero on failure, indicating an error in scheduling the callback.
 */
int ws_server_sched_callback(ws_server_t *runner, ws_server_deferred_cb_t cb, void *ctx);


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
typedef void (*ws_disconnect_cb_t)(ws_conn_t *ws_conn, unsigned long err);

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
 * Optional Callback invoked when a new connection is accepted through `accept4(2)`.
 *
 * This callback allows the user to inspect and optionally pre-process the incoming
 * connection before the WebSocket handshake commences. Users can perform initial
 * validation or setup as needed.
 *
 * It's important for users to avoid closing the file descriptor (`fd`) directly.
 * Instead, to reject and close the connection, return -1. This ensures that the
 * library is aware that the connection is not proceeding and will handle the
 * closure and cleanup appropriately. 
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
 * Optional Callback invoked on receiving a WebSocket upgrade request.
 *
 * @param c                   Pointer to the WebSocket connection (`ws_conn_t`).
 * @param request             Pointer to the Request data (`struct ws_conn_handshake`)
 * @param accept_key_header   Pre-calculated WebSocket accept key header.
 *
 */
typedef void (*ws_handshake_cb_t)(ws_conn_t *c, struct ws_conn_handshake *hs);



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




typedef void (*ws_on_msg_cb_t)(ws_conn_t *c, void *msg, size_t n, uint8_t opcode);

// Server parameter structure with optional callbacks for various WebSocket events.
struct ws_server_params {
  const char *addr;
  uint16_t port;
  bool silent;              // logs server config to stdout if 0
  unsigned max_conns;        // Maximum connections the server is willing to accept.
  size_t max_header_count; // Maximum number of headers to parse in the upgrade equest. (defaults to 32 and max is 512)
                             // Defaults to the system's limit for maximum open file descriptors.
  size_t max_buffered_bytes; // Maximum amount of websocket payload data to buffer before the connection
                             // is dropped. Defaults to 16000 bytes.
  ws_on_msg_cb_t on_ws_msg;      // Callback for when a complete WebSocket frame is received.
  ws_open_cb_t on_ws_open;             // Callback for when a WebSocket connection is opened.
  ws_drain_cb_t on_ws_drain;           // Callback for when the send buffer has been drained.
  ws_disconnect_cb_t on_ws_disconnect; // Callback for after the connection has been closed.
  ws_on_timeout_t on_ws_conn_timeout;  // Callback for when a connection times out 
  ws_err_cb_t on_ws_err;               // Callback for when an internal error occurs.
  ws_accept_cb_t on_ws_accept;         // Callback for when a new connection has been accepted
  ws_err_accept_cb_t on_ws_accept_err; // Callback for when accept() fails.
  ws_handshake_cb_t on_ws_handshake; // Callback for when a Websocket handshake request is received
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


#define WS_HANDSHAKE_STATUS_101 "101 Switching Protocols"
#define WS_HANDSHAKE_STATUS_400 "400 Bad Request"
#define WS_HANDSHAKE_STATUS_404 "404 Not Found"
#define WS_HANDSHAKE_STATUS_500 "500 Internal Server Error"



#endif /* LWS_HDR_H */

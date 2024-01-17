/* We simply call the root header file "App.h", giving you uWS::App and
 * uWS::SSLApp */
#include "App.h"
#include <sys/signal.h>

/* This is a simple WebSocket echo server example.
 * You may compile it with "WITH_OPENSSL=1 make" or with "make" */

int main() {
  signal(SIGPIPE, SIG_IGN);
  /* ws->getUserData returns one of these */
  struct PerSocketData {
    /* Fill with user data */
  };

  /* Keep in mind that uWS::SSLApp({options}) is the same as uWS::App() when
   * compiled without SSL support. You may swap to using uWS:App() if you don't
   * need SSL */
  uWS::App()
      .ws<PerSocketData>(
          "/*",
          {/* Settings */
           .maxPayloadLength = 1024 * 512,
           .idleTimeout = 60,
           .maxBackpressure = 1024 * 512,
           .closeOnBackpressureLimit = false,
           .resetIdleTimeoutOnSend = false,
           .sendPingsAutomatically = false,
           /* Handlers */
           .upgrade = nullptr,
           .open =
               [](auto * /*ws*/) {

               },
           .message = [](auto *ws, std::string_view message,
                         uWS::OpCode opCode) { ws->send(message, opCode); },
           .drain =
               [](auto * /*ws*/) {
                 /* Check getBufferedAmount here */
               },
           .ping =
               [](auto * /*ws*/, std::string_view) {

               },
           .pong =
               [](auto * /*ws*/, std::string_view) {

               },
           .close =
               [](auto * /*ws*/, int /*code*/, std::string_view /*message*/) {

               }})
      .listen(9001,
              [](auto *listen_socket) {
                if (listen_socket) {
                  std::cout << "Listening on port " << 9001 << std::endl;
                }
              })
      .run();
}


// #include "App.h"
// #include <algorithm>
// #include <thread>
// #include <sys/signal.h>

// int main() {
//    signal(SIGPIPE, SIG_IGN);
//   /* ws->getUserData returns one of these */
//   struct PerSocketData {};

//   /* Simple echo websocket server, using multiple threads */
//   std::vector<std::thread *> threads(4);

//   std::transform(
//       threads.begin(), threads.end(), threads.begin(), [](std::thread * /*t*/) {
//         return new std::thread([]() {
//           /* Very simple WebSocket echo server */
//           uWS::App()
//               .ws<PerSocketData>(
//                   "/*",
//                   {/* Settings */
//                    .maxPayloadLength = 1024 * 512,
//                    .idleTimeout = 60,
//                    .maxBackpressure = 1024 * 512,
//                    .closeOnBackpressureLimit = false,
//                    .resetIdleTimeoutOnSend = false,
//                    .sendPingsAutomatically = true,
//                    /* Handlers */
//                    .upgrade = nullptr,
//                    .open =
//                        [](auto * /*ws*/) {

//                        },
//                    .message =
//                        [](auto *ws, std::string_view message,
//                           uWS::OpCode opCode) { ws->send(message, opCode); },
//                    .drain =
//                        [](auto * /*ws*/) {
//                          /* Check getBufferedAmount here */
//                        },
//                    .ping =
//                        [](auto * /*ws*/, std::string_view) {

//                        },
//                    .pong =
//                        [](auto * /*ws*/, std::string_view) {

//                        },
//                    .close =
//                        [](auto * /*ws*/, int /*code*/,
//                           std::string_view /*message*/) {

//                        }})
//               .listen(9001,
//                       [](auto *listen_socket) {
//                         if (listen_socket) {
//                           std::cout << " listening on port " << 9001
//                                     << std::endl;
//                         } else {
//                           std::cout << "Thread " << std::this_thread::get_id()
//                                     << " failed to listen on port 9001"
//                                     << std::endl;
//                         }
//                       })
//               .run();
//         });
//       });

//   std::for_each(threads.begin(), threads.end(),
//                 [](std::thread *t) { t->join(); });
// }


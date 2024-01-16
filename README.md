# cws

C Web Sockets (CWS) is a high-performance WebSocket server library written in C, designed for efficient handling of WebSocket connections and messaging in server-side applications requiring high throughput and low latency.

## Key Features
- **WebSocket Server Handling**: Efficient management of WebSocket server side operations.
- **Compliance and Reliability**: Fully compliant with WebSocket standards and passes the extensive Autobahn Test Suite.
- **Advanced Callbacks**: Extensive callback support for connection handling, message processing, and more.
- **Message Compression**: Optional per message deflate compression support.
- **Text and Binary Message Support**: Handles both text and binary WebSocket messages.
- **Synchronous and Asynchronous Messaging**: Supports both synchronous message sending and asynchronous queuing.
- **Customizable Connection Parameters**: Users can tailor server parameters to their needs, such as maximum connections, buffer sizing, and IO timeout settings.
- **IPv4 and IPv6 Support**: Compatible with both IPv4 and IPv6 networks.
- **Familiar Event-Loop Architecture**: Implements an event-loop based architecture, akin to Node.js, providing familiarity and ease of use while delivering orders of magnitude better performance.
- **High Resolution Timers**: Incorporates high resolution timers for precise timing operations.
- **Control**: Enables granular control over connection upgrades and supports efficient fragmented message streaming for large data sets.
- **Scalability**: Designed for scalability and can be easily configured to run on multiple threads, with simple yet powerful synchronization primitives
- **Easy Integration**: The library is elegantly designed as a single C source file and a single header file, ensuring ease of integration and straightforward use in various projects.
- **Determinist Memory Usage**: With minimal reliance on dynamic memory allocations when serving clients, the library ensures predictable and efficient memory management. This contributes to stable performance, optimized resource utilization & reuse, particularly suitable for performance critical services.
- **Cross Platform**: Compiled and tested on various 64bit platforms such as Linux, freeBSD & MacOS, (no windows support)

## Samples

Coming Soon (checkout examples in the meantime). 

## Benchmarks

Coming Soon.


## API Documentation

Coming Soon (ws.h should be referenced for now).



## License

cws is available under the [MIT license](https://opensource.org/licenses/MIT).

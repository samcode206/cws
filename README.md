# cws


CWS is a high-performance, Linux-only WebSocket server library written in C. It is designed for efficient handling of WebSocket connections and messaging, with a focus on server-side applications requiring high throughput and low latency.

## Key Features
- **Linux Optimization**: Tailored specifically for Linux environments for optimal performance.
- **WebSocket Server Handling**: Efficient management of WebSocket server side operations.
- **Compliance and Reliability**: Fully compliant with WebSocket standards and passes the extensive Autobahn Test Suite.
- **Advanced Callbacks**: Extensive callback support for connection handling, message processing, and more.
- **Message Compression**: Optional per message deflate compression support.
- **Text and Binary Message Support**: Handles both text and binary WebSocket messages.
- **Synchronous and Asynchronous Messaging**: Supports both synchronous message sending and asynchronous queuing.
- **Customizable Connection Parameters**: Users can tailor server parameters to their needs, such as maximum connections, buffer sizing, and IO timeout settings.
- **IPv4 and IPv6 Support**: Compatible with both IPv4 and IPv6 networks.
- **Familiar Event-Loop Architecture**: Implements an event-loop based architecture, akin to Node.js, providing familiarity and ease of use while delivering orders of magnitude better performance.
- **Control**: Enables granular control over connection upgrades and supports efficient fragmented message streaming for large data sets.
- **Scalability**: Designed for scalability and can be easily configured to run on multiple threads, with simple yet powerful synchronization primitives
- **Easy Integration**: The library is elegantly designed as a single C source file and a single header file, ensuring ease of integration and straightforward use in various projects.


## Samples

Coming Soon. 

## Benchmarks

Coming Soon.


## API Documentation

Coming Soon.


## Development Status

CWS is currently in an advanced stage of development and nearing completion. It is functional and incorporates a range of features designed for high-performance WebSocket handling, with ongoing work to add final touches, Documentation, Examples and Benchmarks against various Websocket Libraries in the same space.

### Current Version

At present, CWS has not been officially versioned. Versioning will be implemented upon reaching a stable and complete release. The first stable release will be marked as `Version 1.0`, signifying that the software has achieved API stability.

### Pre-1.0 Development

Until the arrival of `Version 1.0`, CWS is in a pre-release. This means that, while the library is largely functional, it may undergo significant changes, including additions and improvements, before the official `Version 1.0` release.


note that while CWS is currently stable for testing and development purposes, it is still under active development. Feedback, issue reporting, and suggestions for refinement and enhancement of the library are welcomed.

## License

cws is available under the [MIT license](https://opensource.org/licenses/MIT).

# C Web Sockets

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
- **Deterministic Memory Usage**: Between minimal reliance on dynamic memory allocations and taking advantage of large virtual address space on modern 64 bit hardware, the library ensures predictable and efficient memory management. This contributes to stable performance, optimized resource utilization & reuse, particularly suitable for performance critical services.
- **Cross Platform**: Compiled and tested on various 64bit platforms such as Linux, freeBSD & MacOS. (no windows support, any system with `kqueue` or `epoll` is likely supported)
- **Zero Dependencies**: in it's default configuration cws has zero dependencies on external libraries. Directly interacting with the underlying system calls to deliver all of it's features and performance.

## Samples

Coming Soon (checkout examples in the meantime). 

## Benchmarks

### Measuring IO performance: Streaming Clients

![4kb 8 connections payload](https://github.com/samcode206/cws/blob/master/bench/streaming/8_conns/4096%20bytes8.svg)


client sends data as fast as possible without waiting for a response before beginning transmission of the next frame. This test gives us a better picture as to how each server handles memory build up and how that affects it's performance. each server had one CPU core.

more can be found under `/bench` directory

### Measuring IO performance: classic echo benchmark 

![512 byte payload](https://github.com/samcode206/cws/blob/v1.0.0/bench/echo/512%20bytes1000.svg)

these tests were conducted on `11th Gen Intel(R) Core(TM) i9-11900K @ 3.50GHz` running a standard installation of debian 12.
all servers were running on 4 cpu cores


## API Documentation

Coming Soon (ws.h should be referenced for now).



## License

cws is available under the [MIT license](https://opensource.org/licenses/MIT).

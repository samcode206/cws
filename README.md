# cws

## Introduction
`cws` is a high-performance, Linux-only WebSocket server library written in C. It is designed for efficient handling of WebSocket connections and messaging, with a focus on server-side applications requiring high throughput and low latency.

## Key Features
- **Linux Optimization**: Tailored specifically for Linux environments for optimal performance.
- **WebSocket Server Handling**: Efficient management of WebSocket server side operations.
- **Compliance and Reliability**: Fully compliant with WebSocket standards and passes the extensive Autobahn Test Suite.
- **Advanced Callbacks**: Extensive callback support for connection handling, message processing, and more.
- **Message Compression**: Optional per message deflate compression support.
- **Text and Binary Message Support**: Handles both text and binary WebSocket messages.
- **Synchronous and Asynchronous Messaging**: Supports both synchronous message sending and asynchronous queuing.
- **Customizable Connection Parameters**: sers can tailor server parameters to their needs, such as maximum connections, buffer sizing, and IO timeout settings.
- **IPv4 and IPv6 Support**: Compatible with both IPv4 and IPv6 networks.
- **Familiar Event-Loop Architecture**: Implements an event-loop based architecture, akin to Node.js, providing familiarity and ease of use while delivering orders of magnitude better performance.
- **Control**: Enables granular control over connection upgrades and supports efficient fragmented message streaming for large data sets.
- **Scalability**: Designed for scalability and can be easily configured to run on multiple threads, with simple yet powerful synchronization primitives

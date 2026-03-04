# UDP Multicast Chat Room

A high-performance chat room server designed to automatically upgrade to UDP multicast for efficient message broadcasting, trading reliability for speed and scalability.

## Project Overview

This project implements a chat room system where clients connect over TCP and exchange newline-delimited JSON messages. The long-term goal is to create a scalable chat infrastructure that can handle many users efficiently by using UDP multicast for message distribution, despite the inherent trade-off in reliability compared to TCP.

## Project Goals

### Primary Aim
Create chat rooms that **automatically upgrade to UDP multicast** to broadcast updates to users. This approach prioritizes:
- **Speed**: Low-latency message delivery
- **Scalability**: Efficient broadcasting to multiple clients
- **Resource Efficiency**: Reduced server load compared to TCP-based message distribution

The system accepts the trade-off of reduced reliability (UDP's connectionless nature) for improved performance and scalability in chat room scenarios.

## Current Implementation Status

### Completed Features

1. **Multithreaded TCP Server Infrastructure**
   - TCP acceptor running on port 4040 (configurable)
   - Asynchronous I/O using Boost.Cobalt coroutines (C++26)
   - Thread pool execution: defaults to one thread per hardware core, configurable via `-j<N>`
   - Graceful shutdown on SIGINT / SIGTERM

2. **Chat Room Management**
   - Multiple named chat rooms, each with a unique numeric ID
   - Rooms created on demand via the `csub` message type
   - Thread-safe connection management with mutex protection

3. **Custom Socket Protocol**
   - Custom protocol over raw TCP sockets (not HTTP)
   - Message framing: newline-terminated JSON (`\n` delimiter)
   - JSON-based message structure with type-based routing

4. **Client Message Types**
   - `sub`: Subscribe to an existing chat room by numeric group ID
   - `csub`: Create a new named room and subscribe to it in one step
   - `desub`: End the client session (disconnect)
   - `msg`: Send a message to a subscribed room

5. **Asynchronous Message Broadcasting**
   - Boost.Cobalt channel-based message distribution to subscribers
   - Non-blocking send operations via `wait_group`
   - Automatic cleanup of disconnected clients (`weak_ptr` management)

### In Progress

1. **UDP Multicast Infrastructure**
   - `UDP_Server` class defined but not yet implemented
   - Placeholder for multicast logic

## Technologies & Libraries

### Programming Language
- **C++26**: Leveraging the latest C++ standard for modern features

### Core Libraries

1. **Boost.Cobalt** (Primary Async / Coroutine Library)
   - `boost::cobalt::task` / `boost::cobalt::promise` for coroutines
   - `boost::cobalt::channel` for inter-coroutine communication
   - `boost::cobalt::wait_group` for concurrent fan-out
   - `boost::cobalt::race` for racing multiple async operations

2. **Boost.Asio** (I/O & Networking)
   - Async TCP socket management
   - `io_context` + thread pool

3. **Boost.JSON**
   - JSON parsing and serialization
   - Message structure encoding/decoding

### Build System
- **CMake** (version 3.5+)
- C++26 standard (`set(CMAKE_CXX_STANDARD 26)`)
- Boost libraries installed at `/usr/local/boost/`

### Development Environment
- Compile commands export for IDE integration (`CMAKE_EXPORT_COMPILE_COMMANDS ON`)

## Current Infrastructure

### Architecture

```
┌──────────────────────────────────────────────────────┐
│              Server (Port 4040)                      │
├──────────────────────────────────────────────────────┤
│                                                      │
│  ┌──────────────────────────────────────────────┐   │
│  │  TCP Acceptor (Boost.Cobalt + Boost.Asio)    │   │
│  │  - Async connection acceptance               │   │
│  │  - Coroutine-based per-connection handling   │   │
│  │  - Thread pool (hardware_concurrency threads)│   │
│  └──────────────────────────────────────────────┘   │
│                                                      │
│  ┌──────────────────────────────────────────────┐   │
│  │  Chat Rooms (std::list<TCPChatRoom>)         │   │
│  │  - Named rooms with numeric IDs              │   │
│  │  - Thread-safe connection vectors            │   │
│  │  - Channel-based broadcasting               │   │
│  └──────────────────────────────────────────────┘   │
│                                                      │
│  ┌──────────────────────────────────────────────┐   │
│  │  UDP Multicast Server (Future)               │   │
│  │  - Not yet implemented                       │   │
│  └──────────────────────────────────────────────┘   │
│                                                      │
└──────────────────────────────────────────────────────┘
         │                    │
         │ TCP Connections    │
         │                    │
    ┌────▼────┐         ┌────▼────┐
    │ Client1 │   ...   │ ClientN │
    └─────────┘         └─────────┘
```

### Protocol Design

The server uses a custom text protocol over TCP: each message is a **newline-terminated JSON object** (`\n` delimiter). There is no binary size prefix.

**Message Frame:**
```
<json_payload>\n
```

**Client → Server Message Types:**

1. **Subscribe to an existing room** (`sub`)
```json
{"type": "sub", "grp": 12345}
```
- `grp`: numeric ID of an existing room

2. **Create a new room and subscribe** (`csub`)
```json
{"type": "csub", "grp_name": "my-room"}
```
- `grp_name`: string name for the new room
- The server creates the room, assigns it a numeric ID, and immediately subscribes the client

3. **Send a message to a room** (`msg`)
```json
{"type": "msg", "grp": 12345, "msg": "Hello, world!"}
```
- `grp`: numeric ID of a room the client is subscribed to
- `msg`: message content string
- The server fans the message out to all other subscribers in the room

4. **End session** (`desub`)
```json
{"type": "desub"}
```
- Closes the client's session immediately

**Server → Client Broadcast:**
```json
{"type": "message", "data": "<text>", "grp_name": "<room-name>"}
```

### Key Components

1. **Server Class** (`server.hpp`, `server.cpp`)
   - Main server orchestration
   - TCP acceptor management
   - Chat room lifecycle management
   - Message routing and broadcasting

2. **TCPChatRoom Struct**
   - Room name (`std::string`)
   - Room ID (`uint64_t`)
   - Thread-safe connection vector (`weak_ptr` to `Channel<std::string>`)
   - Mutex for concurrent access protection

3. **UDP_Server Class** (`udp_server.hpp`, `udp_server.cpp`)
   - Placeholder for future UDP multicast implementation

4. **Main Entry Point** (`main.cpp`)
   - Server initialization (port 4040, max 10 rooms)
   - Optional `-j<N>` flag to set the thread-pool size

## Building the Project

### Prerequisites
- CMake 3.5+
- C++26-compatible compiler (GCC 14+, Clang 16+)
- Boost libraries (1.84+) installed at `/usr/local/boost/`
  - Boost.Asio
  - Boost.Cobalt
  - Boost.JSON

### Build Steps

```bash
# Create build directory
mkdir build && cd build

# Configure with CMake
cmake ..

# Build the project
cmake --build .
```

## Running the Server

```bash
# Start with default thread count (hardware_concurrency)
./udp_multicast_chat_room

# Start with a specific number of worker threads
./udp_multicast_chat_room -j4
```

The server will start listening on **port 4040**. Use SIGINT (`Ctrl-C`) or SIGTERM to shut it down gracefully.

## Future Goals

### 🎯 Planned Features

1. **UDP Multicast Implementation**
   - Implement automatic upgrade from TCP to UDP multicast
   - Multicast group management
   - Hybrid TCP/UDP protocol switching
   - Reliability layer on top of UDP (optional)

2. **Authentication System**
   - User authentication before room access
   - Token-based session management
   - Secure credential handling
   - Authorization for room creation/access

3. **QUIC Integration**
   - QUIC protocol support with UDP multicast
   - Improved reliability over standard UDP
   - Built-in encryption and security
   - 0-RTT connection establishment

4. **Enhanced Features**
   - Room deletion APIs
   - Private/public room modes
   - User presence notifications
   - Message history and persistence
   - Rate limiting and spam prevention

## Protocol Design Philosophy

This project deliberately uses a **custom protocol over raw sockets** rather than HTTP/WebSocket for several reasons:

1. **Performance**: Reduced overhead compared to HTTP
2. **Control**: Full control over message framing and protocol semantics
3. **Multicast Ready**: Easier integration with UDP multicast (no WebSocket overhead)
4. **Learning**: Educational value in understanding low-level networking

**Note**: This is an active development project. The UDP multicast functionality is the next major milestone.

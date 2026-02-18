# UDP Multicast Chat Room

A high-performance chat room server designed to automatically upgrade to UDP multicast for efficient message broadcasting, trading reliability for speed and scalability.

## Project Overview

This project implements a chat room system that leverages UDP multicast to broadcast messages to multiple users simultaneously. The goal is to create a scalable chat infrastructure that can handle many users efficiently by using UDP multicast for message distribution, despite the inherent trade-off in reliability compared to TCP.

## Project Goals

### Primary Aim
Create chat rooms that **automatically upgrade to UDP multicast** to broadcast updates to users. This approach prioritizes:
- **Speed**: Low-latency message delivery
- **Scalability**: Efficient broadcasting to multiple clients
- **Resource Efficiency**: Reduced server load compared to TCP-based message distribution

The system accepts the trade-off of reduced reliability (UDP's connectionless nature) for improved performance and scalability in chat room scenarios.

## Current Implementation Status

### Completed Features

1. **TCP-based Server Infrastructure**
   - TCP acceptor running on port 4040 (configurable)
   - Asynchronous I/O using coroutines (C++26)
   - Non-blocking connection handling

2. **Chat Room Management**
   - Multiple chat room support with unique room IDs
   - Room limit enforcement (configurable max rooms)
   - Thread-safe connection management with mutex protection

3. **Custom Socket Protocol**
   - Custom protocol over raw sockets (not HTTP)
   - Message format: `<size> <JSON_payload>`
   - JSON-based message structure with type-based routing

4. **Client Message Types**
   - `sub`: Subscribe to a chat room by group ID
   - `desub`: Unsubscribe from a chat room
   - `message`: Send messages within a room

5. **Asynchronous Message Broadcasting**
   - Channel-based message distribution to subscribers
   - Non-blocking send operations
   - Automatic cleanup of disconnected clients (weak_ptr management)

### In Progress

1. **UDP Multicast Infrastructure**
   - UDP_Server class defined but not yet implemented
   - Placeholder for multicast logic

## Technologies & Libraries

### Programming Language
- **C++26**: Leveraging the latest C++ standard for modern features

### Core Libraries

1. **Boost.Asio** (Primary Networking Library)
   - Asynchronous I/O operations
   - TCP socket management
   - Coroutine support for `co_await` patterns
   - Experimental channels for inter-coroutine communication

2. **Boost.JSON**
   - JSON parsing and serialization
   - Message structure encoding/decoding

### Build System
- **CMake** (version 3.1+)
- Custom compiler flags for C++26 standard
- Boost library integration

### Development Environment
- Homebrew-based Boost installation (macOS/Linux)
- Compile commands export for IDE integration

## Current Infrastructure

### Architecture

```
┌─────────────────────────────────────────────────┐
│                 Server (Port 4040)              │
├─────────────────────────────────────────────────┤
│                                                 │
│  ┌─────────────────────────────────────────┐   │
│  │     TCP Acceptor (Boost.Asio)           │   │
│  │  - Async connection acceptance           │   │
│  │  - Coroutine-based handling              │   │
│  └─────────────────────────────────────────┘   │
│                                                 │
│  ┌─────────────────────────────────────────┐   │
│  │    Chat Rooms (std::list)               │   │
│  │  - Room ID management                    │   │
│  │  - Thread-safe connection vectors        │   │
│  │  - Channel-based broadcasting            │   │
│  └─────────────────────────────────────────┘   │
│                                                 │
│  ┌─────────────────────────────────────────┐   │
│  │   UDP Multicast Server (Future)         │   │
│  │  - Not yet implemented                   │   │
│  └─────────────────────────────────────────┘   │
│                                                 │
└─────────────────────────────────────────────────┘
         │                    │
         │ TCP Connections    │
         │                    │
    ┌────▼────┐         ┌────▼────┐
    │ Client1 │   ...   │ ClientN │
    └─────────┘         └─────────┘
```

### Protocol Design

The server uses a custom binary protocol over TCP sockets:

**Message Format:**
```
<size: uint64_t> <space> <json_payload>
```

**JSON Message Types:**

1. **Subscribe to Room:**
```json
{
  "type": "sub",
  "grp": 12345
}
```

2. **Unsubscribe:**
```json
{
  "type": "desub"
}
```

3. **Send Message:**
```json
{
  "type": "message",
  "data": "Hello, world!"
}
```

4. **Broadcast (Server to Client):**
```json
{
  "type": "message",
  "data": "User message content"
}
```

### Key Components

1. **Server Class** (`server.hpp`, `server.cpp`)
   - Main server orchestration
   - TCP acceptor management
   - Chat room lifecycle management
   - Message routing and broadcasting

2. **TCPChatRoom Struct**
   - Room ID (`uint64_t`)
   - Thread-safe connection vector (weak_ptr to channels)
   - Mutex for concurrent access protection

3. **UDP_Server Class** (`udp_server.hpp`, `udp_server.cpp`)
   - Placeholder for future UDP multicast implementation

4. **Main Entry Point** (`main.cpp`)
   - Server initialization (port 4040, max 10 rooms)
   - Server execution

## Building the Project

### Prerequisites
- CMake 3.1+
- C++26-compatible compiler (GCC 14+, Clang 16+, or MSVC 2022+)
- Boost libraries (1.80+)
  - Boost.Asio
  - Boost.JSON

### Build Steps

```bash
# Create build directory
mkdir build && cd build

# Configure with CMake
cmake ..

# Build the project
cmake --build .

# Run the server
./udp_multicast_chat_room
```

### macOS (Homebrew)
```bash
# Install Boost
brew install boost

# Build
mkdir build && cd build
cmake ..
make
```

## Running the Server

```bash
./udp_multicast_chat_room
```

The server will start listening on **port 4040** with a maximum of **10 chat rooms**.

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
   - Room creation and deletion APIs
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

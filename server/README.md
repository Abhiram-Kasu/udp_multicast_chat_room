# UDP Multicast Chat Room

A high-performance chat room server designed to automatically upgrade to UDP multicast for efficient message broadcasting, trading reliability for speed and scalability.

## Table of Contents

1. [Overview](#1-overview)
2. [Getting Started](#2-getting-started)
   - 2.1 [Prerequisites](#21-prerequisites)
   - 2.2 [Building](#22-building)
   - 2.3 [Running](#23-running)
3. [Architecture](#3-architecture)
   - 3.1 [High-Level Diagram](#31-high-level-diagram)
   - 3.2 [Source Layout](#32-source-layout)
   - 3.3 [Key Types](#33-key-types)
4. [Protocol Reference](#4-protocol-reference)
   - 4.1 [Message Framing](#41-message-framing)
   - 4.2 [Client → Server Messages](#42-client--server-messages)
   - 4.3 [Server → Client Messages](#43-server--client-messages)
5. [Technologies & Libraries](#5-technologies--libraries)
   - 5.1 [Language](#51-language)
   - 5.2 [Core Libraries](#52-core-libraries)
   - 5.3 [Build System](#53-build-system)
6. [Implementation Status](#6-implementation-status)
   - 6.1 [Completed](#61-completed)
   - 6.2 [In Progress](#62-in-progress)
7. [Future Goals](#7-future-goals)
8. [Design Philosophy](#8-design-philosophy)

---

## 1. Overview

This project implements a chat room system where clients connect over TCP and exchange newline-delimited JSON messages. The long-term goal is to upgrade rooms to UDP multicast for efficient message distribution, trading some reliability for speed and scalability.

## 2. Getting Started

### 2.1 Prerequisites

| Dependency | Minimum Version | Notes |
|---|---|---|
| CMake | 3.5 | |
| C++ compiler | GCC 14+ / Clang 16+ | Must support C++26 |
| Boost | 1.84+ | Installed at `/usr/local/boost/` |

Required Boost components: **Asio**, **Cobalt**, **JSON**.

### 2.2 Building

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

### 2.3 Running

```bash
# Default thread count (std::thread::hardware_concurrency)
./udp_multicast_chat_room

# Explicit thread-pool size
./udp_multicast_chat_room -j4
```

The server listens on **port 4040**. Send SIGINT (`Ctrl-C`) or SIGTERM to shut it down gracefully.

## 3. Architecture

### 3.1 High-Level Diagram

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

### 3.2 Source Layout

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Build configuration (C++26, Boost linkage) |
| `src/main.cpp` | Entry point — parses `-j<N>`, constructs `Server{4040, 10}`, calls `serve()` |
| `src/server.hpp` | `Server` class, `TCPChatRoom` struct, type aliases, `Channel` template |
| `src/server.cpp` | All server logic — accept loop, message parsing, routing, broadcasting |
| `src/udp_server.hpp` | `UDP_Server` placeholder struct |
| `src/udp_server.cpp` | `UDP_Server` placeholder (includes header only) |

### 3.3 Key Types

1. **`Server`** — Main server class.
   - Owns the TCP acceptor, the chat-room list, and an (unused) `UDP_Server`.
   - `serve(size_t thread_count)` creates a `boost::asio::io_context`, spawns the coroutine tree via `boost::cobalt::spawn`, then runs the context across a `std::jthread` pool.
   - Handles SIGINT / SIGTERM through `boost::cobalt::io::signal_set` + `boost::cobalt::race`.

2. **`TCPChatRoom`** — Represents a single chat room.
   - `std::string name` — human-readable room name.
   - `uint64_t id` — unique numeric room identifier.
   - `connection_list connections` — `std::vector<std::weak_ptr<Channel<std::string>>>` of subscriber channels.
   - `std::mutex mutex` — guards the connection list.

3. **`Channel<std::string>`** — `boost::cobalt::channel<std::string>` used for inter-coroutine message passing between the broadcasting coroutine and each subscriber's listener coroutine.

4. **`UDP_Server`** — Empty placeholder struct for future multicast work.

## 4. Protocol Reference

### 4.1 Message Framing

Each message is a **newline-terminated JSON object** (`\n` delimiter). There is no binary length prefix.

```
<json_object>\n
```

### 4.2 Client → Server Messages

#### 4.2.1 `sub` — Subscribe to an existing room

```json
{"type": "sub", "grp": <room_id>}
```

| Field | Type | Description |
|---|---|---|
| `type` | `string` | `"sub"` |
| `grp` | `uint64` | Numeric ID of an existing room |

The server looks up the room, creates a `Channel` for this client, and starts a listener coroutine. Returns an error string if the room does not exist or the client is already subscribed.

#### 4.2.2 `csub` — Create a new room and subscribe

```json
{"type": "csub", "grp_name": "<room_name>"}
```

| Field | Type | Description |
|---|---|---|
| `type` | `string` | `"csub"` |
| `grp_name` | `string` | Human-readable name for the new room |

The server creates the room (assigning the next numeric ID), subscribes the client, and starts a listener coroutine.

#### 4.2.3 `msg` — Send a message to a room

```json
{"type": "msg", "grp": <room_id>, "msg": "<text>"}
```

| Field | Type | Description |
|---|---|---|
| `type` | `string` | `"msg"` |
| `grp` | `uint64` | Numeric ID of a room the client is subscribed to |
| `msg` | `string` | Message content |

The server verifies the client is subscribed, then fans the message out to every other subscriber's channel in the room via a `boost::cobalt::wait_group`.

#### 4.2.4 `desub` — End session

```json
{"type": "desub"}
```

| Field | Type | Description |
|---|---|---|
| `type` | `string` | `"desub"` |

Immediately closes the client's TCP session (the coroutine returns).

### 4.3 Server → Client Messages

#### 4.3.1 Broadcast

When a subscriber's listener coroutine reads from its channel, the server sends:

```json
{"type": "message", "data": "<channel_payload>", "grp_name": "<room_name>"}
```

| Field | Type | Description |
|---|---|---|
| `type` | `string` | `"message"` |
| `data` | `string` | The channel payload — a serialized JSON string `{"grp":"<room_name>","message":"<text>"}` |
| `grp_name` | `string` | Room name |

> **Note:** The `data` field contains a JSON-encoded string (the serialized inner object), not the raw message text.

#### 4.3.2 Error responses

On invalid input (bad JSON, unknown type, missing fields, not subscribed, etc.) the server sends a plain-text error string directly over the socket (not JSON-wrapped).

## 5. Technologies & Libraries

### 5.1 Language

- **C++26** — uses `std::print` / `std::println`, `std::format`, `std::ranges`, coroutines, `std::jthread`, etc.

### 5.2 Core Libraries

1. **Boost.Cobalt** — Primary async / coroutine library.
   - `boost::cobalt::task`, `boost::cobalt::promise` — coroutine types.
   - `boost::cobalt::channel` — inter-coroutine communication.
   - `boost::cobalt::wait_group` — concurrent fan-out.
   - `boost::cobalt::race` — race multiple async operations.
   - `boost::cobalt::spawn` — launch a coroutine tree on an `io_context`.
   - `boost::cobalt::io::signal_set` — async signal handling.

2. **Boost.Asio** — I/O & networking.
   - `boost::asio::io_context` — event loop.
   - `tcp::acceptor`, `tcp::socket` — TCP connection management.
   - `boost::asio::async_read_until` — newline-delimited reads.

3. **Boost.JSON** — JSON parsing and serialization.
   - `boost::json::parse` — parse incoming messages.
   - `boost::json::serialize` / `boost::json::object` — build outgoing messages.

### 5.3 Build System

- **CMake 3.5+**
- C++26 standard — `set(CMAKE_CXX_STANDARD 26)`
- Boost include/lib from `/usr/local/boost/`
- Links `boost_json` and `boost_cobalt`
- `CMAKE_EXPORT_COMPILE_COMMANDS ON` for IDE integration

## 6. Implementation Status

### 6.1 Completed

1. **Multithreaded TCP server** — async accept loop, coroutine-per-connection, `std::jthread` pool (default `hardware_concurrency`, override with `-j<N>`).
2. **Graceful shutdown** — SIGINT / SIGTERM via `boost::cobalt::io::signal_set` + `boost::cobalt::race`.
3. **Named chat rooms** — created on demand (`csub`), looked up by numeric ID (`sub`, `msg`), stored in a `std::list<TCPChatRoom>` guarded by `std::mutex`.
4. **Newline-delimited JSON protocol** — four client message types (`sub`, `csub`, `msg`, `desub`) and one server broadcast type.
5. **Channel-based broadcasting** — per-subscriber `boost::cobalt::channel<std::string>`, fan-out via `boost::cobalt::wait_group`, stale subscriber cleanup with `weak_ptr`.

### 6.2 In Progress

1. **UDP multicast** — `UDP_Server` struct exists but is empty; no multicast logic yet.

## 7. Future Goals

1. **UDP Multicast** — automatic upgrade from TCP to multicast, group management, hybrid TCP/UDP switching, optional reliability layer.
2. **Authentication** — user auth before room access, token-based sessions, secure credential handling, authorization for room creation.
3. **QUIC Integration** — QUIC protocol support alongside UDP multicast, built-in encryption, 0-RTT connection establishment.
4. **Enhanced Features** — room deletion, private/public modes, presence notifications, message history, rate limiting.

## 8. Design Philosophy

This project deliberately uses a **custom protocol over raw sockets** rather than HTTP/WebSocket:

1. **Performance** — minimal overhead compared to HTTP.
2. **Control** — full control over message framing and protocol semantics.
3. **Multicast Ready** — no WebSocket layer to bypass when upgrading to UDP multicast.
4. **Learning** — educational value in understanding low-level networking.

---

**Note:** This is an active development project. UDP multicast is the next major milestone.

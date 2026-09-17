# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ChatServer is a high-concurrency chat server built with C++20 coroutines and Boost.Asio. It has been migrated from muduo (callback-based) to a fully async coroutine architecture. The server supports user authentication, one-to-one chat, group chat, friend management, and Redis-based cross-server message delivery.

## Build Commands

```bash
# Build the project (requires CMake 3.16+, C++20 compiler)
cmake -B build -S .
cmake --build build

# Output binary location: bin/ChatServer
```

## Running the Server

```bash
# Start server (requires MySQL and Redis running)
./bin/ChatServer <ip> <port>

# Example:
./bin/ChatServer 127.0.0.1 6000
```

Prerequisites:
- MySQL server with database `chat` (configure credentials in `src/server/main.cpp`)
- Redis server at 127.0.0.1:6379
- OpenCV 4 (for image processing)

## Architecture

### Layer Structure

```
main.cpp
  └── bootstrap() coroutine → init DB pool + Redis + start ChatServer
        └── ChatServer
              └── do_accept() coroutine → creates Session per connection
                    └── Session::read_loop() coroutine → JSON parsing → handler dispatch
                          └── ChatService handler coroutines → Model layer → DB/Redis
```

### Key Components

| Component | Purpose |
|-----------|---------|
| `Session` | Per-connection coroutine handling reads/writes, brace-matching JSON extraction |
| `ChatServer` | Accept loop coroutine spawning Sessions |
| `ChatService` | Singleton business logic dispatcher with 16+ handler coroutines |
| `AsyncConnectionPool` | Async MySQL connection pool (boost::mysql), co_await-based |
| `Redis` | Hybrid: sync publish + async subscribe (hiredis + asio integration) |

### Thread Model

Single-threaded `io_context` runs all coroutines. No worker thread pool needed—all I/O is async via co_await.

### Data Flow

1. Client sends JSON with `msgid` field
2. `Session::read_loop()` extracts complete JSON via brace matching
3. Handler coroutine looked up via `ChatService::getHandler(msgid)`
4. Handler co_awaits Model layer DB operations
5. Response sent via `Session::send()` (thread-safe via post)

## Message Types

Defined in `include/public.hpp` (`EnMsgType` enum): login, register, OTOMsg, group chat, friend/group management, history, image requests.

## Key Patterns

### Async Database Operations

```cpp
// All Model methods are awaitable
auto user = co_await _userModel.query(name);
co_await _userModel.updateState(user);
```

### Connection Pool Usage

```cpp
auto guard = co_await AsyncConnectionGuard::create();
auto& conn = guard->connection();
auto stmt = co_await conn.async_prepare_statement("SELECT ...", use_awaitable);
```

### Session Send (Thread-Safe)

```cpp
session->send(response.dump());  // Posts to io_context, serializes writes
```

## Dependencies

- Boost 1.82+ (Asio, mysql)
- hiredis (Redis client)
- OpenCV 4
- nlohmann/json (in thirdparty/)
- MySQL 8.0+ server
- Redis server

## Code Style

Google C++ Style Guide with project conventions:
- Handler functions: `asio::awaitable<void> handler(Session::Ptr, json, Timestamp)`
- Model methods: `asio::awaitable<T> method(args)`
- Member variables: `trailing_underscore_`
- Coroutines for all I/O operations
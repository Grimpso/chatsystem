# Multi-Threaded TCP Chat Server

A cross-platform, multi-threaded TCP chat server written in **pure C** supporting concurrent clients with a custom binary message-framing protocol. Designed to demonstrate low-level socket programming, POSIX threading, and embedded-style protocol design — all in a single codebase that compiles on both **Windows** and **Linux** without any code changes.

---

## Table of Contents

- [Features](#features)
- [Architecture](#architecture)
- [Message Frame Protocol](#message-frame-protocol)
- [Project Structure](#project-structure)
- [Prerequisites](#prerequisites)
- [Build Instructions](#build-instructions)
  - [Windows (MinGW / GCC)](#windows-mingw--gcc)
  - [Linux (GCC)](#linux-gcc)
- [How to Run](#how-to-run)
- [Commands](#commands)
- [Platform Differences](#platform-differences)
- [Key Concepts](#key-concepts)
- [Resume Highlights](#resume-highlights)

---

## Features

- Concurrent multi-client support (up to 10 clients by default)
- Custom binary **message-framing protocol** with magic header validation
- Thread-per-client architecture using **POSIX threads** (Linux) and **Win32 threads** (Windows)
- **Mutex-protected** shared client table for thread-safe access
- Reliable `send_all()` loop to handle partial TCP sends
- Join/leave announcements broadcast to all connected clients
- `/quit` command for graceful client disconnection
- Single source — **no code changes needed** between Windows and Linux builds

---

## Architecture

```
                        ┌─────────────────────────────┐
                        │         SERVER               │
                        │                              │
   Client A ───TCP───►  │  accept()  ──► thread_A      │
   Client B ───TCP───►  │  accept()  ──► thread_B      │
   Client C ───TCP───►  │  accept()  ──► thread_C      │
                        │                │             │
                        │         mutex-protected      │
                        │         client table         │
                        │    [fd | name | active]      │
                        │                │             │
                        │         broadcast()          │
                        │    sends to all except       │
                        │    the sender                │
                        └─────────────────────────────┘
```

- The **main thread** runs an `accept()` loop and spawns a new thread for every incoming connection.
- Each **client thread** handles one client: handshake → message loop → cleanup.
- The **client table** is shared across all threads and protected by a mutex.
- The **client** runs two concurrent threads: one for sending (main), one for receiving.

---

## Message Frame Protocol

Every message exchanged over the wire uses a fixed-size binary frame:

```
 0        2        4       36      1060
 ┌────────┬────────┬───────┬───────────┐
 │ MAGIC  │  LEN   │ NAME  │   DATA    │
 │ 2 bytes│ 2 bytes│32 bytes│1024 bytes │
 └────────┴────────┴───────┴───────────┘
```

| Field    | Size     | Description                              |
|----------|----------|------------------------------------------|
| `magic`  | 2 bytes  | `0xABCD` — frame start marker            |
| `data_len`| 2 bytes | Length of the `data` field content       |
| `name`   | 32 bytes | Sender's username (null-terminated)      |
| `data`   | 1024 bytes | Message text (null-terminated)         |

> `#pragma pack(1)` is used to ensure **no struct padding**, so the layout on wire exactly matches the struct in memory — a critical requirement in embedded and protocol programming.

The receiver validates `magic == 0xABCD` before processing. Frames with incorrect magic are silently discarded.

---

## Project Structure

```
.
├── server.c       # Multi-threaded TCP server
├── client.c       # TCP chat client with send/receive threads
└── README.md      # This file
```

---

## Prerequisites

### Windows
- [MinGW-w64](https://www.mingw-w64.org/) or [MSYS2](https://www.msys2.org/) with GCC installed
- Winsock2 (included with Windows SDK — no extra install needed)

### Linux
- GCC: `sudo apt install gcc` (Ubuntu/Debian) or `sudo dnf install gcc` (Fedora)
- pthreads: included with glibc — no extra install needed

---

## Build Instructions

### Windows (MinGW / GCC)

Open a terminal (CMD, PowerShell, or MSYS2 shell):

```bash
# Build the server
gcc server.c -o server.exe -lws2_32

# Build the client
gcc client.c -o client.exe -lws2_32
```

### Linux (GCC)

```bash
# Build the server
gcc server.c -o server -lpthread

# Build the client
gcc client.c -o client -lpthread
```

> The only difference is the linker flag: `-lws2_32` (Winsock) on Windows vs `-lpthread` on Linux. The source code is identical.

---

## How to Run

### Step 1 — Start the server

```bash
# Windows
server.exe

# Linux
./server
```

Expected output:
```
============================================
  Multi-Threaded TCP Chat Server
  Listening on port 8080 | Max clients: 10
============================================
```

### Step 2 — Connect clients (open a new terminal for each)

```bash
# Windows
client.exe

# Linux
./client
```

You will be prompted:
```
Enter your name: Alice
```

After entering your name, you can start chatting. Open more terminals and connect more clients to simulate a group chat.

---

## Commands

| Command  | Description                        |
|----------|------------------------------------|
| `/quit`  | Gracefully disconnect from server  |

> More commands (e.g., `/list` to show active users) can be added to the `client_handler` function in `server.c`.

---

## Platform Differences

The cross-platform support is handled entirely via `#ifdef _WIN32` preprocessor macros. Here is a summary of what changes under the hood:

| Feature           | Windows                        | Linux                          |
|-------------------|--------------------------------|--------------------------------|
| Socket type       | `SOCKET` (unsigned)            | `int`                          |
| Invalid socket    | `INVALID_SOCKET`               | `-1`                           |
| Close socket      | `closesocket(fd)`              | `close(fd)`                    |
| Socket error      | `SOCKET_ERROR`                 | `-1`                           |
| Init required     | `WSAStartup(MAKEWORD(2,2), &w)`| None                           |
| Cleanup required  | `WSACleanup()`                 | None                           |
| Thread type       | `HANDLE` (Win32 thread)        | `pthread_t`                    |
| Thread create     | `CreateThread()`               | `pthread_create()`             |
| Thread detach     | Not needed (handle ignored)    | `pthread_detach()`             |
| Mutex type        | `CRITICAL_SECTION`             | `pthread_mutex_t`              |
| Mutex lock        | `EnterCriticalSection()`       | `pthread_mutex_lock()`         |
| Mutex unlock      | `LeaveCriticalSection()`       | `pthread_mutex_unlock()`       |
| Linker flag       | `-lws2_32`                     | `-lpthread`                    |

---

## Key Concepts

### `send_all()` — Reliable Send Loop
TCP is a **stream protocol** — a single `send()` call may not transmit all bytes at once. The `send_all()` function loops until every byte is delivered, which is critical for binary frame integrity.

```c
static int send_all(sock_t fd, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}
```

### Mutex-Protected Client Table
All threads share a global `clients[]` array. Every read or write to this array is wrapped in `lock()` / `unlock()` to prevent race conditions.

### Thread-Per-Client Model
Each connected client gets its own OS thread. The server's main thread never blocks on a client — it immediately returns to `accept()` for the next connection.

### Binary Frame with Magic Header
Using a fixed magic value (`0xABCD`) at the start of every frame lets the receiver detect corrupt or partial frames — a standard technique in embedded communication protocols (e.g., CAN bus, UART protocols).

---

## Resume Highlights

- Designed and implemented a **cross-platform multi-threaded TCP chat server** in C supporting up to 10 concurrent clients using POSIX threads (Linux) and Win32 threads (Windows)
- Implemented a **custom binary message-framing protocol** with magic header validation and `#pragma pack(1)` for exact wire-layout control, mirroring embedded protocol design practices
- Used **mutex synchronization** to protect a shared client registry across concurrent threads, ensuring race-condition-free state management
- Built a reliable `send_all()` transmission loop to handle partial TCP sends, ensuring binary frame integrity over streaming sockets

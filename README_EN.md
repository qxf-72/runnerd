<div align="center">

# runnerd

English | [简体中文](README.md)

A local Linux task execution daemon written in C++17.

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B)
![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C?logo=cmake)
![Platform](https://img.shields.io/badge/platform-Linux-FCC624?logo=linux&logoColor=black)
![Status](https://img.shields.io/badge/status-early%20development-orange)
[![License](https://img.shields.io/github/license/qxf-72/runnerd)](LICENSE)

</div>

`runnerd` is designed for a single user on a local Linux host. The
`runnerctl` client communicates with the resident daemon over a Unix Domain
Socket. The project will eventually support job submission, status queries,
cancellation, timeouts, output capture, and job history recovery.

> [!IMPORTANT]
> This project is in early development. The current version provides the Unix
> Domain Socket communication skeleton, length-prefixed framing, a `PING/PONG`
> path, and a basic `epoll` event loop; it cannot execute jobs yet.

## ✨ Features

Currently implemented:

- Local client/server communication based on `AF_UNIX + SOCK_STREAM`
- A default listening endpoint at `/tmp/runnerd.sock`
- `PING/PONG` communication between `runnerctl ping` and `runnerd`
- A 4-byte big-endian length prefix with a 64 KiB payload limit
- A protocol-layer incremental decoder for fragmented and coalesced frames
- Non-blocking listener and client sockets
- A level-triggered `epoll` loop for listener and client readability
- `accept` and `read` loops that continue until `EAGAIN`
- Socket file permissions restricted to `0600`
- Safe handling of an existing path on startup:
  - Refuses to start when another server is already listening
  - Refuses to remove a path that is not a socket
  - Removes a stale socket owned by the current user
- Retries for interrupted `accept`, `read`, and `write` calls
- `SOCK_CLOEXEC` to prevent accidental file descriptor inheritance
- `SIGPIPE` handling so a disconnected peer does not terminate the process
- CMake builds with smoke and protocol tests registered with CTest

## 🏗️ Current Architecture

```text
runnerctl ping
      |
      | Unix Domain Socket
      | /tmp/runnerd.sock
      v
   runnerd
      |
      `-- epoll_wait
            |-- readable listen fd
            |     `-- accept until EAGAIN
            `-- readable client fd
                  `-- read until EAGAIN
                        |-- parse the "PING" payload
                        `-- directly write a framed "PONG"
```

The current implementation is a basic level-triggered `epoll` loop. Listener
and client file descriptors are non-blocking, but per-connection decoder state
is not stored yet and responses are still written directly. Fragmented frames
that span multiple `epoll` events and writes that need to resume after
`EAGAIN` will be handled by the next connection-management and output-buffer
milestone.

## 📁 Project Structure

```text
runnerd/
|-- CMakeLists.txt
|-- include/
|   `-- runnerd/
|       |-- protocol.h
|       `-- unix_socket.h
|-- src/
|   |-- protocol.cpp
|   |-- runnerd_main.cpp
|   |-- runnerctl_main.cpp
|   `-- unix_socket.cpp
|-- tests/
|   |-- protocol_test.cpp
|   `-- smoke_test.cpp
|-- docs/
|   |-- requirements.md
|   `-- state_machine.md
|-- LICENSE
|-- README.md
`-- README_EN.md
```

## 🚀 Build and Run

### Requirements

- Linux
- GCC or Clang with C++17 support
- CMake 3.16 or later

### Build

```bash
git clone https://github.com/qxf-72/runnerd.git
cd runnerd

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The build produces:

```text
build/
|-- librunnerd_protocol.a
|-- protocol_test
|-- runnerd
|-- runnerctl
`-- smoke_test
```

### Run the Example

Start the server in the first terminal:

```bash
./build/runnerd
```

Expected output:

```text
runnerd is listening on /tmp/runnerd.sock
```

Send a request from a second terminal:

```bash
./build/runnerctl ping
```

Expected client output:

```text
PONG
```

The server also prints:

```text
received PING
```

The socket path is currently fixed at `/tmp/runnerd.sock`. If the server exits
abnormally and leaves a stale socket file behind, the next startup checks the
path and safely removes the stale file.

### Run the Tests

```bash
cmake -E chdir build ctest --output-on-failure
```

Alternatively, run CTest from the build directory:

```bash
cd build
ctest --output-on-failure
```

The current test suite includes:

- `smoke_test`, which verifies that the basic test target builds and runs
- `protocol_test`, which covers big-endian encoding, fragmented and coalesced
  frames, empty and binary payloads, the 64 KiB boundary, and invalid lengths

Automated client-to-daemon integration tests will be added in a later
milestone.

## 📡 Current Protocol

Like TCP, a Unix Domain Stream Socket does not preserve message boundaries.
The current protocol therefore uses a 4-byte big-endian length field to
delimit each payload:

```text
+--------------------------+----------------------+
| payload length (4 bytes) | variable payload     |
| big-endian unsigned int  | up to 64 KiB         |
+--------------------------+----------------------+
```

The currently supported request and response payloads are:

```text
request:  "PING"
response: "PONG"
error:    "ERR!"
```

`FrameDecoder` accepts fragmented input across multiple calls and can extract
multiple complete frames from one input buffer. Frames declaring payloads
larger than 64 KiB are rejected. The daemon does not yet retain a decoder for
each connection, so it currently handles only request frames completed during
one readable-event handling pass reliably.

## 🎯 Project Scope

The final project is intentionally scoped as a local, single-user job runner.
It does not plan to support:

- Remote access over TCP
- Multi-user login or authorization
- HTTP APIs or a Web UI
- Containers or cgroups
- Databases or distributed agents
- Thread pools, job dependency DAGs, or automatic retries

This scope keeps the project focused on Linux process management,
non-blocking I/O, event loops, and honest crash recovery.

## 🗺️ Roadmap

- [x] Initialize CMake, CTest, requirements, and state machine documentation
- [x] Implement Unix Domain Socket `PING/PONG` communication
- [x] Implement length-prefixed framing and incremental decoding
- [ ] Support multiple clients with non-blocking I/O and `epoll` (basic event
  loop complete; connection state and output buffering remain)
- [ ] Add job submission, `fork/execve`, and stdout/stderr capture
- [ ] Add the job state machine, concurrency queue, cancellation, and timeouts
- [ ] Persist job history in a journal and recover it after restart
- [ ] Add integration tests, Sanitizer checks, and diagnostic reports

## 📚 Documentation

- [Requirements](docs/requirements.md)
- [Job state machine](docs/state_machine.md)

## 🤝 Contributing

Issues and Pull Requests are welcome.

Before submitting a change, make sure the project builds and the tests pass:

```bash
cmake --build build
cmake -E chdir build ctest --output-on-failure
```

The project is still in an early stage. If a change affects the protocol,
state machine, or job lifecycle, please describe the proposed design and
behavioral boundaries in an Issue first.

## 📄 License

This project is licensed under the [MIT License](LICENSE).

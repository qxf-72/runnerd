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
> This project is in early development. The current version only provides the
> Unix Domain Socket communication skeleton and a `PING/PONG` path; it cannot
> execute jobs yet.

## ✨ Features

Currently implemented:

- Local client/server communication based on `AF_UNIX + SOCK_STREAM`
- A default listening endpoint at `/tmp/runnerd.sock`
- `PING/PONG` communication between `runnerctl ping` and `runnerd`
- Socket file permissions restricted to `0600`
- Safe handling of an existing path on startup:
  - Refuses to start when another server is already listening
  - Refuses to remove a path that is not a socket
  - Removes a stale socket owned by the current user
- Correct handling of short reads, short writes, and interrupted system calls
- `SOCK_CLOEXEC` to prevent accidental file descriptor inheritance
- `SIGPIPE` handling so a disconnected peer does not terminate the process
- CMake builds and a basic smoke test registered with CTest

## 🏗️ Current Architecture

```text
runnerctl ping
      |
      | Unix Domain Socket
      | /tmp/runnerd.sock
      v
   runnerd
      |
      |-- accept a client connection
      |-- read the fixed 4-byte PING request
      `-- return the fixed 4-byte PONG response
```

The server currently uses blocking I/O and processes clients sequentially. A
non-blocking, `epoll`-based event loop will replace this implementation in a
later milestone.

## 📁 Project Structure

```text
runnerd/
|-- CMakeLists.txt
|-- include/
|   `-- runnerd/
|       `-- unix_socket.h
|-- src/
|   |-- runnerd_main.cpp
|   |-- runnerctl_main.cpp
|   `-- unix_socket.cpp
|-- tests/
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

The repository currently contains one basic smoke test. It only verifies that
the test target builds and runs successfully. Protocol unit tests and daemon
integration tests will be added in later milestones.

## 📡 Current Protocol

The current protocol only verifies the basic communication path:

```text
request:  "PING"  # fixed 4 bytes
response: "PONG"  # fixed 4 bytes
```

Like TCP, a Unix Domain Stream Socket does not preserve message boundaries.
The next milestone will introduce a length-prefixed protocol:

```text
+--------------------------+----------------------+
| payload length (4 bytes) | variable payload     |
| big-endian unsigned int  | up to 64 KiB         |
+--------------------------+----------------------+
```

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
- [ ] Implement length-prefixed framing and incremental decoding
- [ ] Support multiple clients with non-blocking I/O and `epoll`
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

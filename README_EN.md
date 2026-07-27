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
> Domain Socket transport, length-prefixed framing, a `PING/PONG` path, and an
> `epoll` event loop with per-connection state and non-blocking output
> buffering. It also defines the job data model, validation rules, and state
> transitions. The model is not connected to the daemon or wire protocol yet,
> so jobs still cannot be submitted or executed.

## ✨ Features

Currently implemented:

- Local client/server communication based on `AF_UNIX + SOCK_STREAM`
- A default endpoint at `/tmp/runnerd.sock`, configurable with
  `--socket <path>`
- `PING/PONG` communication between `runnerctl ping` and `runnerd`
- A 4-byte big-endian length prefix with a 64 KiB payload limit
- A protocol-layer incremental decoder for fragmented and coalesced frames
- Non-blocking listener and client sockets
- A level-triggered `epoll` loop for the listener and multiple client fds
- Per-connection decoder state, output buffering, and write offsets
- Incomplete frames retained across `epoll` events and all complete frames
  processed from each read
- `accept` and `read` loops that continue until `EAGAIN`; writes continue
  until the output buffer is drained or `EAGAIN` is returned
- `EPOLLOUT` enabled only while output is pending, with partial-write resumption
- Pending responses flushed before closing a peer-half-closed connection
- Socket file permissions restricted to `0600`
- Safe handling of an existing path on startup:
  - Refuses to start when another server is already listening
  - Refuses to remove a path that is not a socket
  - Removes a stale socket owned by the current user
- Retries for interrupted `accept`, `read`, and `write` calls
- `SOCK_CLOEXEC` to prevent accidental file descriptor inheritance
- `SIGPIPE` handling so a disconnected peer does not terminate the process
- `JobId`, `JobSpec`, `Job`, and eight `JobState` values for identity,
  arguments, runtime state, process metadata, and exit results
- Validation for missing commands, embedded NUL bytes, and non-positive
  configured timeouts
- Centralized checks for all ten legal state transitions and terminal states
- CMake builds with smoke, protocol, job model, and multi-client integration
  tests registered with CTest

## 🏗️ Current Architecture

```text
runnerctl ping
      |
      | Unix Domain Socket
      | /tmp/runnerd.sock (default)
      v
   runnerd
      |
      `-- epoll_wait
            |-- readable listen fd
            |     `-- accept until EAGAIN
            `-- client fd
                  |-- Connection
                  |     |-- FrameDecoder
                  |     |-- output buffer
                  |     |-- write offset
                  |     `-- read closed
                  |-- EPOLLIN
                  |     `-- read -> decode -> queue response
                  |-- EPOLLOUT
                  |     `-- resume from the write offset
                  `-- EPOLLRDHUP
                        `-- drain final input -> flush output -> close
```

The server stores connection state by file descriptor in a `Connections` map.
Reads continue until `EAGAIN`; complete requests are decoded and their
responses are appended to the output buffer. `EPOLLOUT` is registered only
while output is pending. A partial write resumes from its saved offset on the
next writable event, and `EPOLLOUT` is removed once the buffer is drained to
avoid a busy loop.

The job model currently lives in a standalone `runnerd_job` static library:

```text
JobSpec
   |
   `-- validateJobSpec
           |
           v
      Job (initial state: QUEUED)
           |
           |-- canTransition: validate a state change
           |-- transitionJob: apply a legal state change
           `-- isTerminal: determine whether the job has finished
```

This model has unit-test coverage but is not linked into the `runnerd` event
loop yet. The daemon will begin using it when job submission and process
management are implemented.

## 📁 Project Structure

```text
runnerd/
|-- CMakeLists.txt
|-- include/
|   `-- runnerd/
|       |-- job.h
|       |-- protocol.h
|       `-- unix_socket.h
|-- src/
|   |-- job.cpp
|   |-- protocol.cpp
|   |-- runnerd_main.cpp
|   |-- runnerctl_main.cpp
|   `-- unix_socket.cpp
|-- tests/
|   |-- job_test.cpp
|   |-- protocol_test.cpp
|   |-- runnerd_integration_test.sh
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
|-- librunnerd_job.a
|-- librunnerd_protocol.a
|-- job_test
|-- protocol_test
|-- runnerd
|-- runnerctl
`-- smoke_test
```

### Run the Example

Command syntax:

```text
runnerd [--socket <path>]
runnerctl [--socket <path>] ping
```

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

Both programs use `/tmp/runnerd.sock` when `--socket` is omitted. To use a
different path, pass the same option to the server and client:

```bash
./build/runnerd --socket /tmp/runnerd-demo.sock
./build/runnerctl --socket /tmp/runnerd-demo.sock ping
```

If the server exits abnormally and leaves a stale socket file behind, the next
startup using the same path checks and safely removes it.

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
- `job_test`, which covers valid and invalid job specifications, every legal
  state transition, representative invalid transitions, and terminal-state
  detection
- `runnerd_integration_test`, which starts a real daemon on an isolated
  temporary socket, runs 20 `runnerctl ping` processes concurrently, and
  verifies that every client prints `PONG`

Additional integration coverage for cross-event fragmentation, half-closes,
and partial writes will be added in a later milestone.

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
unknown request response: "ERR!"
```

`FrameDecoder` accepts fragmented input across multiple calls and can extract
multiple complete frames from one input buffer. Frames declaring payloads
larger than 64 KiB are rejected. The daemon retains an independent decoder for
each client, so a request may span multiple reads and multiple `epoll` events.
Multiple complete requests received together are processed in order and each
produces a response. Unknown requests receive `ERR!`; malformed or oversized
frames cause the server to close the corresponding client connection.

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
- [x] Support multiple clients with non-blocking I/O, connection state, output
  buffering, and `epoll`
- [x] Define the job model, validation, state-transition rules, and unit tests
- [ ] Add job submission, `fork/execve`, and stdout/stderr capture
- [ ] Connect the job model to the daemon and add the concurrency queue,
  cancellation, and timeouts
- [ ] Persist job history in a journal and recover it after restart
- [ ] Add more failure-path integration tests, Sanitizer checks, and
  diagnostic reports

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

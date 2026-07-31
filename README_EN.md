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

`runnerd` is a single-host, single-user job execution service. The `runnerctl`
client submits commands over a Unix Domain Socket, while the daemon starts,
monitors, and reaps child processes.

> [!NOTE]
> This project is still in early development, and its protocol and CLI may change.

## ✨ Features

- Local communication over a Unix Domain Socket with a configurable path
- A non-blocking, level-triggered `epoll` event loop for multiple clients
- Length-prefixed framing with incremental decoding for fragmented,
  coalesced, and binary payloads
- `ping`, `submit`, `status`, and `list` commands with server-side validation
- Job startup through `fork/execve` in an independent process group, without
  invoking a shell
- stdout/stderr capture through non-blocking pipes and child reaping with
  `signalfd + waitpid`
- A job state machine, in-memory job table, and GoogleTest/CTest coverage

## 🏗️ Current Architecture

```text
runnerctl
    |  Unix Domain Socket + length-prefixed protocol
    v
runnerd (epoll event loop)
    |-- client fd ----------------> decode requests, query jobs, buffer responses
    `-- ProcessMonitor
          |-- process_launcher ---> fork / execve ---> child process
          |-- process pipes ------------------------> capture output/startup errors
          `-- signalfd ---> waitpid ----------------> update Job state
```

Network sockets and child-process pipes share the same `epoll` event loop.
Disconnecting a client does not terminate its submitted jobs. `ProcessMonitor`
settles a result only after the child has exited and every monitored pipe has
reached EOF.

### Current Limitations

- `--timeout` is stored in `JobSpec` but is not enforced yet
- There are no `cancel` or output-query commands
- There is no concurrency limit or waiting queue; valid jobs start immediately
- Jobs and output live only in memory, with no output-size limit
- Restarting the daemon loses the JobId counter, job states, and output

## 📁 Project Structure

| Module | Responsibility |
| --- | --- |
| `protocol` | Length-prefixed framing and SUBMIT/STATUS encoding |
| `job` | Job model, validation, and state machine |
| `process_launcher` | Pipes, fork, redirection, process groups, and execve |
| `process_monitor` | epoll registration, output capture, SIGCHLD, and settlement |
| `unix_socket` | Unix Domain Socket creation and connection |
| `runnerd_main` | Daemon entry point and event loop |
| `runnerctl_main` | Command-line client |
| `tests/` | Unit and end-to-end integration tests |

## 🚀 Build and Run

### Requirements

- Linux
- GCC or Clang with C++17 support
- CMake 3.16 or later

The test suite uses GoogleTest 1.17.0. On the first CMake configuration with
tests enabled, `FetchContent` downloads and verifies its source, so access to
GitHub is required. Later configurations reuse the dependency already present
in the build directory.

### Build

```bash
git clone https://github.com/qxf-72/runnerd.git
cd runnerd

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Run the Example

Command syntax:

```text
runnerd [--socket <path>]
runnerctl [--socket <path>] ping
runnerctl [--socket <path>] submit [--timeout <milliseconds>] \
          -- <absolute-path> [arguments...]
runnerctl [--socket <path>] status <job_id>
runnerctl [--socket <path>] list
```

Start the daemon in the first terminal:

```bash
./build/runnerd
```

Check the connection and submit a job from a second terminal:

```bash
./build/runnerctl ping                         # prints PONG
./build/runnerctl submit -- /bin/echo hello   # prints a JobId, such as 1
```

Submit a job with a positive timeout configuration in milliseconds:

```bash
./build/runnerctl submit --timeout 5000 -- /bin/sleep 1
```

Query one job or list every in-memory job:

```bash
./build/runnerctl status 1
./build/runnerctl list
```

Both programs use `/tmp/runnerd.sock` when `--socket` is omitted. For a custom
path, pass the same option to the daemon and client:

```bash
./build/runnerd --socket /tmp/runnerd-demo.sock
./build/runnerctl --socket /tmp/runnerd-demo.sock ping
```

The `--` delimiter ends `runnerctl` option parsing; everything after it is
preserved as job argv. `argv[0]` must be an absolute path, so `echo hello` and
`./echo hello` are rejected. The daemon captures stdout/stderr, but the client
cannot query it yet.

![runnerd runtime demo](docs/images/runnerd-demo.png)

### Run the Tests

```bash
cmake -E chdir build ctest --output-on-failure
```

The current test targets include:

| Test target | Main coverage |
| --- | --- |
| `protocol_test` | Framing, SUBMIT/STATUS encoding, and malformed input |
| `job_test` | Validation, state transitions, and terminal states |
| `process_launcher_test` | fork/execve, pipes, process groups, and startup errors |
| `process_monitor_test` | Output, settlement, large output, and concurrent reaping |
| `runnerd_integration_test` | Real daemon, concurrent PING, SUBMIT, STATUS, and LIST |

## 📡 Current Protocol

Like TCP, a Unix Domain Stream Socket does not preserve message boundaries.
The protocol uses a 4-byte big-endian length prefix and limits each payload to
64 KiB:

```text
[ payload length: uint32 big-endian ][ payload bytes ]
```

| Request | Successful response |
| --- | --- |
| `PING` | `PONG` |
| `SUBMIT + timeout_ms + argc + argv` | `OK <job_id>` |
| `STATUS + job_id` | `OK id=<id> state=<state> ...` |
| `LIST` | `OK` followed by JobId-sorted summaries |

SUBMIT integers and argument lengths also use unsigned big-endian encoding;
`timeout_ms = 0` means no timeout. STATUS uses an 8-byte unsigned big-endian
JobId. Invalid requests return `ERR <message>`. `FrameDecoder` can assemble one
frame across multiple reads and decode multiple frames from a single read.

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

- [x] Initialize CMake, GoogleTest, CTest, requirements, and state machine
  documentation
- [x] Implement Unix Domain Socket `PING/PONG` communication
- [x] Implement length-prefixed framing and incremental decoding
- [x] Support multiple clients with non-blocking I/O, connection state, output
  buffering, and `epoll`
- [x] Define the job model, validation, state-transition rules, and unit tests
- [x] Add SUBMIT encoding, JobId allocation, and an in-memory job table
- [x] Start jobs with `fork/execve` and capture stdout/stderr
- [x] Add STATUS and LIST job queries
- [ ] Add the concurrency queue, cancellation, and timeouts
- [ ] Persist job history in a journal and recover it after restart
- [ ] Add more failure-path integration tests, Sanitizer checks, and
  diagnostic reports

## 📚 Documentation

- [Requirements](docs/requirements.md)
- [Job state machine](docs/state_machine.md)
- [Process launcher](docs/process_launcher.md)

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

<div align="center">

# runnerd

**An event-driven Linux daemon for executing and supervising local jobs.**

English | [简体中文](README.md)

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C?logo=cmake)
![Platform](https://img.shields.io/badge/platform-Linux-FCC624?logo=linux&logoColor=black)
[![CI](https://github.com/qxf-72/runnerd/actions/workflows/ci.yml/badge.svg)](https://github.com/qxf-72/runnerd/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/qxf-72/runnerd)](LICENSE)

[Quick start](#-quick-start) · [Usage](#-usage) · [How it works](#-how-it-works) · [Documentation](#-documentation)

</div>

`runnerd` targets trusted, single-host, single-user environments. `runnerctl`
submits commands over a Unix Domain Socket while the daemon queues, launches,
monitors, times out, and reaps child processes in the background.

The project focuses on Linux event loops and process lifecycle management. It
is a practical example of coordinating `epoll`, `signalfd`, `timerfd`,
`fork/execve`, and process groups.

> [!WARNING]
> `runnerd` is experimental. It currently has no authentication, sandboxing,
> or persistence. Do not expose it to untrusted users or workloads.

## ✨ Features

- **Event driven**: one `epoll` loop handles clients, child output, signals, and timers.
- **Predictable execution**: absolute-path programs are launched with `fork/execve`, without a shell.
- **Process supervision**: every job gets a separate process group, with stdout, stderr, and exit results captured.
- **FIFO scheduling**: `--max-running` bounds concurrency while excess jobs wait in order.
- **Cancellation and timeouts**: process-group `SIGTERM` is followed by `SIGKILL` after a one-second grace period.
- **Tested behavior**: GoogleTest and real-daemon integration tests cover the protocol, scheduler, and process lifecycle.

## 🚀 Quick Start

### Requirements

- Linux
- GCC or Clang with C++17 support
- CMake 3.16 or later

### Build

```bash
git clone https://github.com/qxf-72/runnerd.git
cd runnerd

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build --parallel
```

### Run

Start the daemon in one terminal:

```bash
./build/runnerd --max-running 2
```

Submit and inspect jobs from another terminal:

```bash
./build/runnerctl ping
# PONG

./build/runnerctl submit --timeout 5000 -- /bin/sleep 10
# 1

./build/runnerctl status 1
./build/runnerctl list
./build/runnerctl cancel 1
```

The default socket is `/tmp/runnerd.sock`. Pass the same custom path to both
programs when needed:

```bash
./build/runnerd --socket /tmp/runnerd-demo.sock --max-running 2
./build/runnerctl --socket /tmp/runnerd-demo.sock ping
```

## 📖 Usage

### Daemon

```text
runnerd [--socket <path>] [--max-running <positive-integer>]
```

`--max-running` defaults to `1`.

### Client

| Command | Purpose |
| --- | --- |
| `runnerctl ping` | Check whether the daemon is reachable |
| `runnerctl submit [--timeout <ms>] -- <absolute-path> [args...]` | Submit a job and return its JobId |
| `runnerctl status <job_id>` | Query a job's state and available result fields |
| `runnerctl list` | List in-memory jobs in JobId order |
| `runnerctl cancel <job_id>` | Cancel a queued job or terminate a running job |

Every client command accepts `--socket <path>` before the command name.

### Behavior to know

- `--` ends `runnerctl` option parsing; the remaining values form the job's `argv` unchanged.
- `argv[0]` must be an absolute path. Shell syntax such as `|`, `>`, and `$VAR` is not interpreted.
- Disconnecting a client does not cancel a submitted job.
- Execution timeout starts when the job enters `RUNNING`; time spent in the FIFO queue is excluded.
- Cancellation and timeout first send process-group `SIGTERM`, then `SIGKILL` if the job has not exited within one second.
- When cancellation, timeout, and natural exit race, the first event that successfully changes the job state wins.
- stdout and stderr are captured in memory; only their byte counts are currently queryable.

## 🔄 How It Works

```text
runnerctl ── Unix Domain Socket ──> runnerd ── fork/execve ──> child process group
                                      │
                       epoll drives clients, pipes, signals, and deadlines
```

`SUBMIT` creates a job and adds it to the FIFO queue. When a slot is free, the
daemon launches the job and supervises its entire process group. `signalfd`
reports child exit, non-blocking pipes carry stdout and stderr, and `timerfd`
drives both execution and force-kill deadlines. A job becomes terminal and
releases its slot only after the direct child exits and every monitored pipe
reaches EOF.

Jobs can move through these states:

```text
QUEUED -> RUNNING -> SUCCEEDED / FAILED
                  -> TERMINATING -> CANCELLED / TIMED_OUT
QUEUED -> CANCELLED / FAILED
```

See the [project documentation](#-documentation) for protocol constraints,
the complete state machine, and file-descriptor ownership.

## ⚠️ Current Limitations

- Linux-only and intended for a local single-user environment; the socket file is created with mode `0600`.
- Jobs, results, and the JobId counter live only in memory and disappear when the daemon restarts.
- Captured output is unbounded and cannot yet be retrieved through `runnerctl`.
- There is no authentication, container or cgroup isolation, multi-user authorization, or remote execution.
- LIST has no pagination, and its complete response is limited by the 64 KiB frame size.

## 🧪 Development

Configure and run the complete test suite:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
cmake -E chdir build ctest --output-on-failure
```

Tests use GoogleTest and are discovered through CTest. When tests are enabled
for the first time, CMake downloads a pinned GoogleTest release with
`FetchContent`. GitHub Actions configures, builds, and tests the project for
every push and pull request.

The project uses C++17 and enables `-Wall`, `-Wextra`, and `-Wpedantic` for its targets.

## 📚 Documentation

| Document | Contents |
| --- | --- |
| [Requirements and current boundaries](docs/requirements.md) | Commands, protocol, scheduling, cancellation, timeouts, and non-goals |
| [Job state machine](docs/state_machine.md) | Legal transitions, runtime behavior, and test coverage |
| [Process launcher](docs/process_launcher.md) | `fork/execve`, pipes, process groups, and startup error reporting |

## 🗺️ Roadmap

- [x] Unix Domain Socket transport and length-prefixed protocol
- [x] Non-blocking multi-client I/O with `epoll`
- [x] `fork/execve`, process groups, output capture, and child reaping
- [x] FIFO scheduling, concurrency limits, and automatic queue advancement
- [x] STATUS, LIST, and CANCEL
- [x] Execution timeouts driven by `timerfd`
- [x] `SIGTERM` grace period and process-group `SIGKILL` escalation
- [ ] Bounded output and output retrieval
- [ ] Persistent job history and restart recovery
- [ ] Sanitizers, additional fault injection, and diagnostics

## 🤝 Contributing

Issues and pull requests are welcome. Before submitting a change, make sure the
project builds and the complete test suite passes. For changes to the protocol,
state machine, or job lifecycle semantics, describe the intended behavior and
boundaries in an issue first.

## 📄 License

Distributed under the [MIT License](LICENSE).

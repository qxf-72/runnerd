<div align="center">

# runnerd

**一个用于执行和监管本地任务的事件驱动 Linux 守护服务。**

[English](README_EN.md) | 简体中文

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C?logo=cmake)
![Platform](https://img.shields.io/badge/platform-Linux-FCC624?logo=linux&logoColor=black)
[![CI](https://github.com/qxf-72/runnerd/actions/workflows/ci.yml/badge.svg)](https://github.com/qxf-72/runnerd/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/qxf-72/runnerd)](LICENSE)

[快速开始](#-快速开始) · [使用方法](#-使用方法) · [工作原理](#-工作原理) · [文档](#-文档)

</div>

`runnerd` 面向可信的单机、单用户环境。`runnerctl` 通过 Unix Domain Socket
提交命令，daemon 在后台负责排队、启动、监控、超时终止和回收子进程。

这个项目聚焦 Linux 事件循环与进程生命周期管理，适合作为 `epoll`、`signalfd`、
`timerfd`、`fork/execve` 和进程组协作的实践项目。

> [!WARNING]
> `runnerd` 仍处于实验阶段，目前没有认证、沙箱隔离和持久化。请勿将它暴露给
> 不受信任的用户或工作负载。

## ✨ 特性

- **事件驱动**：一个 `epoll` 循环处理客户端连接、子进程输出、信号和定时器。
- **可靠执行**：使用 `fork/execve` 启动绝对路径程序，不经过 shell。
- **任务监管**：每个任务拥有独立进程组，并采集 stdout、stderr 和退出结果。
- **FIFO 调度**：通过 `--max-running` 限制并发数，超额任务自动排队。
- **取消与超时**：先向进程组发送 `SIGTERM`，1 秒宽限期后升级为 `SIGKILL`。
- **可验证行为**：GoogleTest 和真实 daemon 集成测试覆盖协议、调度和进程生命周期。

## 🚀 快速开始

### 环境要求

- Linux
- 支持 C++17 的 GCC 或 Clang
- CMake 3.16 或更高版本

### 构建

```bash
git clone https://github.com/qxf-72/runnerd.git
cd runnerd

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build --parallel
```

### 运行

在第一个终端启动 daemon：

```bash
./build/runnerd --max-running 2
```

在第二个终端提交和查询任务：

```bash
./build/runnerctl ping
# PONG

./build/runnerctl submit --timeout 5000 -- /bin/sleep 10
# 1

./build/runnerctl status 1
./build/runnerctl list
./build/runnerctl cancel 1
```

默认 socket 路径为 `/tmp/runnerd.sock`。自定义路径时，两个程序必须使用相同参数：

```bash
./build/runnerd --socket /tmp/runnerd-demo.sock --max-running 2
./build/runnerctl --socket /tmp/runnerd-demo.sock ping
```

## 📖 使用方法

### daemon

```text
runnerd [--socket <path>] [--max-running <positive-integer>]
```

`--max-running` 默认为 `1`。

### 客户端

| 命令 | 作用 |
| --- | --- |
| `runnerctl ping` | 检查 daemon 是否可用 |
| `runnerctl submit [--timeout <ms>] -- <absolute-path> [args...]` | 提交任务并返回 JobId |
| `runnerctl status <job_id>` | 查询任务状态和已产生的运行结果 |
| `runnerctl list` | 按 JobId 列出当前内存中的任务 |
| `runnerctl cancel <job_id>` | 取消排队任务或终止运行任务 |

所有客户端命令都可以在命令名前添加 `--socket <path>`。

### 需要了解的行为

- `--` 表示 `runnerctl` 参数结束，其后的内容会原样组成任务的 `argv`。
- `argv[0]` 必须是绝对路径；`runnerd` 不解释 `|`、`>`、`$VAR` 等 shell 语法。
- 客户端断开不会取消已经提交的任务。
- 超时从任务进入 `RUNNING` 后开始计算，FIFO 排队时间不计入执行时间。
- 取消和超时都会先发送进程组 `SIGTERM`；任务未在 1 秒内退出时再发送 `SIGKILL`。
- 取消、超时和自然退出发生竞争时，第一个成功改变任务状态的事件决定最终结果。
- stdout/stderr 会被采集到内存中；当前只能查询字节数，不能读取正文。

## 🔄 工作原理

```text
runnerctl ── Unix Domain Socket ──> runnerd ── fork/execve ──> 子进程组
                                      │
                         epoll 驱动连接、管道、信号和期限
```

`SUBMIT` 创建任务并加入 FIFO 队列；有空闲槽位时，daemon 启动任务并监管整个
进程组。`signalfd` 通知子进程退出，非阻塞 pipe 提供 stdout/stderr，`timerfd`
负责执行超时和强杀期限。只有直接子进程退出且所有管道 EOF 后，任务才会进入终态
并释放运行槽位。

任务可能经历以下状态：

```text
QUEUED -> RUNNING -> SUCCEEDED / FAILED
                  -> TERMINATING -> CANCELLED / TIMED_OUT
QUEUED -> CANCELLED / FAILED
```

更详细的状态迁移、协议约束和文件描述符所有权请查看[项目文档](#-文档)。

## ⚠️ 当前边界

- 仅支持 Linux 上的本地单用户场景，socket 文件权限为 `0600`。
- 任务、结果和 JobId 计数只保存在内存中，daemon 重启后会丢失。
- 已采集输出没有大小上限，也暂时不能通过 `runnerctl` 读取正文。
- 没有认证、容器、cgroup、多用户授权或远程执行能力。
- LIST 没有分页，完整响应受 64 KiB 单帧上限约束。

## 🧪 开发

配置并运行完整测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
cmake -E chdir build ctest --output-on-failure
```

测试使用 GoogleTest，并通过 CTest 发现和运行。首次启用测试时，CMake 会通过
`FetchContent` 下载固定版本的 GoogleTest。GitHub Actions 会在 push 和 Pull
Request 时执行配置、编译和测试。

项目使用 C++17，并对各目标启用 `-Wall`、`-Wextra` 和 `-Wpedantic`。

## 📚 文档

| 文档 | 内容 |
| --- | --- |
| [需求与当前边界](docs/requirements.md) | 命令、协议、调度、取消、超时和非目标 |
| [任务状态机](docs/state_machine.md) | 合法迁移、运行时行为和测试覆盖 |
| [进程启动器](docs/process_launcher.md) | `fork/execve`、pipe、进程组和错误上报 |

## 🗺️ 路线图

- [x] Unix Domain Socket 与长度前缀协议
- [x] 基于 `epoll` 的多客户端非阻塞 I/O
- [x] `fork/execve`、进程组、输出采集和子进程回收
- [x] FIFO 调度、并发限制和队列自动推进
- [x] STATUS、LIST 和 CANCEL
- [x] 基于 `timerfd` 的执行超时
- [x] `SIGTERM` 宽限期与进程组 `SIGKILL` 升级
- [ ] 有界输出与输出正文查询
- [ ] 任务历史持久化与重启恢复
- [ ] Sanitizer、更多故障注入和诊断能力

## 🤝 贡献

欢迎提交 Issue 和 Pull Request。提交前请确保项目能够构建并通过完整测试。涉及协议、
状态机或任务生命周期语义的改动，请在 Issue 中说明预期行为和边界。

## 📄 许可证

本项目采用 [MIT License](LICENSE) 开源。

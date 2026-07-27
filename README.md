<div align="center">

# runnerd

[English](README_EN.md) | 简体中文

一个使用 C++17 编写的 Linux 本地任务执行守护服务。

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B)
![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C?logo=cmake)
![Platform](https://img.shields.io/badge/platform-Linux-FCC624?logo=linux&logoColor=black)
![Status](https://img.shields.io/badge/status-early%20development-orange)
[![License](https://img.shields.io/github/license/qxf-72/runnerd)](LICENSE)

</div>

`runnerd` 面向同一台 Linux 主机上的同一用户。客户端 `runnerctl`
通过 Unix Domain Socket 与常驻服务通信；项目最终将支持任务提交、状态查询、
取消、超时控制、输出采集和任务历史恢复。

> [!IMPORTANT]
> 项目目前处于早期开发阶段。当前版本完成了 Unix Domain Socket
> 通信、长度前缀协议、`PING/PONG` 链路，以及带有连接状态和非阻塞
> 写缓冲的 `epoll` 事件循环；同时已经定义任务数据模型、参数校验和
> 状态迁移规则。任务模型尚未接入 daemon 和通信协议，因此目前仍不能
> 提交或执行任务。

## ✨ 功能特性

当前已经实现：

- 基于 `AF_UNIX + SOCK_STREAM` 的本地客户端/服务端通信
- 默认监听 `/tmp/runnerd.sock`，也可以通过 `--socket <path>` 指定路径
- `runnerctl ping` 与 `runnerd` 之间的 `PING/PONG` 通信
- 4 字节大端长度前缀，单帧 payload 最大为 64 KiB
- 协议层提供支持拆包和粘包的增量帧解码器
- 将监听 socket 和客户端 socket 设置为非阻塞模式
- 使用 LT（水平触发）`epoll` 管理监听 fd 和多个客户端 fd
- 为每个连接保存独立的解码器、写缓冲和写入偏移
- 跨多次 `epoll` 事件保留不完整帧，并依次处理一次读取中的多个完整帧
- `accept` 和 `read` 持续到 `EAGAIN`；`write` 持续到缓冲区清空或
  返回 `EAGAIN`
- 仅在存在待写数据时监听 `EPOLLOUT`，支持部分写入后继续发送
- 对端半关闭发送方向后，先发送完已经排队的响应再关闭连接
- 将 socket 文件权限限制为 `0600`
- 启动时安全处理同名路径：
  - 已有服务正在监听时拒绝重复启动
  - 同名路径不是 socket 时拒绝删除
  - 失效且属于当前用户的旧 socket 会被清理
- 对被信号中断的 `accept`、`read` 和 `write` 进行重试
- 使用 `SOCK_CLOEXEC` 避免文件描述符被后续程序意外继承
- 忽略 `SIGPIPE`，避免对端断开时进程被信号终止
- 定义 `JobId`、`JobSpec`、`Job` 和 8 种 `JobState`，保存任务标识、
  执行参数、运行状态、进程信息和退出结果
- 校验任务命令不能为空、参数不能包含 NUL 字节，并要求已设置的超时为正数
- 集中检查 10 条合法状态迁移，并提供终态判断
- 使用 CMake 构建，并通过 CTest 运行 smoke test、协议单元测试、
  任务模型单元测试和多客户端集成测试

## 🏗️ 当前架构

```text
runnerctl ping
      │
      │ Unix Domain Socket
      │ /tmp/runnerd.sock（默认）
      ▼
   runnerd
      │
      └── epoll_wait
            ├── listen fd 可读
            │     └── accept，直到 EAGAIN
            └── client fd
                  ├── Connection
                  │     ├── FrameDecoder
                  │     ├── write buffer
                  │     ├── write offset
                  │     └── read closed
                  ├── EPOLLIN
                  │     └── read → 解码请求 → 响应入队
                  ├── EPOLLOUT
                  │     └── 从 write offset 继续发送
                  └── EPOLLRDHUP
                        └── 读完最后输入 → 排空待写响应 → 关闭
```

服务端使用 `Connections` 表按 fd 保存连接状态。读取时持续调用 `read`
直到 `EAGAIN`，完整请求会被解析并将响应追加到写缓冲；存在待写数据时才
注册 `EPOLLOUT`。如果 `write` 只发送了一部分，下一次可写事件会从保存的
偏移继续发送，缓冲区清空后取消 `EPOLLOUT`，避免事件循环空转。

任务模型目前作为独立的 `runnerd_job` 静态库存在：

```text
JobSpec
   │
   └── validateJobSpec
           │
           ▼
      Job（初始状态 QUEUED）
           │
           ├── canTransition：检查迁移是否合法
           ├── transitionJob：执行合法迁移
           └── isTerminal：判断任务是否已经结束
```

这一模型已经完成单元测试，但尚未链接到 `runnerd` 的事件循环。后续实现任务
提交和进程管理时，daemon 才会用它记录真实任务。

## 📁 项目结构

```text
runnerd/
├── CMakeLists.txt
├── include/
│   └── runnerd/
│       ├── job.h
│       ├── protocol.h
│       └── unix_socket.h
├── src/
│   ├── job.cpp
│   ├── protocol.cpp
│   ├── runnerd_main.cpp
│   ├── runnerctl_main.cpp
│   └── unix_socket.cpp
├── tests/
│   ├── job_test.cpp
│   ├── protocol_test.cpp
│   ├── runnerd_integration_test.sh
│   └── smoke_test.cpp
├── docs/
│   ├── requirements.md
│   └── state_machine.md
├── LICENSE
├── README.md
└── README_EN.md
```

## 🚀 编译运行

### 环境要求

- Linux
- 支持 C++17 的 GCC 或 Clang
- CMake 3.16 或更高版本

### 构建项目

```bash
git clone https://github.com/qxf-72/runnerd.git
cd runnerd

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

构建完成后会生成：

```text
build/
├── librunnerd_job.a
├── librunnerd_protocol.a
├── job_test
├── protocol_test
├── runnerd
├── runnerctl
└── smoke_test
```

### 运行示例

命令格式：

```text
runnerd [--socket <path>]
runnerctl [--socket <path>] ping
```

在第一个终端启动服务：

```bash
./build/runnerd
```

预期输出：

```text
runnerd is listening on /tmp/runnerd.sock
```

在第二个终端发送请求：

```bash
./build/runnerctl ping
```

客户端预期输出：

```text
PONG
```

服务端会同时输出：

```text
received PING
```

省略 `--socket` 时，两个程序都使用 `/tmp/runnerd.sock`。如需指定其他
路径，服务端和客户端必须使用相同的参数：

```bash
./build/runnerd --socket /tmp/runnerd-demo.sock
./build/runnerctl --socket /tmp/runnerd-demo.sock ping
```

如果服务异常退出并留下旧的 socket 文件，下次使用相同路径启动时会检查并
安全清理失效文件。

### 运行测试

```bash
cmake -E chdir build ctest --output-on-failure
```

也可以进入构建目录运行：

```bash
cd build
ctest --output-on-failure
```

当前测试包括：

- `smoke_test`：验证基础测试目标能够成功构建和运行
- `protocol_test`：验证大端编码、拆包、粘包、空 payload、二进制
  payload、64 KiB 边界以及非法长度拒绝
- `job_test`：验证合法和非法任务参数、全部合法状态迁移、典型非法迁移，
  以及终态判断
- `runnerd_integration_test`：使用独立临时 socket 启动真实 daemon，
  并发运行 20 个 `runnerctl ping`，验证每个客户端都成功输出 `PONG`

跨事件拆包、半关闭和部分写入等异常场景的集成测试将在后续阶段继续补充。

## 📡 当前协议

Unix Domain Stream Socket 与 TCP 一样不保留消息边界，因此当前协议使用
4 字节大端长度字段来界定每个 payload：

```text
+------------------------+----------------------+
| payload length (4 字节) | payload (变长)       |
| 大端无符号整数          | 最大 64 KiB          |
+------------------------+----------------------+
```

当前请求和响应 payload 为：

```text
request:  "PING"
response: "PONG"
unknown request response: "ERR!"
```

`FrameDecoder` 可以多次接收不完整数据，也可以从一次输入中依次取出多个
完整帧。声明长度超过 64 KiB 的帧会被拒绝。daemon 为每条客户端连接保存
独立的解码器，因此请求帧可以跨越多次 `read` 和多次 `epoll` 事件到达；
同一次读取中的多个完整请求也会被依次处理并生成响应。未知请求会收到
`ERR!`；非法或超长帧会导致服务端关闭对应的客户端连接。

## 🎯 设计边界

项目最终定位为同一用户本机上的任务执行服务，明确不计划支持：

- TCP 远程访问
- 多用户登录和权限系统
- HTTP 或 Web UI
- 容器和 cgroup
- 数据库和分布式 Agent
- 线程池、任务依赖 DAG 和自动重试

这些边界让项目可以集中展示 Linux 进程管理、非阻塞 I/O、事件循环和
崩溃恢复等核心能力。

## 🗺️ 路线图

- [x] 初始化 CMake、CTest、需求文档和状态机文档
- [x] 完成 Unix Domain Socket 与 `PING/PONG` 通信
- [x] 实现长度前缀协议和增量解码
- [x] 使用非阻塞 I/O、连接状态、写缓冲和 `epoll` 支持多个客户端
- [x] 定义任务数据模型、参数校验、状态迁移规则和对应单元测试
- [ ] 实现任务提交、`fork/execve` 和 stdout/stderr 采集
- [ ] 将任务模型接入 daemon，实现并发队列、取消和超时
- [ ] 使用 journal 保存任务历史并支持重启恢复
- [ ] 补充更多异常场景集成测试、Sanitizer 检查和诊断报告

## 📚 文档

- [需求说明](docs/requirements.md)
- [任务状态机](docs/state_machine.md)

## 🤝 Contributing

欢迎提交 Issue 和 Pull Request。

提交代码前，请确保项目能够完成构建和测试：

```bash
cmake --build build
cmake -E chdir build ctest --output-on-failure
```

项目仍处于早期阶段。如果改动涉及协议、状态机或任务生命周期，建议先在
Issue 中说明设计和行为边界。

## 📄 License

本项目使用 [MIT License](LICENSE)。

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
> 状态迁移规则。`runnerctl` 现在可以提交任务，daemon 会校验参数、
> 分配 JobId，并在内存中保存 `QUEUED` 任务；子进程启动尚未实现，因此
> 任务目前不会真正执行。

## ✨ 功能特性

当前已经实现：

- 基于 Unix Domain Socket 的本地客户端/服务端通信，支持自定义 socket 路径
- 使用 4 字节大端长度前缀协议，单帧 payload 最大为 64 KiB
- 增量帧解码，支持拆包、粘包和二进制 payload
- 基于非阻塞 I/O 和 LT（水平触发）`epoll` 的多客户端事件循环
- 每个连接独立维护解码器、写缓冲和连接关闭状态
- 安全管理 socket 文件，并处理 `EINTR`、`SIGPIPE` 和文件描述符继承
- 支持 `runnerctl ping` 和带有可选超时的 `runnerctl submit`
- 提供任务参数校验、JobId 分配、状态机和内存中的 `QUEUED` 任务表
- 使用 GoogleTest 编写单元测试和集成测试，并通过 CTest 统一运行

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

SUBMIT 请求进入 daemon 后的处理流程如下：

```text
SUBMIT payload
      │
      ├── decodeSubmitRequest：恢复 timeout 和 argv
      ├── validateJobSpec：检查绝对路径、NUL 和超时
      ▼
Job{id, spec, state = QUEUED}
      │
      ├── 保存到 Jobs[job_id]
      └── 返回 "OK <job_id>"
```

`Connections` 表管理客户端连接，连接关闭后对应状态会被删除；`Jobs` 表与
daemon 生命周期相同，因此提交任务的客户端断开后，任务仍会保留。当前任务
不会离开 `QUEUED`，daemon 重启后 JobId 会从 1 重新开始，内存任务记录也会
丢失。这些限制会在后续的进程执行和 journal 阶段解决。

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
│   ├── runnerd_integration_test.cpp
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

测试使用 GoogleTest 1.17.0。首次启用测试进行 CMake 配置时，
`FetchContent` 会自动下载并校验对应源码，因此需要能够访问 GitHub；
后续配置会复用构建目录中已经下载的依赖。

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
├── runnerd_integration_test
├── runnerctl
└── smoke_test
```

### 运行示例

命令格式：

```text
runnerd [--socket <path>]
runnerctl [--socket <path>] ping
runnerctl [--socket <path>] submit [--timeout <milliseconds>] \
          -- <absolute-path> [arguments...]
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

提交一个没有超时的任务：

```bash
./build/runnerctl submit -- /bin/echo hello
```

客户端会输出 daemon 分配的 JobId，例如：

```text
1
```

也可以提交带有正数毫秒超时的任务：

```bash
./build/runnerctl submit --timeout 5000 -- /bin/sleep 1
```

`--` 表示 `runnerctl` 自身的选项到此结束，后面的内容全部属于任务 argv。
`argv[0]` 必须是绝对路径，所以 `echo hello` 和 `./echo hello` 会被拒绝。
当前版本只保存 `QUEUED` 任务，不会执行命令；提交 `/bin/echo hello` 后看不到
`hello` 输出属于预期行为。

如果服务异常退出并留下旧的 socket 文件，下次使用相同路径启动时会检查并
安全清理失效文件。

### 运行测试

所有测试均使用 GoogleTest 编写。CMake 的 `gtest_discover_tests()` 会把
每个 GoogleTest 用例分别注册给 CTest，因此失败输出能够直接指出具体用例。

```bash
cmake -E chdir build ctest --output-on-failure
```

也可以进入构建目录运行：

```bash
cd build
ctest --output-on-failure
```

当前测试目标包括：

- `smoke_test`：验证基础测试目标能够成功构建和运行
- `protocol_test`：验证大端编码、拆包、粘包、空 payload、二进制
  payload、64 KiB 边界、SUBMIT 编解码以及畸形 SUBMIT 拒绝
- `job_test`：验证合法和非法任务参数、全部合法状态迁移、典型非法迁移，
  终态判断和绝对程序路径约束
- `runnerd_integration_test`：使用独立临时 socket 启动真实 daemon，
  验证 20 个并发 `PING`、连续提交时 JobId 递增、正数超时传递、相对路径
  拒绝，以及提交后 `PING` 仍然可用

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

PING 请求和响应 payload 为：

```text
request:  "PING"
response: "PONG"
```

SUBMIT 请求的 payload 格式为：

```text
+----------------------+--------------------------------------+
| 字段                 | 含义                                 |
+----------------------+--------------------------------------+
| "SUBMIT"             | 6 字节请求标记                       |
| timeout_ms           | 4 字节大端整数；0 表示不设置超时     |
| argc                 | 4 字节大端参数数量                   |
| argument_length      | 4 字节大端参数长度                   |
| argument             | 对应长度的原始参数字节               |
| ...                  | 后续参数重复“长度 + 内容”             |
+----------------------+--------------------------------------+
```

成功提交返回 `"OK <job_id>"`；无法识别的请求或非法 SUBMIT 返回
`"ERR <message>"`。客户端显示 JobId 时只输出数字，错误信息写入标准错误。

`FrameDecoder` 可以多次接收不完整数据，也可以从一次输入中依次取出多个
完整帧。声明长度超过 64 KiB 的帧会被拒绝。daemon 为每条客户端连接保存
独立的解码器，因此请求帧可以跨越多次 `read` 和多次 `epoll` 事件到达；
同一次读取中的多个完整请求也会被依次处理并生成响应。未知请求会收到
`ERR` 响应；外层帧非法或超长时，服务端会关闭对应的客户端连接。

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

- [x] 初始化 CMake、GoogleTest、CTest、需求文档和状态机文档
- [x] 完成 Unix Domain Socket 与 `PING/PONG` 通信
- [x] 实现长度前缀协议和增量解码
- [x] 使用非阻塞 I/O、连接状态、写缓冲和 `epoll` 支持多个客户端
- [x] 定义任务数据模型、参数校验、状态迁移规则和对应单元测试
- [x] 实现 SUBMIT 编解码、JobId 分配和内存中的 `QUEUED` 任务表
- [ ] 使用 `fork/execve` 启动任务并采集 stdout/stderr
- [ ] 实现并发队列、取消和超时
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

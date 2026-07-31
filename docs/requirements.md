# runnerd 需求说明

## 项目目标

runnerd 是一个面向同一用户、本机运行的任务执行守护服务。

客户端 runnerctl 通过 Unix Domain Socket 向 runnerd 提交任务、
查询任务状态、列出任务以及取消任务。

默认通信路径为 `/tmp/runnerd.sock`，服务端和客户端都可以使用
`--socket <path>` 指定其他路径。

## 当前实现进度

当前已经实现 Unix Domain Socket 通信、长度前缀协议、非阻塞 `epoll`
事件循环、连接读写状态、并发客户端测试、任务数据模型、状态机规则、
进程启动、输出采集和子进程回收。

`runnerctl` 已支持提交任务。runnerd 会解码并校验 `JobSpec`、分配 JobId，
将任务保存到内存表并立即使用 `fork/execve` 启动。stdout/stderr 通过
非阻塞 pipe 接入 `epoll`，`SIGCHLD` 通过 `signalfd` 接入事件循环，退出
结果由 `waitpid` 结算。`status` 可以查询单个任务，`list` 会按 JobId
顺序列出内存中的任务。当前没有 `cancel` 命令和并发上限；daemon 重启后
内存任务、输出及 JobId 计数都会丢失。

## 第一阶段核心功能

### 通信与协议

- [x] Unix Domain Socket 本地通信
- [x] 4 字节大端长度前缀协议
- [x] 非阻塞 `epoll` 多连接事件循环
- [x] SUBMIT 请求、响应和 argv 长度字段编解码
- [x] 查询和列表任务的协议消息
- [ ] 取消任务的协议消息

### 任务模型与执行

- [x] `JobSpec`、`Job` 和 `JobState` 数据模型
- [x] 任务参数校验和状态迁移规则
- [x] JobId 分配和内存任务表
- [x] 使用 `fork + execve` 启动任务
- [x] 采集 stdout 和 stderr
- [x] 使用 `signalfd` 接收 `SIGCHLD` 并通过 `waitpid` 回收子进程

### 调度与可靠性

- [ ] 最大并发数和等待队列
- [ ] 执行超时和手动取消
- [ ] 任务历史持久化
- [ ] runnerd 重启后的任务状态恢复

### 测试

- [x] 使用 GoogleTest 编写 C++ 单元测试和集成测试，并通过 CTest 运行
- [x] 协议单元测试
- [x] 任务模型单元测试
- [x] 20 个并发客户端的 `PING/PONG` 集成测试
- [x] SUBMIT 编解码和顺序提交集成测试
- [x] STATUS 编解码及 STATUS/LIST 集成测试
- [x] 进程启动、输出采集、execve 失败和并发回收测试
- [ ] 任务取消、超时和恢复的集成测试

## 任务参数约束

- `argv` 至少包含一个元素，`argv[0]` 必须是绝对程序路径。
- 当前只检查 `argv[0]` 是否以 `/` 开头；路径是否存在、是否可以执行，
  将由后续的 `execve` 结果决定。
- 每个参数都不能包含 NUL 字节，避免传给 `execve` 时被提前截断。
- `execution_timeout` 为空时表示不设置执行超时。
- 如果设置 `execution_timeout`，其毫秒数必须大于 0。
- 当前协议使用 `uint32_t` 传输毫秒超时，因此最大值为 `UINT32_MAX`。

## 当前 SUBMIT 行为

- 命令格式为
  `runnerctl [--socket <path>] submit [--timeout <ms>] -- <path> [args...]`。
- `--` 后面的参数会保持各自边界，不会拼接成 shell 命令。
- 服务端再次校验客户端输入，不依赖 `runnerctl` 的本地校验。
- 合法任务获得从 1 开始递增的 JobId，以 `QUEUED` 状态保存后立即尝试启动。
- 启动成功后进入 `RUNNING`；正常退出码为 0 时进入 `SUCCEEDED`，否则进入
  `FAILED`。
- stdout/stderr 当前保存在内存中，尚不能通过 `runnerctl` 查询，也没有
  输出大小上限。
- 客户端连接关闭不会删除或终止任务。
- timeout 当前只被保存，不会触发计时或终止。
- 当前没有调度队列和并发限制，每个合法任务都会立即尝试启动。
- 所有任务、输出和结果仍只存在于 daemon 内存中。

## 当前查询行为

- `runnerctl status <job_id>` 查询单个任务。响应始终包含 JobId 和当前状态；
  如果对应字段已经产生，还会包含 `timeout_ms`、PID、退出码、退出信号和
  失败信息。
- `ProcessMonitor` 在任务最终结算时才把输出移动到 `Job`，因此只有终态任务的
  STATUS 响应会显示完整的 stdout/stderr 字节数。
- `runnerctl list` 按 JobId 升序输出内存中的任务，只显示 JobId 和状态；
  没有任务时客户端输出 `No jobs`。
- LIST 当前没有分页。完整响应如果超过 64 KiB 单帧上限，服务端会返回
  `ERR too many jobs to list`，不会返回残缺列表。
- `STATUS` 和 `LIST` 只能查询当前 daemon 内存中的任务；重启后不能查询旧任务。

## 任务结果

每个任务保存唯一的任务 ID、原始执行参数和当前状态。启动后记录子进程 ID、
进程组 ID、stdout/stderr，以及最终的退出码或退出信号。无法启动或执行任务
时保存失败阶段和系统错误信息。终止原因将在取消和超时功能接入后使用。

## 明确不做

- TCP 远程访问
- HTTP 接口和 Web UI
- 多用户和权限系统
- 容器和 cgroup
- 数据库
- 分布式执行
- 线程池
- 任务依赖 DAG
- 自动重试

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
进程启动、输出采集、子进程回收、FIFO 调度器、运行中取消和执行超时。

`runnerctl` 已支持提交、查询、列出以及取消排队或运行中的任务。runnerd 会解码并校验
`JobSpec`、分配 JobId，将任务保存到内存表并加入 FIFO 队列。
`--max-running` 控制并发运行槽位，
默认值为 `1`；超出容量的任务保持 `QUEUED`。stdout/stderr 通过非阻塞 pipe
接入 `epoll`，`SIGCHLD` 通过 `signalfd` 接入事件循环，退出结果由 `waitpid`
结算。任务最终结算后，daemon 会释放运行槽位并自动启动 FIFO 队首。
`status` 可以查询单个任务，`list` 会按 JobId 顺序列出内存中的任务。

运行中取消和执行超时都会先向整个任务进程组发送 `SIGTERM`。任务如果没有在固定的
1 秒宽限期内退出，daemon 会再向进程组发送 `SIGKILL`。直接子进程退出且三个输出
管道 EOF 后，任务根据最初的终止原因结算为 `CANCELLED` 或 `TIMED_OUT`。
daemon 重启后，内存任务、输出及 JobId 计数都会丢失。

## 第一阶段核心功能

### 通信与协议

- [x] Unix Domain Socket 本地通信
- [x] 4 字节大端长度前缀协议
- [x] 非阻塞 `epoll` 多连接事件循环
- [x] SUBMIT 请求、响应和 argv 长度字段编解码
- [x] 查询和列表任务的协议消息
- [x] CANCEL 请求、响应和 JobId 编解码

### 任务模型与执行

- [x] `JobSpec`、`Job` 和 `JobState` 数据模型
- [x] 任务参数校验和状态迁移规则
- [x] JobId 分配和内存任务表
- [x] 使用 `fork + execve` 启动任务
- [x] 采集 stdout 和 stderr
- [x] 使用 `signalfd` 接收 `SIGCHLD` 并通过 `waitpid` 回收子进程

### 调度与可靠性

- [x] `--max-running`、FIFO 等待队列、终态槽位释放和队列自动推进
- [x] 从 FIFO 等待队列取消尚未启动的任务
- [x] 运行中任务的进程组 `SIGTERM` 终止
- [x] 基于最小堆和单个 `timerfd` 的执行超时
- [x] 固定宽限期后的进程组 `SIGKILL` 强制终止
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
- [x] FIFO 调度器单元测试，以及最大并发、自动推进和失败恢复调度集成测试
- [x] CANCEL 协议、命令行校验和排队取消集成测试
- [x] 运行中取消、进程组终止和自然退出竞争测试
- [x] TimeoutManager 单元测试和真实 daemon 超时集成测试
- [x] 宽限期退出、`SIGKILL` 升级、槽位释放及取消/超时竞争测试
- [ ] 持久化与重启恢复测试

## 当前 daemon 启动参数

- 命令格式为
  `runnerd [--socket <path>] [--max-running <positive-integer>]`。
- `--socket` 默认使用 `/tmp/runnerd.sock`。
- `--max-running` 默认使用 `1`，只接受不超出 `std::size_t` 的正整数。
- 两个选项可以按任意顺序出现，但同一选项不能重复指定。

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
- 合法任务获得从 1 开始递增的 JobId，以 `QUEUED` 状态保存并加入 FIFO
  等待队列；存在空闲槽位时立即尝试启动。
- 启动成功后进入 `RUNNING`；正常退出码为 0 时进入 `SUCCEEDED`，否则进入
  `FAILED`。
- stdout/stderr 当前保存在内存中，尚不能通过 `runnerctl` 查询，也没有
  输出大小上限。
- 客户端连接关闭不会删除或终止任务。
- 任务成功进入 `RUNNING` 后，daemon 才会登记 `execution_timeout`；FIFO
  排队时间不计入执行时间。
- 运行任务最终结算后，`ProcessMonitor` 会通知 daemon 归还调度器槽位；daemon
  随后继续调度，按 FIFO 顺序启动下一个等待任务。
- 如果任务在同步启动阶段失败，daemon 会立即归还槽位并继续调度后续任务。
- 所有任务、输出和结果仍只存在于 daemon 内存中。

## 当前超时行为

- `TimeoutManager` 使用 `CLOCK_MONOTONIC`、一个非阻塞且带 `CLOEXEC` 的
  `timerfd`，以及一个最小堆管理所有任务期限。
- `RUNNING` 任务的期限表示 execution deadline；`TERMINATING` 任务的期限表示
  SIGKILL deadline。同一个 JobId 同一时刻只保留一个有效期限。
- timerfd 始终设置为最近的有效 deadline。相同 JobId 重新登记期限时使用
  generation 使旧堆节点失效；任务自然结束或取消期限时使用惰性删除。
- execution deadline 到达时，daemon 向整个进程组发送 `SIGTERM`，执行
  `RUNNING -> TERMINATING`，记录 `TerminationCause::kTimedOut`，并登记 1 秒后的
  SIGKILL deadline。
- 如果任务在宽限期内退出，则不发送 `SIGKILL`；如果宽限期到达时任务仍未完成，
  daemon 会向整个进程组发送 `SIGKILL`，同时保留原来的状态和终止原因。
- 只有直接子进程已经退出，stdout、stderr 和 startup-error 三个管道全部 EOF 后，
  任务才从 `TERMINATING` 结算为 `TIMED_OUT`，然后释放运行槽位并启动 FIFO 队首。
- 自然退出与 deadline 同时到达时，`ProcessMonitor` 会在发送信号前使用非阻塞
  `waitpid` 检查真实退出结果，避免把已经完成的任务错误标记为 `TIMED_OUT`。

## 当前 CANCEL 行为

- 命令格式为 `runnerctl [--socket <path>] cancel <job_id>`；JobId 必须是
  `uint64_t` 范围内的大于 0 的十进制整数。
- 线协议 payload 为 `CANCEL` 加 8 字节大端 JobId；长度、前缀或 JobId
  不合法时返回明确错误。
- `QUEUED` 任务取消时，daemon 会先从 `JobScheduler` 的 FIFO 等待队列中移除
  该任务，再执行 `QUEUED -> CANCELLED`，避免任务进入终态后仍被启动。服务端
  返回 `OK cancelled`，客户端输出 `Cancelled job <job_id>`。
- `RUNNING` 任务取消时，`ProcessMonitor` 向负进程组 ID 发送 `SIGTERM`，执行
  `RUNNING -> TERMINATING` 并记录 `TerminationCause::kCancelled`，同时用 1 秒后的
  SIGKILL deadline 替换原 execution deadline。服务端返回 `OK terminating`，
  客户端输出 `Cancellation requested for job <job_id>`。
- 任务在宽限期内退出时不发送 `SIGKILL`；否则宽限期结束后强制终止整个进程组。
  三个管道 EOF 且直接子进程退出后，任务结算为 `CANCELLED`。
- 运行中取消成功后，原执行期限会立即失效，后续不能再把任务改成 `TIMED_OUT`。
- 任务不存在、已经进入终态或正在终止时，取消请求会返回错误且不改变状态。
  重复取消当前也返回“任务已是终态”，因此该接口不是幂等成功接口。
- 如果直接子进程在取消信号发送前已经自然退出，`ProcessMonitor` 会保留真实退出
  结果，不会把任务错误标记为 `CANCELLED`。
- 取消、超时和自然退出发生竞争时，第一个成功改变任务状态的事件决定结果；进入
  `TERMINATING` 后，后续请求不能覆盖已经记录的 `TerminationCause`。

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
时保存失败阶段和系统错误信息。运行中取消或执行超时时还会记录终止原因，用于
在进程退出并排空输出后选择 `CANCELLED` 或 `TIMED_OUT`。

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

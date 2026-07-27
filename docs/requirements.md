# runnerd 需求说明

## 项目目标

runnerd 是一个面向同一用户、本机运行的任务执行守护服务。

客户端 runnerctl 通过 Unix Domain Socket 向 runnerd 提交任务、
查询任务状态、列出任务以及取消任务。

默认通信路径为 `/tmp/runnerd.sock`，服务端和客户端都可以使用
`--socket <path>` 指定其他路径。

## 当前实现进度

当前已经实现 Unix Domain Socket 通信、长度前缀协议、非阻塞 `epoll`
事件循环、连接读写状态、并发客户端测试、任务数据模型和状态机规则。

`runnerctl` 已支持提交任务。runnerd 会解码并校验 `JobSpec`、分配 JobId，
并将任务以 `QUEUED` 状态保存到内存任务表。当前尚未启动子进程，也没有
`status`、`list` 和 `cancel` 命令；daemon 重启后内存任务及 JobId 计数会
丢失。

## 第一阶段核心功能

### 通信与协议

- [x] Unix Domain Socket 本地通信
- [x] 4 字节大端长度前缀协议
- [x] 非阻塞 `epoll` 多连接事件循环
- [x] SUBMIT 请求、响应和 argv 长度字段编解码
- [ ] 查询、列表和取消任务的协议消息

### 任务模型与执行

- [x] `JobSpec`、`Job` 和 `JobState` 数据模型
- [x] 任务参数校验和状态迁移规则
- [x] JobId 分配和内存中的 `QUEUED` 任务表
- [ ] 使用 `fork + execve` 启动任务
- [ ] 采集 stdout 和 stderr
- [ ] 使用 `SIGCHLD` 回收子进程

### 调度与可靠性

- [ ] 最大并发数和等待队列
- [ ] 执行超时和手动取消
- [ ] 任务历史持久化
- [ ] runnerd 重启后的任务状态恢复

### 测试

- [x] 协议单元测试
- [x] 任务模型单元测试
- [x] 20 个并发客户端的 `PING/PONG` 集成测试
- [x] SUBMIT 编解码和顺序提交集成测试
- [ ] 任务执行、取消、超时和恢复的集成测试

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
- 合法任务获得从 1 开始递增的 JobId，并以 `QUEUED` 状态保存。
- 客户端连接关闭不会删除任务。
- timeout 当前只被保存，不会触发计时或终止。
- 任务当前不会执行，所有记录只存在于 daemon 内存中。

## 任务结果

每个任务需要保存唯一的任务 ID、原始执行参数和当前状态。任务启动后还需要
记录子进程 ID、进程组 ID、终止原因，以及最终的退出码或退出信号。无法
启动或执行任务时，应保存便于诊断的错误信息。

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

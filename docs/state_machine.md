# 任务状态机

任务状态机用于限制任务生命周期中的状态变化。目前状态定义、迁移检查和
单元测试已经实现，并已接入 SUBMIT 流程：每个合法提交都会创建一个初始
状态为 `QUEUED` 的任务并加入 `JobScheduler`。获得运行槽位的任务由
`ProcessMonitor` 启动，并根据子进程结果迁移到 `RUNNING`、`SUCCEEDED`
或 `FAILED`；没有空闲槽位的任务继续保持 `QUEUED`。

## 状态说明

| 状态 | 是否终态 | 含义 |
| --- | --- | --- |
| `QUEUED` | 否 | 任务已经创建，正在等待启动 |
| `RUNNING` | 否 | 子进程已经启动，任务正在执行 |
| `TERMINATING` | 否 | 已经请求终止，正在等待子进程真正退出 |
| `SUCCEEDED` | 是 | 任务执行成功 |
| `FAILED` | 是 | 任务启动失败或执行失败 |
| `CANCELLED` | 是 | 任务因用户主动取消而结束 |
| `TIMED_OUT` | 是 | 任务因超过执行时间限制而结束 |
| `INTERRUPTED` | 是 | 未来恢复流程中，runnerd 发现任务此前没有进入终态 |

## 合法迁移

```text
QUEUED ────────> RUNNING ────────> SUCCEEDED
   │                ├────────────> FAILED
   │                └────────────> TERMINATING ─────> CANCELLED
   │                                      └─────────> TIMED_OUT
   ├──────────────> FAILED
   └──────────────> CANCELLED

QUEUED ────────────> INTERRUPTED
RUNNING ───────────> INTERRUPTED
TERMINATING ───────> INTERRUPTED
```

完整迁移列表：

```text
QUEUED -> RUNNING
QUEUED -> FAILED
QUEUED -> CANCELLED
QUEUED -> INTERRUPTED

RUNNING -> SUCCEEDED
RUNNING -> FAILED
RUNNING -> TERMINATING
RUNNING -> INTERRUPTED

TERMINATING -> CANCELLED
TERMINATING -> TIMED_OUT
TERMINATING -> INTERRUPTED
```

## 状态机规则

- `QUEUED -> CANCELLED` 表示任务尚未启动时就被用户取消。
- `QUEUED -> FAILED` 表示在进入运行状态前创建 pipe、fork 或监控注册失败。
- 运行中的任务不能直接进入 `CANCELLED` 或 `TIMED_OUT`。runnerd 需要先
  请求终止子进程并进入 `TERMINATING`，等待子进程退出后再确定最终状态。
- `TerminationCause` 记录进入 `TERMINATING` 的原因，目前分为用户取消和
  执行超时。
- 超时从任务进入 `RUNNING` 时开始计算，不包含排队时间。
- `SUCCEEDED`、`FAILED`、`CANCELLED`、`TIMED_OUT` 和 `INTERRUPTED`
  都是终态，不能再迁移到其他状态。
- runnerd 重启恢复任务历史时，之前仍处于 `QUEUED`、`RUNNING` 或
  `TERMINATING` 的任务会变为 `INTERRUPTED`；恢复流程目前尚未实现。

## 当前代码接口

- `jobStateName(state)`：返回用于日志和协议的大写状态名称。
- `isTerminal(state)`：判断一个状态是否为终态。
- `canTransition(from, to)`：检查迁移是否合法，但不修改任务。
- `transitionJob(job, to)`：执行迁移；非法迁移会抛出 `std::logic_error`，
  并保持原状态不变。
- `validateJobSpec(spec)`：检查命令参数和超时设置；参数不合法时抛出
  `std::invalid_argument`。当前还要求 `argv[0]` 必须是绝对路径。

`transitionJob` 只负责校验并更新 `Job::state`。`ProcessMonitor` 负责填写
PID、进程组、stdout/stderr、退出码、退出信号和失败信息；取消与超时功能
接入后再填写终止原因。

## 当前运行时行为

- 合法 SUBMIT 会分配 JobId，并创建一个 `QUEUED` 任务。
- 任务保存在 daemon 级别的 `Jobs` 表中，不属于某一条客户端连接。
- `--max-running` 设置运行槽位数，默认值为 `1`；超出容量的任务按 FIFO
  顺序保持 `QUEUED`。
- 运行任务最终结算后，`ProcessMonitor` 会通知 daemon 释放运行槽位；daemon
  随后继续调度，按 FIFO 顺序启动下一个等待任务。
- 如果任务在同步启动阶段失败，daemon 会立即释放槽位并继续调度后续任务。
- 客户端断开后任务仍然执行并保留。
- 任务启动成功后进入 `RUNNING`；退出码为 0 时进入 `SUCCEEDED`，非零
  退出、信号终止或启动失败时进入 `FAILED`。
- `runnerctl status <job_id>` 可以查询任务当前状态和已经产生的运行结果；
  `runnerctl list` 按 JobId 升序列出内存任务。
- stdout/stderr 在任务最终结算后保存在任务对象中，尚不能通过客户端查询
  正文。
- 因为尚未实现 journal，daemon 重启后内存任务会丢失。

## 测试覆盖

`tests/job_test.cpp` 使用 GoogleTest，当前覆盖：

- 不设置超时和设置正数超时的合法任务参数
- 空 `argv`、空 `argv[0]`、相对程序路径、含 NUL 参数、零超时和负超时
- 当前定义的全部 11 条合法迁移
- 典型非法迁移，并验证拒绝后任务状态保持不变
- 全部终态和非终态的判断

`tests/process_monitor_test.cpp` 还覆盖成功执行、stdout/stderr 采集、
execve 失败、非零退出、大输出排空和多个子进程同时回收。

`tests/job_scheduler_test.cpp` 覆盖非法最大并发数、FIFO 启动顺序、并发槽位、
终态任务释放槽位、排队任务移除和重复调度保护。

`tests/runnerd_integration_test.cpp` 通过真实 daemon 和 runnerctl 覆盖成功
任务与失败任务的 STATUS 查询、非零退出码、stdout/stderr 字节数、LIST
顺序、不存在的 JobId、非法 STATUS/LIST 参数，以及 `--max-running` 为 `1`
或 `2` 时的并发限制和非法并发参数。还覆盖成功退出、非零退出及 `execve`
失败后的队列自动推进、FIFO 启动顺序，以及多个任务快速退出时槽位不会泄漏
或重复释放。

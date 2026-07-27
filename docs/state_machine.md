# 任务状态机

任务状态机用于限制任务生命周期中的状态变化。目前状态定义、迁移检查和
单元测试已经实现，但尚未接入 runnerd 的事件循环和任务执行流程。

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
   │                │ └──────────> FAILED
   │                │
   │                └────────────> TERMINATING ─────> CANCELLED
   │                                      └─────────> TIMED_OUT
   │
   └──────────> CANCELLED

QUEUED ────────────> INTERRUPTED
RUNNING ───────────> INTERRUPTED
TERMINATING ───────> INTERRUPTED
```

完整迁移列表：

```text
QUEUED -> RUNNING
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
  `std::invalid_argument`。

当前 `transitionJob` 只负责校验并更新 `Job::state`，不会自动设置进程信息、
终止原因、退出结果或错误信息。这些字段将在任务模型接入实际进程管理后由
对应流程填写。

## 测试覆盖

`tests/job_test.cpp` 当前覆盖：

- 不设置超时和设置正数超时的合法任务参数
- 空 `argv`、空 `argv[0]`、含 NUL 参数、零超时和负超时
- 当前定义的全部 10 条合法迁移
- 典型非法迁移，并验证拒绝后任务状态保持不变
- 全部终态和非终态的判断

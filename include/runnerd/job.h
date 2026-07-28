#ifndef RUNNERD_JOB_H
#define RUNNERD_JOB_H

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace runnerd {

// JobId 用于在服务端唯一标识一个任务。
using JobId = std::uint64_t;

// 项目内部统一使用毫秒表示任务执行超时。
using JobTimeout = std::chrono::milliseconds;

// 任务从排队到结束所经历的状态。
enum class JobState {
  kQueued,       // 已创建，正在等待启动。
  kRunning,      // 子进程已经启动，任务正在执行。
  kTerminating,  // 已请求终止，正在等待子进程退出。
  kSucceeded,    // 任务执行成功。
  kFailed,       // 任务启动失败或执行失败。
  kCancelled,    // 任务被主动取消。
  kTimedOut,     // 任务因执行超时而终止。
  kInterrupted,  // 任务因服务退出等外部原因中断。
};

// 记录任务进入 kTerminating 的原因，用于在子进程退出后确定最终状态。
enum class TerminationCause {
  kCancelled,
  kTimedOut,
};

// 返回便于日志和协议输出使用的大写状态名称。
std::string_view jobStateName(JobState state);

// 判断任务是否已经进入不会再发生状态迁移的终态。
bool isTerminal(JobState state);

// 只检查状态迁移是否合法，不会修改任务。
bool canTransition(JobState from, JobState to);

// 创建任务时由客户端提供的执行参数。
struct JobSpec {
  // argv[0] 是要执行的程序路径；后续元素是参数。
  std::vector<std::string> argv;

  // nullopt 表示不设置执行超时。
  std::optional<JobTimeout> execution_timeout;
};

// 校验任务参数；参数不合法时抛出 std::invalid_argument。
void validateJobSpec(const JobSpec& spec);

// 服务端保存的任务信息，包含任务配置、运行状态和最终结果。
struct Job {
  // 任务标识、原始执行参数和当前状态。
  JobId id = 0;
  JobSpec spec;
  JobState state = JobState::kQueued;

  // fork 成功后记录子进程 ID 及其进程组 ID。
  std::optional<pid_t> pid;
  std::optional<pid_t> process_group_id;

  // 任务进入 kTerminating 时记录原因。
  std::optional<TerminationCause> termination_cause;

  // 子进程正常退出时记录退出码，被信号终止时记录信号编号；两者只会设置一个。
  std::optional<int> exit_code;
  std::optional<int> exit_signal;

  // 保存无法启动或执行任务时的错误说明，例如 execve failed: No such file or directory。
  std::string failure_message;

  // 本阶段先将输出保存在内存中；输出上限将在后续阶段加入。
  std::string standard_output;
  std::string standard_error;
};

// 执行一次状态迁移；迁移不合法时抛出 std::logic_error，且不会修改原状态。
void transitionJob(Job& job, JobState to);

}  // namespace runnerd

#endif  // RUNNERD_JOB_H

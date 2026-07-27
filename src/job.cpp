#include "runnerd/job.h"

#include <stdexcept>

namespace runnerd {

std::string_view jobStateName(JobState state) {
  // 返回字符串字面量的只读视图，不产生额外的字符串拷贝。
  switch (state) {
    case JobState::kQueued:
      return "QUEUED";
    case JobState::kRunning:
      return "RUNNING";
    case JobState::kTerminating:
      return "TERMINATING";
    case JobState::kSucceeded:
      return "SUCCEEDED";
    case JobState::kFailed:
      return "FAILED";
    case JobState::kCancelled:
      return "CANCELLED";
    case JobState::kTimedOut:
      return "TIMED_OUT";
    case JobState::kInterrupted:
      return "INTERRUPTED";
  }

  // 防止调用者通过强制类型转换传入枚举定义之外的值。
  throw std::invalid_argument("unknown job state");
}

bool isTerminal(JobState state) {
  switch (state) {
    // 这些状态表示任务已经结束，不允许再迁移到其他状态。
    case JobState::kSucceeded:
    case JobState::kFailed:
    case JobState::kCancelled:
    case JobState::kTimedOut:
    case JobState::kInterrupted:
      return true;

    // 这些状态仍处于任务生命周期中。
    case JobState::kQueued:
    case JobState::kRunning:
    case JobState::kTerminating:
      return false;
  }

  throw std::invalid_argument("unknown job state");
}

bool canTransition(JobState from, JobState to) {
  switch (from) {
    case JobState::kQueued:
      // 排队中的任务可以开始执行，也可以在启动前被取消或中断。
      return to == JobState::kRunning || to == JobState::kCancelled || to == JobState::kInterrupted;

    case JobState::kRunning:
      // 运行中的任务可能直接结束，也可能先进入等待终止的中间状态。
      return to == JobState::kSucceeded || to == JobState::kFailed ||
             to == JobState::kTerminating || to == JobState::kInterrupted;

    case JobState::kTerminating:
      // 子进程退出后，根据终止原因确定最终状态。
      return to == JobState::kCancelled || to == JobState::kTimedOut ||
             to == JobState::kInterrupted;

    // 终态没有任何后继状态。
    case JobState::kSucceeded:
    case JobState::kFailed:
    case JobState::kCancelled:
    case JobState::kTimedOut:
    case JobState::kInterrupted:
      return false;
  }

  throw std::invalid_argument("unknown job state");
}

void validateJobSpec(const JobSpec& spec) {
  // 没有 argv[0] 就无法确定要执行哪个程序。
  if (spec.argv.empty()) {
    throw std::invalid_argument("job argv must not be empty");
  }

  // exec 系列接口要求 argv[0] 至少包含程序名或程序路径。
  if (spec.argv.front().empty()) {
    throw std::invalid_argument("job argv[0] must not be empty");
  }

  // runnerd 只接受绝对路径。
  if (spec.argv.front().front() != '/') {
    throw std::invalid_argument("job argv[0] must be an absolute path");
  }

  for (const std::string& argument : spec.argv) {
    // exec 接口使用 C 字符串，参数中的 NUL 会导致内容被提前截断。
    if (argument.find('\0') != std::string::npos) {
      throw std::invalid_argument("job argument contains a NUL byte");
    }
  }

  // nullopt 表示不限制时间；一旦设置，超时值必须大于 0。
  if (spec.execution_timeout.has_value() && spec.execution_timeout->count() <= 0) {
    throw std::invalid_argument("execution timeout must be positive");
  }
}

void transitionJob(Job& job, JobState to) {
  // 统一通过 canTransition 检查，避免业务代码绕过状态机规则。
  if (!canTransition(job.state, to)) {
    throw std::logic_error("invalid job state transition from " +
                           std::string(jobStateName(job.state)) + " to " +
                           std::string(jobStateName(to)));
  }

  job.state = to;
}

}  // namespace runnerd

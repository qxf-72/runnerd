#ifndef RUNNERD_PROCESS_MONITOR_H
#define RUNNERD_PROCESS_MONITOR_H

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "runnerd/job.h"

namespace runnerd {

using JobTable = std::unordered_map<JobId, Job>;

class ProcessMonitor {
 public:
  ProcessMonitor(int epoll_fd, JobTable& jobs);
  ~ProcessMonitor();

  ProcessMonitor(const ProcessMonitor&) = delete;
  ProcessMonitor& operator=(const ProcessMonitor&) = delete;

  // 启动已存在的 QUEUED 任务。
  // 启动失败会把 Job 标记为 FAILED，而不是让 daemon 退出。
  void startJob(JobId job_id);

  // 请求终止一个正在运行的任务。
  //
  // 返回 true 表示已经向进程组发送 SIGTERM，任务进入 TERMINATING。
  // 返回 false 表示直接子进程在信号发送前已经退出，任务会按照真实退出结果结算。
  bool requestTerminate(JobId job_id, TerminationCause cause);

  bool ownsFileDescriptor(int fd) const;

  // 处理 stdout、stderr、startup error pipe 或 signalfd 事件。
  void handleFileDescriptorEvent(int fd, std::uint32_t event_mask);

  using TerminalJobCallback = std::function<void(JobId)>;

  // 只能在尚未启动任何任务时设置一次。
  //
  // 注意：
  // - launchProcess() 等同步启动失败不会触发这个回调；
  //   调用 startJob() 的 daemon 会立即观察到 FAILED，并自行释放槽位。
  // - 已经启动的任务在 SIGCHLD / pipe EOF 后最终结算时，
  //   才会触发这个回调。
  void setTerminalJobCallback(TerminalJobCallback callback);

 private:
  TerminalJobCallback terminal_job_callback_;
  enum class ProcessFdKind {
    kStdout,
    kStderr,
    kStartupError,
  };

  struct TrackedFd {
    JobId job_id;
    ProcessFdKind kind;
  };

  struct ActiveProcess {
    ActiveProcess(pid_t child_pid, pid_t child_process_group_id, int child_stdout_fd,
                  int child_stderr_fd, int child_startup_error_fd)
        : pid(child_pid),
          process_group_id(child_process_group_id),
          stdout_fd(child_stdout_fd),
          stderr_fd(child_stderr_fd),
          startup_error_fd(child_startup_error_fd) {}

    pid_t pid = -1;
    pid_t process_group_id = -1;

    int stdout_fd = -1;
    int stderr_fd = -1;
    int startup_error_fd = -1;

    bool stdout_eof = false;
    bool stderr_eof = false;
    bool startup_error_eof = false;

    bool child_exited = false;
    int wait_status = 0;

    std::string standard_output;
    std::string standard_error;
    std::string startup_error_bytes;
  };

  int epoll_fd_;
  JobTable& jobs_;
  int sigchld_fd_ = -1;

  std::unordered_map<JobId, ActiveProcess> active_jobs_;
  std::unordered_map<pid_t, JobId> pid_to_job_;
  std::unordered_map<int, TrackedFd> tracked_fds_;

  void registerProcessFd(int fd, JobId job_id, ProcessFdKind kind);
  void closeTrackedFd(int fd);

  void handleSigchld();
  void reapExitedChildren();

  void drainProcessFd(int fd);
  void tryFinalizeJob(JobId job_id);

  void failQueuedJob(Job& job, const std::string& message);
  void abandonStartedJob(JobId job_id, const std::string& message);
};

}  // namespace runnerd

#endif  // RUNNERD_PROCESS_MONITOR_H

#ifndef RUNNERD_PROCESS_MONITOR_H
#define RUNNERD_PROCESS_MONITOR_H

#include <cstdint>
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

  bool ownsFileDescriptor(int fd) const;

  // 处理 stdout、stderr、startup error pipe 或 signalfd 事件。
  void handleFileDescriptorEvent(int fd, std::uint32_t event_mask);

 private:
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

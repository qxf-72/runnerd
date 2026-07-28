#ifndef RUNNERD_PROCESS_LAUNCHER_H
#define RUNNERD_PROCESS_LAUNCHER_H

#include <sys/types.h>

#include <cstdint>

#include "runnerd/job.h"

namespace runnerd {

enum class ChildStartupStage : std::uint32_t {
  kParentDied = 1,
  kSetParentDeathSignal,
  kSetProcessGroup,
  kDuplicateStdin,
  kDuplicateStdout,
  kDuplicateStderr,
  kExecve,
};

// 只在父子进程之间的 startup error pipe 上传输，
// 不是网络协议的一部分。
struct ChildStartupError {
  ChildStartupStage stage;
  std::int32_t error_number;
};

struct LaunchedProcess {
  pid_t pid;
  pid_t process_group_id;

  // 父进程拥有这三个 fd，且它们已经设置 O_NONBLOCK 和 FD_CLOEXEC。
  int stdout_fd;
  int stderr_fd;
  int startup_error_fd;
};

// 启动失败于 fork、pipe、fcntl 等父进程操作时抛异常。
// execve 失败不会在此函数中抛异常；子进程会将错误写入
// startup_error_fd，然后以 127 退出。
LaunchedProcess launchProcess(const JobSpec& spec);

}  // namespace runnerd

#endif  // RUNNERD_PROCESS_LAUNCHER_H

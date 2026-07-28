#include "runnerd/process_launcher.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <vector>

#include "runnerd/fd_utils.h"

extern char** environ;

namespace runnerd {

namespace {

struct Pipe {
  int read_fd = -1;
  int write_fd = -1;
};

void closeFd(int& fd) noexcept {
  if (fd != -1) {
    static_cast<void>(::close(fd));
    fd = -1;
  }
}

void closePipe(Pipe& pipe) noexcept {
  closeFd(pipe.read_fd);
  closeFd(pipe.write_fd);
}

// 确保 0、1、2 都存在，避免 pipe 意外占用标准 fd，
// 从而使后续 dup2/close 的语义变得复杂。
void ensureStandardDescriptors() {
  for (int standard_fd = STDIN_FILENO; standard_fd <= STDERR_FILENO; ++standard_fd) {
    if (::fcntl(standard_fd, F_GETFD) != -1) {
      continue;
    }

    if (errno != EBADF) {
      const int error_number = errno;
      throw std::system_error(error_number, std::generic_category(),
                              "fcntl(F_GETFD) for standard descriptor");
    }

    const int open_flags = standard_fd == STDIN_FILENO ? O_RDONLY : O_WRONLY;

    const int replacement_fd = ::open("/dev/null", open_flags);

    if (replacement_fd == -1) {
      const int error_number = errno;
      throw std::system_error(error_number, std::generic_category(),
                              "open(/dev/null) for standard descriptor");
    }

    if (replacement_fd != standard_fd) {
      if (::dup2(replacement_fd, standard_fd) == -1) {
        const int saved_errno = errno;
        static_cast<void>(::close(replacement_fd));

        throw std::system_error(saved_errno, std::generic_category(),
                                "dup2(/dev/null) for standard descriptor");
      }

      static_cast<void>(::close(replacement_fd));
    }
  }
}

Pipe createPipe() {
  int fds[2] = {-1, -1};

  if (::pipe2(fds, O_CLOEXEC) == -1) {
    const int error_number = errno;
    throw std::system_error(error_number, std::generic_category(), "pipe2");
  }

  return Pipe{fds[0], fds[1]};
}

[[noreturn]] void reportChildStartupError(int error_fd, ChildStartupStage stage,
                                          int error_number) noexcept {
  const ChildStartupError error{
      stage,
      static_cast<std::int32_t>(error_number),
  };

  const char* data = reinterpret_cast<const char*>(&error);

  std::size_t written = 0;

  while (written < sizeof(error)) {
    const ssize_t result = ::write(error_fd, data + written, sizeof(error) - written);

    if (result > 0) {
      written += static_cast<std::size_t>(result);
      continue;
    }

    if (result == -1 && errno == EINTR) {
      continue;
    }

    break;
  }

  ::_exit(127);
}

void duplicateOrReport(int source_fd, int target_fd, int error_fd,
                       ChildStartupStage stage) noexcept {
  if (::dup2(source_fd, target_fd) == -1) {
    reportChildStartupError(error_fd, stage, errno);
  }
}

void setParentProcessGroup(pid_t child_pid) {
  if (::setpgid(child_pid, child_pid) == 0) {
    return;
  }

  const int error_number = errno;

  // 子进程可能已经在自身分支中完成 setpgid 并 exec，
  // 这种情况下无需视为 launcher 失败。
  if (error_number == EACCES || error_number == ESRCH) {
    return;
  }

  throw std::system_error(error_number, std::generic_category(), "setpgid in parent");
}

void killAndReapChild(pid_t child_pid) noexcept {
  static_cast<void>(::kill(child_pid, SIGKILL));

  int status = 0;

  while (::waitpid(child_pid, &status, 0) == -1) {
    if (errno != EINTR) {
      break;
    }
  }
}

}  // namespace

LaunchedProcess launchProcess(const JobSpec& spec) {
  validateJobSpec(spec);

  // 必须在 fork 前构造，子进程分支中不再分配 vector/string。
  std::vector<char*> argv;
  argv.reserve(spec.argv.size() + 1);

  for (const std::string& argument : spec.argv) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }

  argv.push_back(nullptr);

  ensureStandardDescriptors();

  int dev_null_fd = -1;
  Pipe stdout_pipe;
  Pipe stderr_pipe;
  Pipe startup_error_pipe;

  try {
    dev_null_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);

    if (dev_null_fd == -1) {
      const int error_number = errno;
      throw std::system_error(error_number, std::generic_category(), "open(/dev/null)");
    }

    stdout_pipe = createPipe();
    stderr_pipe = createPipe();
    startup_error_pipe = createPipe();
  } catch (...) {
    closeFd(dev_null_fd);
    closePipe(stdout_pipe);
    closePipe(stderr_pipe);
    closePipe(startup_error_pipe);
    throw;
  }

  const pid_t original_parent_pid = ::getpid();

  const pid_t child_pid = ::fork();

  if (child_pid == -1) {
    const int saved_errno = errno;

    closeFd(dev_null_fd);
    closePipe(stdout_pipe);
    closePipe(stderr_pipe);
    closePipe(startup_error_pipe);

    throw std::system_error(saved_errno, std::generic_category(), "fork");
  }

  if (child_pid == 0) {
    // 子进程不读取自己的 stdout/stderr/error pipe。
    closeFd(stdout_pipe.read_fd);
    closeFd(stderr_pipe.read_fd);
    closeFd(startup_error_pipe.read_fd);

    // 尽早设置；如果 runnerd 意外退出，直接子进程会收到 SIGKILL。
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) {
      reportChildStartupError(startup_error_pipe.write_fd, ChildStartupStage::kSetParentDeathSignal,
                              errno);
    }

    // 处理父进程在 prctl 前已经退出的竞态。
    if (::getppid() != original_parent_pid) {
      reportChildStartupError(startup_error_pipe.write_fd, ChildStartupStage::kParentDied, ECHILD);
    }

    // PGID 等于 PID；后续取消和超时会对 -pgid 发信号。
    if (::setpgid(0, 0) == -1) {
      reportChildStartupError(startup_error_pipe.write_fd, ChildStartupStage::kSetProcessGroup,
                              errno);
    }

    duplicateOrReport(dev_null_fd, STDIN_FILENO, startup_error_pipe.write_fd,
                      ChildStartupStage::kDuplicateStdin);

    duplicateOrReport(stdout_pipe.write_fd, STDOUT_FILENO, startup_error_pipe.write_fd,
                      ChildStartupStage::kDuplicateStdout);

    duplicateOrReport(stderr_pipe.write_fd, STDERR_FILENO, startup_error_pipe.write_fd,
                      ChildStartupStage::kDuplicateStderr);

    closeFd(dev_null_fd);
    closeFd(stdout_pipe.write_fd);
    closeFd(stderr_pipe.write_fd);

    // startup_error_pipe.write_fd 保留到 execve：
    // 成功时 CLOEXEC 自动关闭；失败时写入 errno。
    ::execve(argv[0], argv.data(), environ);

    reportChildStartupError(startup_error_pipe.write_fd, ChildStartupStage::kExecve, errno);
  }

  // 父进程只保留读端。
  closeFd(dev_null_fd);
  closeFd(stdout_pipe.write_fd);
  closeFd(stderr_pipe.write_fd);
  closeFd(startup_error_pipe.write_fd);

  try {
    setParentProcessGroup(child_pid);

    setNonBlocking(stdout_pipe.read_fd);
    setNonBlocking(stderr_pipe.read_fd);
    setNonBlocking(startup_error_pipe.read_fd);

    return LaunchedProcess{
        child_pid, child_pid, stdout_pipe.read_fd, stderr_pipe.read_fd, startup_error_pipe.read_fd,
    };
  } catch (...) {
    closeFd(stdout_pipe.read_fd);
    closeFd(stderr_pipe.read_fd);
    closeFd(startup_error_pipe.read_fd);

    killAndReapChild(child_pid);
    throw;
  }
}

}  // namespace runnerd

#include "runnerd/process_launcher.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <system_error>

namespace {

struct ProcessResult {
  int status = 0;
  std::string standard_output;
  std::string standard_error;
  std::string startup_error;
};

void closeFd(int& fd) {
  if (fd != -1) {
    static_cast<void>(::close(fd));
    fd = -1;
  }
}

int waitForChild(pid_t pid) {
  int status = 0;

  while (::waitpid(pid, &status, 0) == -1) {
    if (errno != EINTR) {
      const int error_number = errno;
      throw std::system_error(error_number, std::generic_category(), "waitpid");
    }
  }

  return status;
}

// 尽可能读完当前可用的数据。EAGAIN 表示暂时没有数据，EOF 才表示写端已全部关闭。
void drainAvailableData(int& fd, std::string& output) {
  char buffer[4096];

  for (;;) {
    const ssize_t read_size = ::read(fd, buffer, sizeof(buffer));

    if (read_size > 0) {
      output.append(buffer, static_cast<std::size_t>(read_size));
      continue;
    }

    if (read_size == 0) {
      closeFd(fd);
      return;
    }

    if (errno == EINTR) {
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }

    const int error_number = errno;
    closeFd(fd);

    throw std::system_error(error_number, std::generic_category(), "read launcher pipe");
  }
}

// 同时排空 stdout、stderr 和启动错误管道，避免任意管道写满后让子进程阻塞。
ProcessResult collectProcessResult(runnerd::LaunchedProcess process) {
  ProcessResult result;

  std::array<pollfd, 3> descriptors{{
      {process.stdout_fd, POLLIN, 0},
      {process.stderr_fd, POLLIN, 0},
      {process.startup_error_fd, POLLIN, 0},
  }};

  std::array<std::string*, 3> outputs{{
      &result.standard_output,
      &result.standard_error,
      &result.startup_error,
  }};

  std::size_t open_count = descriptors.size();

  while (open_count > 0) {
    int poll_result = -1;

    do {
      poll_result = ::poll(descriptors.data(), descriptors.size(), -1);
    } while (poll_result == -1 && errno == EINTR);

    if (poll_result == -1) {
      const int error_number = errno;
      throw std::system_error(error_number, std::generic_category(), "poll launcher pipes");
    }

    for (std::size_t index = 0; index < descriptors.size(); ++index) {
      pollfd& descriptor = descriptors[index];

      if (descriptor.fd == -1 || descriptor.revents == 0) {
        continue;
      }

      drainAvailableData(descriptor.fd, *outputs[index]);
      descriptor.revents = 0;

      if (descriptor.fd == -1) {
        --open_count;
      }
    }
  }

  result.status = waitForChild(process.pid);
  return result;
}

void expectDescriptorFlags(int fd) {
  const int status_flags = ::fcntl(fd, F_GETFL, 0);
  const int status_flags_error = status_flags == -1 ? errno : 0;

  ASSERT_NE(status_flags, -1) << std::strerror(status_flags_error);
  EXPECT_NE(status_flags & O_NONBLOCK, 0);

  const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
  const int descriptor_flags_error = descriptor_flags == -1 ? errno : 0;

  ASSERT_NE(descriptor_flags, -1) << std::strerror(descriptor_flags_error);
  EXPECT_NE(descriptor_flags & FD_CLOEXEC, 0);
}

TEST(ProcessLauncherTest, CapturesStdoutAndStderr) {
  runnerd::JobSpec spec;
  spec.argv = {
      "/bin/sh",
      "-c",
      "printf stdout; printf stderr >&2",
  };

  runnerd::LaunchedProcess process = runnerd::launchProcess(spec);

  expectDescriptorFlags(process.stdout_fd);
  expectDescriptorFlags(process.stderr_fd);
  expectDescriptorFlags(process.startup_error_fd);

  const ProcessResult result = collectProcessResult(process);

  ASSERT_TRUE(WIFEXITED(result.status));
  EXPECT_EQ(WEXITSTATUS(result.status), 0);
  EXPECT_EQ(result.standard_output, "stdout");
  EXPECT_EQ(result.standard_error, "stderr");

  // execve 成功，CLOEXEC 关闭写端，父进程只读到 EOF。
  EXPECT_TRUE(result.startup_error.empty());
}

TEST(ProcessLauncherTest, DrainsOutputLargerThanPipeCapacity) {
  runnerd::JobSpec spec;
  spec.argv = {
      "/bin/sh",
      "-c",
      "i=0; while [ \"$i\" -lt 20000 ]; do "
      "printf 0123456789; i=$((i + 1)); done",
  };

  runnerd::LaunchedProcess process = runnerd::launchProcess(spec);
  const ProcessResult result = collectProcessResult(process);

  ASSERT_TRUE(WIFEXITED(result.status));
  EXPECT_EQ(WEXITSTATUS(result.status), 0);
  EXPECT_EQ(result.standard_output.size(), 200000U);
  EXPECT_TRUE(result.standard_error.empty());
  EXPECT_TRUE(result.startup_error.empty());
}

TEST(ProcessLauncherTest, ReportsExecveFailureSeparatelyFromExitCode) {
  runnerd::JobSpec spec;
  spec.argv = {
      "/definitely/not/a/real/executable",
  };

  runnerd::LaunchedProcess process = runnerd::launchProcess(spec);
  const ProcessResult result = collectProcessResult(process);

  ASSERT_TRUE(WIFEXITED(result.status));
  EXPECT_EQ(WEXITSTATUS(result.status), 127);
  EXPECT_TRUE(result.standard_output.empty());
  EXPECT_TRUE(result.standard_error.empty());

  ASSERT_EQ(result.startup_error.size(), sizeof(runnerd::ChildStartupError));

  runnerd::ChildStartupError startup_error{};
  std::memcpy(&startup_error, result.startup_error.data(), sizeof(startup_error));

  EXPECT_EQ(startup_error.stage, runnerd::ChildStartupStage::kExecve);

  EXPECT_EQ(startup_error.error_number, ENOENT);
}

TEST(ProcessLauncherTest, CreatesIndependentProcessGroup) {
  runnerd::JobSpec spec;
  spec.argv = {
      "/bin/sleep",
      "5",
  };

  runnerd::LaunchedProcess process = runnerd::launchProcess(spec);

  const pid_t actual_group_id = ::getpgid(process.pid);

  const int group_kill_result = ::kill(-process.process_group_id, SIGTERM);

  const int group_kill_errno = group_kill_result == -1 ? errno : 0;

  const ProcessResult result = collectProcessResult(process);

  EXPECT_EQ(actual_group_id, process.process_group_id);
  EXPECT_EQ(group_kill_result, 0) << std::strerror(group_kill_errno);

  ASSERT_TRUE(WIFSIGNALED(result.status));
  EXPECT_EQ(WTERMSIG(result.status), SIGTERM);
}

}  // namespace

#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef RUNNERD_BINARY_PATH
#error "RUNNERD_BINARY_PATH is not defined"
#endif

#ifndef RUNNERCTL_BINARY_PATH
#error "RUNNERCTL_BINARY_PATH is not defined"
#endif

namespace {

struct ChildResult {
  int exit_code = -1;
  std::string standard_output;
  std::string standard_error;
};

std::string readAll(int fd) {
  std::string output;
  char buffer[4096];

  for (;;) {
    const ssize_t size = ::read(fd, buffer, sizeof(buffer));

    if (size > 0) {
      output.append(buffer, static_cast<std::size_t>(size));
      continue;
    }

    if (size == 0) {
      return output;
    }

    if (errno == EINTR) {
      continue;
    }

    throw std::system_error(errno, std::generic_category(), "read child output");
  }
}

void closeIfOpen(int& fd) {
  if (fd != -1) {
    ::close(fd);
    fd = -1;
  }
}

void waitWithoutThrow(pid_t pid) {
  if (pid <= 0) {
    return;
  }

  int status = 0;

  while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {
  }
}

class ChildProcess {
 public:
  ChildProcess() = default;

  ChildProcess(pid_t pid, int stdout_fd, int stderr_fd)
      : pid_(pid), stdout_fd_(stdout_fd), stderr_fd_(stderr_fd) {}

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  ChildProcess(ChildProcess&& other) noexcept
      : pid_(std::exchange(other.pid_, -1)),
        stdout_fd_(std::exchange(other.stdout_fd_, -1)),
        stderr_fd_(std::exchange(other.stderr_fd_, -1)) {}

  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      cleanup();
      pid_ = std::exchange(other.pid_, -1);
      stdout_fd_ = std::exchange(other.stdout_fd_, -1);
      stderr_fd_ = std::exchange(other.stderr_fd_, -1);
    }

    return *this;
  }

  ~ChildProcess() {
    cleanup();
  }

  static ChildProcess start(const std::vector<std::string>& arguments) {
    if (arguments.empty()) {
      throw std::invalid_argument("child command must not be empty");
    }

    std::vector<char*> child_argv;
    child_argv.reserve(arguments.size() + 1);

    for (const std::string& argument : arguments) {
      child_argv.push_back(const_cast<char*>(argument.c_str()));
    }

    child_argv.push_back(nullptr);

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

    if (::pipe2(stdout_pipe, O_CLOEXEC) == -1) {
      throw std::system_error(errno, std::generic_category(), "create stdout pipe");
    }

    if (::pipe2(stderr_pipe, O_CLOEXEC) == -1) {
      const int saved_errno = errno;
      ::close(stdout_pipe[0]);
      ::close(stdout_pipe[1]);
      throw std::system_error(saved_errno, std::generic_category(), "create stderr pipe");
    }

    const pid_t pid = ::fork();

    if (pid == -1) {
      const int saved_errno = errno;
      ::close(stdout_pipe[0]);
      ::close(stdout_pipe[1]);
      ::close(stderr_pipe[0]);
      ::close(stderr_pipe[1]);
      throw std::system_error(saved_errno, std::generic_category(), "fork child process");
    }

    if (pid == 0) {
      ::close(stdout_pipe[0]);
      ::close(stderr_pipe[0]);

      if (::dup2(stdout_pipe[1], STDOUT_FILENO) == -1 ||
          ::dup2(stderr_pipe[1], STDERR_FILENO) == -1) {
        ::_exit(126);
      }

      ::close(stdout_pipe[1]);
      ::close(stderr_pipe[1]);

      ::execv(child_argv[0], child_argv.data());
      ::_exit(127);
    }

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    return ChildProcess(pid, stdout_pipe[0], stderr_pipe[0]);
  }

  ChildResult finish() {
    if (pid_ <= 0) {
      throw std::logic_error("child process is not running");
    }

    ChildResult result;
    result.standard_output = readAll(stdout_fd_);
    closeIfOpen(stdout_fd_);
    result.standard_error = readAll(stderr_fd_);
    closeIfOpen(stderr_fd_);

    int status = 0;
    pid_t wait_result = -1;

    do {
      wait_result = ::waitpid(pid_, &status, 0);
    } while (wait_result == -1 && errno == EINTR);

    if (wait_result == -1) {
      throw std::system_error(errno, std::generic_category(), "wait for child process");
    }

    pid_ = -1;

    if (WIFEXITED(status)) {
      result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      result.exit_code = 128 + WTERMSIG(status);
    }

    return result;
  }

 private:
  void cleanup() noexcept {
    closeIfOpen(stdout_fd_);
    closeIfOpen(stderr_fd_);

    if (pid_ > 0) {
      static_cast<void>(::kill(pid_, SIGKILL));
      waitWithoutThrow(pid_);
      pid_ = -1;
    }
  }

  pid_t pid_ = -1;
  int stdout_fd_ = -1;
  int stderr_fd_ = -1;
};

class RunnerdIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char directory_template[] = "/tmp/runnerd-integration.XXXXXX";
    const char* directory = ::mkdtemp(directory_template);

    ASSERT_NE(directory, nullptr) << std::strerror(errno);

    test_directory_ = directory;
    socket_path_ = test_directory_ + "/runnerd.sock";
    server_log_path_ = test_directory_ + "/runnerd.log";

    server_pid_ = ::fork();
    ASSERT_NE(server_pid_, -1) << std::strerror(errno);

    if (server_pid_ == 0) {
      const int log_fd =
          ::open(server_log_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

      if (log_fd == -1 || ::dup2(log_fd, STDOUT_FILENO) == -1 ||
          ::dup2(log_fd, STDERR_FILENO) == -1) {
        ::_exit(126);
      }

      ::close(log_fd);

      const std::string max_running = maxRunningArgument();

      ::execl(RUNNERD_BINARY_PATH, RUNNERD_BINARY_PATH, "--socket", socket_path_.c_str(),
              "--max-running", max_running.c_str(), static_cast<char*>(nullptr));
      ::_exit(127);
    }

    ASSERT_TRUE(waitForSocket()) << "runnerd did not create its socket\n" << readServerLog();
  }

  void TearDown() override {
    if (server_pid_ > 0) {
      static_cast<void>(::kill(server_pid_, SIGTERM));
      waitWithoutThrow(server_pid_);
      server_pid_ = -1;
    }

    if (!socket_path_.empty()) {
      static_cast<void>(::unlink(socket_path_.c_str()));
    }

    if (!server_log_path_.empty()) {
      static_cast<void>(::unlink(server_log_path_.c_str()));
    }

    if (!test_directory_.empty()) {
      static_cast<void>(::rmdir(test_directory_.c_str()));
    }
  }

  ChildProcess startClient(std::vector<std::string> arguments) const {
    std::vector<std::string> command{
        RUNNERCTL_BINARY_PATH,
        "--socket",
        socket_path_,
    };

    command.insert(command.end(), arguments.begin(), arguments.end());
    return ChildProcess::start(command);
  }

  ChildResult runClient(std::vector<std::string> arguments) const {
    ChildProcess process = startClient(std::move(arguments));
    return process.finish();
  }

  ChildResult waitForJobState(const std::string& job_id,
                              const std::string& expected_state) const {
    ChildResult status;
    const std::string expected_text = "state=" + expected_state;

    // 不使用固定 sleep 猜测任务何时完成，而是轮询对外可见的 STATUS。
    // 200 * 20 ms 给正常 CI 环境最多约 4 秒的调度时间，
    // 同时仍低于该集成测试的 10 秒 CTest 超时。
    for (int attempt = 0; attempt < 200; ++attempt) {
      status = runClient({"status", job_id});

      if (status.exit_code != 0 ||
          status.standard_output.find(expected_text) != std::string::npos) {
        return status;
      }

      ::usleep(20'000);
    }

    return status;
  }

  bool serverIsRunning() const {
    return server_pid_ > 0 && ::kill(server_pid_, 0) == 0;
  }

  std::string readServerLog() const {
    std::ifstream input(server_log_path_);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }

  virtual std::string maxRunningArgument() const {
    return "1";
  }

 private:
  bool waitForSocket() {
    for (int attempt = 0; attempt < 100; ++attempt) {
      struct stat file_stat {};

      if (::lstat(socket_path_.c_str(), &file_stat) == 0 && S_ISSOCK(file_stat.st_mode)) {
        return true;
      }

      int status = 0;
      const pid_t result = ::waitpid(server_pid_, &status, WNOHANG);

      if (result == server_pid_) {
        server_pid_ = -1;
        return false;
      }

      ::usleep(20'000);
    }

    return false;
  }

  std::string test_directory_;
  std::string socket_path_;
  std::string server_log_path_;
  pid_t server_pid_ = -1;
};

TEST_F(RunnerdIntegrationTest, HandlesTwentyConcurrentPingClients) {
  constexpr int kClientCount = 20;
  std::vector<ChildProcess> clients;
  clients.reserve(kClientCount);

  for (int index = 0; index < kClientCount; ++index) {
    clients.push_back(startClient({"ping"}));
  }

  for (int index = 0; index < kClientCount; ++index) {
    const ChildResult result = clients[static_cast<std::size_t>(index)].finish();

    EXPECT_EQ(result.exit_code, 0) << "client " << index + 1 << ": " << result.standard_error;
    EXPECT_EQ(result.standard_output, "PONG\n") << "client " << index + 1;
  }

  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST_F(RunnerdIntegrationTest, SubmitsJobsAndKeepsServingRequests) {
  const ChildResult first_submit = runClient({"submit", "--", "/bin/echo", "hello"});
  ASSERT_EQ(first_submit.exit_code, 0) << first_submit.standard_error;
  EXPECT_EQ(first_submit.standard_output, "1\n");

  const ChildResult second_submit =
      runClient({"submit", "--timeout", "1000", "--", "/bin/sleep", "1"});
  ASSERT_EQ(second_submit.exit_code, 0) << second_submit.standard_error;
  EXPECT_EQ(second_submit.standard_output, "2\n");

  const ChildResult invalid_submit = runClient({"submit", "--", "echo", "hello"});
  EXPECT_NE(invalid_submit.exit_code, 0);
  EXPECT_NE(invalid_submit.standard_error.find("absolute path"), std::string::npos);

  const ChildResult ping = runClient({"ping"});
  ASSERT_EQ(ping.exit_code, 0) << ping.standard_error;
  EXPECT_EQ(ping.standard_output, "PONG\n");
  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST_F(RunnerdIntegrationTest, ListsNoJobsBeforeSubmission) {
  const ChildResult result = runClient({"list"});

  ASSERT_EQ(result.exit_code, 0) << result.standard_error;
  EXPECT_EQ(result.standard_output, "No jobs\n");
}

TEST_F(RunnerdIntegrationTest, RejectsInvalidStatusAndListArguments) {
  const ChildResult zero_id = runClient({"status", "0"});
  EXPECT_NE(zero_id.exit_code, 0);
  EXPECT_NE(zero_id.standard_error.find("greater than zero"), std::string::npos);

  const ChildResult invalid_id = runClient({"status", "abc"});
  EXPECT_NE(invalid_id.exit_code, 0);
  EXPECT_NE(invalid_id.standard_error.find("positive integer"), std::string::npos);

  const ChildResult extra_status_argument = runClient({"status", "1", "extra"});
  EXPECT_NE(extra_status_argument.exit_code, 0);
  EXPECT_NE(extra_status_argument.standard_error.find("exactly one"), std::string::npos);

  const ChildResult extra_list_argument = runClient({"list", "extra"});
  EXPECT_NE(extra_list_argument.exit_code, 0);
  EXPECT_NE(extra_list_argument.standard_error.find("does not accept"), std::string::npos);
}

TEST_F(RunnerdIntegrationTest, RejectsInvalidCancelArguments) {
  const ChildResult missing_id = runClient({"cancel"});
  EXPECT_NE(missing_id.exit_code, 0);
  EXPECT_NE(missing_id.standard_error.find("requires a job id"), std::string::npos);

  const ChildResult zero_id = runClient({"cancel", "0"});
  EXPECT_NE(zero_id.exit_code, 0);
  EXPECT_NE(zero_id.standard_error.find("greater than zero"), std::string::npos);

  const ChildResult invalid_id = runClient({"cancel", "abc"});
  EXPECT_NE(invalid_id.exit_code, 0);
  EXPECT_NE(invalid_id.standard_error.find("positive integer"), std::string::npos);

  const ChildResult extra_argument = runClient({"cancel", "1", "extra"});
  EXPECT_NE(extra_argument.exit_code, 0);
  EXPECT_NE(extra_argument.standard_error.find("exactly one"), std::string::npos);

  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST_F(RunnerdIntegrationTest, ReportsStatusAndListsJobsInIdOrder) {
  const ChildResult first_submit = runClient({"submit", "--", "/bin/echo", "hello"});

  ASSERT_EQ(first_submit.exit_code, 0) << first_submit.standard_error;
  EXPECT_EQ(first_submit.standard_output, "1\n");

  // echo 可能在第一次 STATUS 到达时仍为 RUNNING，因此轮询等待终态。
  ChildResult status;
  bool completed = false;

  for (int attempt = 0; attempt < 100; ++attempt) {
    status = runClient({"status", "1"});

    ASSERT_EQ(status.exit_code, 0) << status.standard_error;

    if (status.standard_output.find("state=SUCCEEDED") != std::string::npos) {
      completed = true;
      break;
    }

    ::usleep(20'000);
  }

  ASSERT_TRUE(completed) << "last status: " << status.standard_output << "\nserver log:\n"
                         << readServerLog();

  EXPECT_NE(status.standard_output.find("id=1"), std::string::npos);
  EXPECT_NE(status.standard_output.find("exit_code=0"), std::string::npos);
  EXPECT_NE(status.standard_output.find("stdout_bytes=6"), std::string::npos);

  const ChildResult second_submit = runClient({"submit", "--", "/bin/sleep", "1"});

  ASSERT_EQ(second_submit.exit_code, 0) << second_submit.standard_error;
  EXPECT_EQ(second_submit.standard_output, "2\n");

  const ChildResult list = runClient({"list"});

  ASSERT_EQ(list.exit_code, 0) << list.standard_error;

  const std::size_t first_position = list.standard_output.find("id=1 state=");
  const std::size_t second_position = list.standard_output.find("id=2 state=");

  ASSERT_NE(first_position, std::string::npos);
  ASSERT_NE(second_position, std::string::npos);

  // LIST 不应依赖 unordered_map 不稳定的遍历顺序。
  EXPECT_LT(first_position, second_position);

  const ChildResult missing = runClient({"status", "999"});

  EXPECT_NE(missing.exit_code, 0);
  EXPECT_NE(missing.standard_error.find("job not found"), std::string::npos);
  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST_F(RunnerdIntegrationTest, ReportsNonzeroExitAndCapturedOutputForFailedJob) {
  const ChildResult submit =
      runClient({"submit", "--", "/bin/sh", "-c", "printf out; printf err >&2; exit 7"});

  ASSERT_EQ(submit.exit_code, 0) << submit.standard_error;
  EXPECT_EQ(submit.standard_output, "1\n");

  // SUBMIT 响应可能早于子进程退出，轮询到 FAILED 后再检查最终结果。
  ChildResult status;
  bool failed = false;

  for (int attempt = 0; attempt < 100; ++attempt) {
    status = runClient({"status", "1"});

    ASSERT_EQ(status.exit_code, 0) << status.standard_error;

    if (status.standard_output.find("state=FAILED") != std::string::npos) {
      failed = true;
      break;
    }

    ::usleep(20'000);
  }

  ASSERT_TRUE(failed) << "last status: " << status.standard_output << "\nserver log:\n"
                      << readServerLog();

  EXPECT_NE(status.standard_output.find("id=1"), std::string::npos);
  EXPECT_NE(status.standard_output.find("exit_code=7"), std::string::npos);
  EXPECT_NE(status.standard_output.find("stdout_bytes=3"), std::string::npos);
  EXPECT_NE(status.standard_output.find("stderr_bytes=3"), std::string::npos);
  EXPECT_NE(status.standard_output.find("error=process exited with code 7"), std::string::npos);
  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST_F(RunnerdIntegrationTest, QueuesSecondLongRunningJobWhenMaximumIsOne) {
  // sleep 时间足够长，确保测试查询状态时两个任务都尚未自然结束。
  const ChildResult first_submit = runClient({"submit", "--", "/bin/sleep", "30"});

  ASSERT_EQ(first_submit.exit_code, 0) << first_submit.standard_error;
  EXPECT_EQ(first_submit.standard_output, "1\n");

  const ChildResult second_submit = runClient({"submit", "--", "/bin/sleep", "30"});

  ASSERT_EQ(second_submit.exit_code, 0) << second_submit.standard_error;
  EXPECT_EQ(second_submit.standard_output, "2\n");

  // 第一个任务取得唯一的运行槽位，因此已经被启动。
  const ChildResult first_status = runClient({"status", "1"});
  ASSERT_EQ(first_status.exit_code, 0) << first_status.standard_error;
  EXPECT_NE(first_status.standard_output.find("state=RUNNING"), std::string::npos)
      << first_status.standard_output;

  // 第二个任务虽然已经进入 JobTable，但没有空闲槽位，
  // 所以必须保持 QUEUED，且不能被 ProcessMonitor 启动。
  const ChildResult second_status = runClient({"status", "2"});
  ASSERT_EQ(second_status.exit_code, 0) << second_status.standard_error;
  EXPECT_NE(second_status.standard_output.find("state=QUEUED"), std::string::npos)
      << second_status.standard_output;

  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST_F(RunnerdIntegrationTest, CancelsQueuedJobAndReportsCancelledState) {
  const ChildResult first_submit = runClient({"submit", "--", "/bin/sleep", "30"});
  const ChildResult second_submit = runClient({"submit", "--", "/bin/sleep", "30"});

  ASSERT_EQ(first_submit.exit_code, 0) << first_submit.standard_error;
  ASSERT_EQ(second_submit.exit_code, 0) << second_submit.standard_error;
  ASSERT_EQ(first_submit.standard_output, "1\n");
  ASSERT_EQ(second_submit.standard_output, "2\n");

  const ChildResult queued_status = runClient({"status", "2"});
  ASSERT_EQ(queued_status.exit_code, 0) << queued_status.standard_error;
  ASSERT_NE(queued_status.standard_output.find("state=QUEUED"), std::string::npos)
      << queued_status.standard_output;

  const ChildResult cancellation = runClient({"cancel", "2"});
  ASSERT_EQ(cancellation.exit_code, 0) << cancellation.standard_error;
  EXPECT_EQ(cancellation.standard_output, "Cancelled job 2\n");

  // CANCEL 必须同时更新 JobTable 和 Scheduler：对外状态变为终态，
  // 后续也不能再被调度器取出启动。
  const ChildResult cancelled_status = runClient({"status", "2"});
  ASSERT_EQ(cancelled_status.exit_code, 0) << cancelled_status.standard_error;
  EXPECT_NE(cancelled_status.standard_output.find("state=CANCELLED"), std::string::npos)
      << cancelled_status.standard_output;

  const ChildResult list = runClient({"list"});
  ASSERT_EQ(list.exit_code, 0) << list.standard_error;
  EXPECT_NE(list.standard_output.find("id=2 state=CANCELLED"), std::string::npos)
      << list.standard_output;

  const ChildResult first_status = runClient({"status", "1"});
  ASSERT_EQ(first_status.exit_code, 0) << first_status.standard_error;
  EXPECT_NE(first_status.standard_output.find("state=RUNNING"), std::string::npos)
      << first_status.standard_output;
  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST_F(RunnerdIntegrationTest, CancellingMiddleQueuedJobPreservesFifoProgress) {
  // Job 1 暂时占用唯一槽位，确保 Job 2 和 Job 3 都先进入等待队列。
  const ChildResult first_submit = runClient({"submit", "--", "/bin/sleep", "1"});
  const ChildResult second_submit = runClient({"submit", "--", "/bin/sleep", "30"});
  const ChildResult third_submit = runClient({"submit", "--", "/bin/sleep", "30"});

  ASSERT_EQ(first_submit.exit_code, 0) << first_submit.standard_error;
  ASSERT_EQ(second_submit.exit_code, 0) << second_submit.standard_error;
  ASSERT_EQ(third_submit.exit_code, 0) << third_submit.standard_error;

  const ChildResult second_status_before = runClient({"status", "2"});
  const ChildResult third_status_before = runClient({"status", "3"});
  ASSERT_EQ(second_status_before.exit_code, 0) << second_status_before.standard_error;
  ASSERT_EQ(third_status_before.exit_code, 0) << third_status_before.standard_error;
  ASSERT_NE(second_status_before.standard_output.find("state=QUEUED"), std::string::npos)
      << second_status_before.standard_output;
  ASSERT_NE(third_status_before.standard_output.find("state=QUEUED"), std::string::npos)
      << third_status_before.standard_output;

  const ChildResult cancellation = runClient({"cancel", "2"});
  ASSERT_EQ(cancellation.exit_code, 0) << cancellation.standard_error;

  // 从队列中间删除 Job 2 后，Job 1 结束释放的槽位必须交给 Job 3。
  const ChildResult third_status = waitForJobState("3", "RUNNING");
  ASSERT_EQ(third_status.exit_code, 0)
      << third_status.standard_error << "\nserver log:\n" << readServerLog();
  EXPECT_NE(third_status.standard_output.find("state=RUNNING"), std::string::npos)
      << third_status.standard_output << "\nserver log:\n"
      << readServerLog();

  const ChildResult second_status_after = runClient({"status", "2"});
  ASSERT_EQ(second_status_after.exit_code, 0) << second_status_after.standard_error;
  EXPECT_NE(second_status_after.standard_output.find("state=CANCELLED"), std::string::npos)
      << second_status_after.standard_output;
  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST_F(RunnerdIntegrationTest, RejectsMissingRunningTerminalAndRepeatedCancellation) {
  const ChildResult missing = runClient({"cancel", "999"});
  EXPECT_NE(missing.exit_code, 0);
  EXPECT_NE(missing.standard_error.find("job not found"), std::string::npos);

  const ChildResult completed_submit = runClient({"submit", "--", "/bin/true"});
  ASSERT_EQ(completed_submit.exit_code, 0) << completed_submit.standard_error;

  const ChildResult completed_status = waitForJobState("1", "SUCCEEDED");
  ASSERT_EQ(completed_status.exit_code, 0)
      << completed_status.standard_error << "\nserver log:\n" << readServerLog();

  const ChildResult terminal = runClient({"cancel", "1"});
  EXPECT_NE(terminal.exit_code, 0);
  EXPECT_NE(terminal.standard_error.find("already terminal"), std::string::npos);

  const ChildResult running_submit = runClient({"submit", "--", "/bin/sleep", "30"});
  ASSERT_EQ(running_submit.exit_code, 0) << running_submit.standard_error;

  const ChildResult running_status = waitForJobState("2", "RUNNING");
  ASSERT_EQ(running_status.exit_code, 0) << running_status.standard_error;

  const ChildResult running = runClient({"cancel", "2"});
  EXPECT_NE(running.exit_code, 0);
  EXPECT_NE(running.standard_error.find("running job cancellation is not available yet"),
            std::string::npos);

  const ChildResult queued_submit = runClient({"submit", "--", "/bin/sleep", "30"});
  ASSERT_EQ(queued_submit.exit_code, 0) << queued_submit.standard_error;

  const ChildResult queued_status = runClient({"status", "3"});
  ASSERT_EQ(queued_status.exit_code, 0) << queued_status.standard_error;
  ASSERT_NE(queued_status.standard_output.find("state=QUEUED"), std::string::npos)
      << queued_status.standard_output;

  const ChildResult first_cancel = runClient({"cancel", "3"});
  ASSERT_EQ(first_cancel.exit_code, 0) << first_cancel.standard_error;

  const ChildResult repeated_cancel = runClient({"cancel", "3"});
  EXPECT_NE(repeated_cancel.exit_code, 0);
  EXPECT_NE(repeated_cancel.standard_error.find("already terminal"), std::string::npos);
  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

// 这个派生 Fixture 与基础 Fixture 唯一的不同：
// 启动 daemon 时传入 --max-running 2。
class RunnerdMaxTwoIntegrationTest : public RunnerdIntegrationTest {
 protected:
  std::string maxRunningArgument() const override {
    return "2";
  }
};

TEST_F(RunnerdMaxTwoIntegrationTest, StartsFirstTwoJobsAndQueuesThirdWhenMaximumIsTwo) {
  const ChildResult first_submit = runClient({"submit", "--", "/bin/sleep", "30"});
  const ChildResult second_submit = runClient({"submit", "--", "/bin/sleep", "30"});
  const ChildResult third_submit = runClient({"submit", "--", "/bin/sleep", "30"});

  ASSERT_EQ(first_submit.exit_code, 0) << first_submit.standard_error;
  ASSERT_EQ(second_submit.exit_code, 0) << second_submit.standard_error;
  ASSERT_EQ(third_submit.exit_code, 0) << third_submit.standard_error;

  EXPECT_EQ(first_submit.standard_output, "1\n");
  EXPECT_EQ(second_submit.standard_output, "2\n");
  EXPECT_EQ(third_submit.standard_output, "3\n");

  const ChildResult first_status = runClient({"status", "1"});
  const ChildResult second_status = runClient({"status", "2"});
  const ChildResult third_status = runClient({"status", "3"});

  ASSERT_EQ(first_status.exit_code, 0) << first_status.standard_error;
  ASSERT_EQ(second_status.exit_code, 0) << second_status.standard_error;
  ASSERT_EQ(third_status.exit_code, 0) << third_status.standard_error;

  EXPECT_NE(first_status.standard_output.find("state=RUNNING"), std::string::npos)
      << first_status.standard_output;

  EXPECT_NE(second_status.standard_output.find("state=RUNNING"), std::string::npos)
      << second_status.standard_output;

  EXPECT_NE(third_status.standard_output.find("state=QUEUED"), std::string::npos)
      << third_status.standard_output;

  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST_F(RunnerdIntegrationTest, StartsNextQueuedJobAfterSuccessfulCompletion) {
  const ChildResult first_submit = runClient({"submit", "--", "/bin/sleep", "0.2"});
  const ChildResult second_submit = runClient({"submit", "--", "/bin/sleep", "30"});

  ASSERT_EQ(first_submit.exit_code, 0) << first_submit.standard_error;
  ASSERT_EQ(second_submit.exit_code, 0) << second_submit.standard_error;
  ASSERT_EQ(first_submit.standard_output, "1\n");
  ASSERT_EQ(second_submit.standard_output, "2\n");

  const ChildResult second_status = waitForJobState("2", "RUNNING");

  ASSERT_EQ(second_status.exit_code, 0)
      << second_status.standard_error << "\nserver log:\n" << readServerLog();
  EXPECT_NE(second_status.standard_output.find("state=RUNNING"), std::string::npos)
      << second_status.standard_output << "\nserver log:\n"
      << readServerLog();

  const ChildResult first_status = runClient({"status", "1"});

  ASSERT_EQ(first_status.exit_code, 0) << first_status.standard_error;
  EXPECT_NE(first_status.standard_output.find("state=SUCCEEDED"), std::string::npos)
      << first_status.standard_output;
  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST_F(RunnerdIntegrationTest, StartsNextQueuedJobAfterNonzeroExit) {
  const ChildResult first_submit =
      runClient({"submit", "--", "/bin/sh", "-c", "sleep 0.2; exit 7"});
  const ChildResult second_submit = runClient({"submit", "--", "/bin/sleep", "30"});

  ASSERT_EQ(first_submit.exit_code, 0) << first_submit.standard_error;
  ASSERT_EQ(second_submit.exit_code, 0) << second_submit.standard_error;

  const ChildResult first_status = waitForJobState("1", "FAILED");
  ASSERT_EQ(first_status.exit_code, 0)
      << first_status.standard_error << "\nserver log:\n" << readServerLog();
  EXPECT_NE(first_status.standard_output.find("state=FAILED"), std::string::npos)
      << first_status.standard_output << "\nserver log:\n"
      << readServerLog();
  EXPECT_NE(first_status.standard_output.find("exit_code=7"), std::string::npos)
      << first_status.standard_output;

  const ChildResult second_status = waitForJobState("2", "RUNNING");
  ASSERT_EQ(second_status.exit_code, 0)
      << second_status.standard_error << "\nserver log:\n" << readServerLog();
  EXPECT_NE(second_status.standard_output.find("state=RUNNING"), std::string::npos)
      << second_status.standard_output << "\nserver log:\n"
      << readServerLog();
  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST_F(RunnerdIntegrationTest, StartsNextJobAfterExecveFailure) {
  const ChildResult first_submit =
      runClient({"submit", "--", "/definitely/not/a/runnerd-test-executable"});
  const ChildResult second_submit = runClient({"submit", "--", "/bin/sleep", "30"});

  ASSERT_EQ(first_submit.exit_code, 0) << first_submit.standard_error;
  ASSERT_EQ(second_submit.exit_code, 0) << second_submit.standard_error;

  const ChildResult first_status = waitForJobState("1", "FAILED");
  ASSERT_EQ(first_status.exit_code, 0)
      << first_status.standard_error << "\nserver log:\n" << readServerLog();
  EXPECT_NE(first_status.standard_output.find("state=FAILED"), std::string::npos)
      << first_status.standard_output << "\nserver log:\n"
      << readServerLog();
  EXPECT_NE(first_status.standard_output.find("execve"), std::string::npos)
      << first_status.standard_output;

  const ChildResult second_status = waitForJobState("2", "RUNNING");
  ASSERT_EQ(second_status.exit_code, 0)
      << second_status.standard_error << "\nserver log:\n" << readServerLog();
  EXPECT_NE(second_status.standard_output.find("state=RUNNING"), std::string::npos)
      << second_status.standard_output << "\nserver log:\n"
      << readServerLog();
  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST_F(RunnerdIntegrationTest, StartsQueuedJobsInFifoOrder) {
  const ChildResult first_submit = runClient({"submit", "--", "/bin/sleep", "0.2"});
  const ChildResult second_submit = runClient({"submit", "--", "/bin/sleep", "30"});
  const ChildResult third_submit = runClient({"submit", "--", "/bin/sleep", "30"});

  ASSERT_EQ(first_submit.exit_code, 0) << first_submit.standard_error;
  ASSERT_EQ(second_submit.exit_code, 0) << second_submit.standard_error;
  ASSERT_EQ(third_submit.exit_code, 0) << third_submit.standard_error;

  // Job 1 结束后只能释放一个槽位。FIFO 队首 Job 2 必须先启动，
  // Job 3 则继续等待，直到 Job 2 也进入终态。
  const ChildResult second_status = waitForJobState("2", "RUNNING");
  ASSERT_EQ(second_status.exit_code, 0)
      << second_status.standard_error << "\nserver log:\n" << readServerLog();
  EXPECT_NE(second_status.standard_output.find("state=RUNNING"), std::string::npos)
      << second_status.standard_output << "\nserver log:\n"
      << readServerLog();

  const ChildResult third_status = runClient({"status", "3"});
  ASSERT_EQ(third_status.exit_code, 0) << third_status.standard_error;
  EXPECT_NE(third_status.standard_output.find("state=QUEUED"), std::string::npos)
      << third_status.standard_output << "\nserver log:\n"
      << readServerLog();
  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST_F(RunnerdIntegrationTest, HandlesRapidExitsWithoutLeakingOrDoubleReleasingSlots) {
  constexpr int kQuickJobCount = 8;

  // 先用一个短 sleep 占住唯一槽位，让后续 /bin/true 都进入队列。
  const ChildResult first_submit = runClient({"submit", "--", "/bin/sleep", "0.2"});
  ASSERT_EQ(first_submit.exit_code, 0) << first_submit.standard_error;

  for (int job_id = 2; job_id <= kQuickJobCount; ++job_id) {
    const ChildResult submit = runClient({"submit", "--", "/bin/true"});

    ASSERT_EQ(submit.exit_code, 0) << "job " << job_id << ": " << submit.standard_error;
    EXPECT_EQ(submit.standard_output, std::to_string(job_id) + "\n");
  }

  // 最后一个快速任务能够完成，说明前面的终态通知都只释放了一次槽位，
  // 且 FIFO 队列没有因快速 SIGCHLD / pipe EOF 事件而卡住。
  const ChildResult last_quick_status =
      waitForJobState(std::to_string(kQuickJobCount), "SUCCEEDED");

  ASSERT_EQ(last_quick_status.exit_code, 0)
      << last_quick_status.standard_error << "\nserver log:\n" << readServerLog();
  EXPECT_NE(last_quick_status.standard_output.find("state=SUCCEEDED"), std::string::npos)
      << last_quick_status.standard_output << "\nserver log:\n"
      << readServerLog();

  // 队列排空后还应能正常占用一个新槽位。
  const int final_job_id = kQuickJobCount + 1;
  const ChildResult final_submit = runClient({"submit", "--", "/bin/sleep", "30"});

  ASSERT_EQ(final_submit.exit_code, 0) << final_submit.standard_error;
  EXPECT_EQ(final_submit.standard_output, std::to_string(final_job_id) + "\n");

  const ChildResult final_status = waitForJobState(std::to_string(final_job_id), "RUNNING");
  ASSERT_EQ(final_status.exit_code, 0)
      << final_status.standard_error << "\nserver log:\n" << readServerLog();
  EXPECT_NE(final_status.standard_output.find("state=RUNNING"), std::string::npos)
      << final_status.standard_output << "\nserver log:\n"
      << readServerLog();
  EXPECT_TRUE(serverIsRunning()) << readServerLog();
}

TEST(RunnerdCommandLineTest, RejectsInvalidMaximumConcurrency) {
  // 这些值都必须在创建 socket、创建 epoll 或启动子进程之前被拒绝。
  const std::vector<std::string> invalid_values{
      "0",
      "-1",
      "999999999999999999999999999999999999",
  };

  for (const std::string& value : invalid_values) {
    ChildProcess daemon = ChildProcess::start({RUNNERD_BINARY_PATH, "--max-running", value});

    const ChildResult result = daemon.finish();

    EXPECT_NE(result.exit_code, 0) << "value: " << value;
    EXPECT_NE(result.standard_error.find("--max-running"), std::string::npos)
        << "value: " << value << "\nstderr:\n"
        << result.standard_error;
  }
}

}  // namespace

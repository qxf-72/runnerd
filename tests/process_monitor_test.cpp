#include "runnerd/process_monitor.h"

#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

class TemporaryPath {
 public:
  TemporaryPath() {
    char path_template[] = "/tmp/runnerd-process-monitor.XXXXXX";
    const int fd = ::mkstemp(path_template);

    if (fd == -1) {
      throw std::system_error(errno, std::generic_category(), "mkstemp");
    }

    path_ = path_template;
    static_cast<void>(::close(fd));

    // 测试任务会在完成初始化后重新创建这个文件，父进程据此避免过早发送 SIGTERM。
    if (::unlink(path_.c_str()) == -1) {
      throw std::system_error(errno, std::generic_category(), "unlink temporary marker");
    }
  }

  TemporaryPath(const TemporaryPath&) = delete;
  TemporaryPath& operator=(const TemporaryPath&) = delete;

  ~TemporaryPath() {
    static_cast<void>(::unlink(path_.c_str()));
  }

  const std::string& get() const noexcept {
    return path_;
  }

 private:
  std::string path_;
};

bool waitForPath(const std::string& path) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (::access(path.c_str(), F_OK) == 0) {
      return true;
    }

    if (errno != ENOENT) {
      throw std::system_error(errno, std::generic_category(), "access temporary marker");
    }

    ::usleep(10'000);
  }

  return false;
}

class ProcessMonitorHarness {
 public:
  ProcessMonitorHarness() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);

    if (epoll_fd_ == -1) {
      const int error_number = errno;
      throw std::system_error(error_number, std::generic_category(), "epoll_create1");
    }

    try {
      monitor_ = std::make_unique<runnerd::ProcessMonitor>(epoll_fd_, jobs_);
    } catch (...) {
      static_cast<void>(::close(epoll_fd_));
      epoll_fd_ = -1;
      throw;
    }
  }

  ProcessMonitorHarness(const ProcessMonitorHarness&) = delete;
  ProcessMonitorHarness& operator=(const ProcessMonitorHarness&) = delete;

  ~ProcessMonitorHarness() {
    monitor_.reset();

    if (epoll_fd_ != -1) {
      static_cast<void>(::close(epoll_fd_));
    }
  }

  void addJob(runnerd::JobId job_id, std::vector<std::string> arguments) {
    runnerd::Job job;
    job.id = job_id;
    job.spec.argv = std::move(arguments);

    const bool inserted = jobs_.emplace(job_id, std::move(job)).second;

    if (!inserted) {
      throw std::logic_error("duplicate test job id");
    }
  }

  void startJob(runnerd::JobId job_id) {
    monitor_->startJob(job_id);
  }

  bool requestTerminate(runnerd::JobId job_id, runnerd::TerminationCause cause) {
    return monitor_->requestTerminate(job_id, cause);
  }

  const runnerd::Job& job(runnerd::JobId job_id) const {
    return jobs_.at(job_id);
  }

  void waitForJob(runnerd::JobId job_id) {
    waitUntil([this, job_id]() { return runnerd::isTerminal(jobs_.at(job_id).state); });
  }

  void waitForJobs(const std::vector<runnerd::JobId>& job_ids) {
    waitUntil([this, &job_ids]() {
      for (runnerd::JobId job_id : job_ids) {
        if (!runnerd::isTerminal(jobs_.at(job_id).state)) {
          return false;
        }
      }

      return true;
    });
  }

 private:
  template <typename Predicate>
  void waitUntil(Predicate predicate) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);

    for (;;) {
      if (predicate()) {
        return;
      }

      const Clock::time_point now = Clock::now();

      if (now >= deadline) {
        throw std::runtime_error("timed out waiting for child process completion");
      }

      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
      const int wait_milliseconds = remaining > 100 ? 100 : static_cast<int>(remaining);

      epoll_event events[16];
      const int ready_count =
          ::epoll_wait(epoll_fd_, events, static_cast<int>(std::size(events)), wait_milliseconds);

      if (ready_count == -1) {
        const int error_number = errno;

        if (error_number == EINTR) {
          continue;
        }

        throw std::system_error(error_number, std::generic_category(), "epoll_wait");
      }

      for (int index = 0; index < ready_count; ++index) {
        const int fd = events[index].data.fd;

        if (!monitor_->ownsFileDescriptor(fd)) {
          throw std::logic_error("epoll returned an unknown test descriptor");
        }

        monitor_->handleFileDescriptorEvent(fd, events[index].events);
      }
    }
  }

  int epoll_fd_ = -1;
  runnerd::JobTable jobs_;
  std::unique_ptr<runnerd::ProcessMonitor> monitor_;
};

TEST(ProcessMonitorTest, ExecutesJobAndCapturesOutput) {
  ProcessMonitorHarness harness;
  harness.addJob(1, {"/bin/echo", "hello"});

  harness.startJob(1);
  harness.waitForJob(1);

  const runnerd::Job& job = harness.job(1);

  EXPECT_EQ(job.state, runnerd::JobState::kSucceeded);
  ASSERT_TRUE(job.exit_code.has_value());
  EXPECT_EQ(*job.exit_code, 0);
  EXPECT_FALSE(job.exit_signal.has_value());
  EXPECT_EQ(job.standard_output, "hello\n");
  EXPECT_TRUE(job.standard_error.empty());
  EXPECT_TRUE(job.failure_message.empty());
}

TEST(ProcessMonitorTest, ReportsExecveFailure) {
  ProcessMonitorHarness harness;
  harness.addJob(1, {"/definitely/not/a/real/executable"});

  harness.startJob(1);
  harness.waitForJob(1);

  const runnerd::Job& job = harness.job(1);

  EXPECT_EQ(job.state, runnerd::JobState::kFailed);
  EXPECT_NE(job.failure_message.find("execve"), std::string::npos);
  EXPECT_TRUE(job.standard_output.empty());
  EXPECT_TRUE(job.standard_error.empty());
}

TEST(ProcessMonitorTest, RecordsNonzeroExitAndBothOutputStreams) {
  ProcessMonitorHarness harness;
  harness.addJob(1, {"/bin/sh", "-c", "printf stdout; printf stderr >&2; exit 7"});

  harness.startJob(1);
  harness.waitForJob(1);

  const runnerd::Job& job = harness.job(1);

  EXPECT_EQ(job.state, runnerd::JobState::kFailed);
  ASSERT_TRUE(job.exit_code.has_value());
  EXPECT_EQ(*job.exit_code, 7);
  EXPECT_EQ(job.standard_output, "stdout");
  EXPECT_EQ(job.standard_error, "stderr");
  EXPECT_NE(job.failure_message.find("code 7"), std::string::npos);
}

TEST(ProcessMonitorTest, DrainsOutputLargerThanPipeCapacity) {
  ProcessMonitorHarness harness;
  harness.addJob(1, {
                        "/bin/sh",
                        "-c",
                        "i=0; while [ \"$i\" -lt 20000 ]; do "
                        "printf 0123456789; i=$((i + 1)); done",
                    });

  harness.startJob(1);
  harness.waitForJob(1);

  const runnerd::Job& job = harness.job(1);

  EXPECT_EQ(job.state, runnerd::JobState::kSucceeded);
  EXPECT_EQ(job.standard_output.size(), 200000U);
  EXPECT_TRUE(job.standard_error.empty());
}

TEST(ProcessMonitorTest, ReapsMultipleQuicklyExitingChildren) {
  ProcessMonitorHarness harness;
  std::vector<runnerd::JobId> job_ids;

  for (runnerd::JobId job_id = 1; job_id <= 20; ++job_id) {
    job_ids.push_back(job_id);
    harness.addJob(job_id, {"/bin/true"});
    harness.startJob(job_id);
  }

  harness.waitForJobs(job_ids);

  for (runnerd::JobId job_id : job_ids) {
    SCOPED_TRACE(job_id);
    const runnerd::Job& job = harness.job(job_id);

    EXPECT_EQ(job.state, runnerd::JobState::kSucceeded);
    ASSERT_TRUE(job.exit_code.has_value());
    EXPECT_EQ(*job.exit_code, 0);
  }
}

TEST(ProcessMonitorTest, MarksInvalidQueuedJobAsFailed) {
  ProcessMonitorHarness harness;
  harness.addJob(1, {});

  harness.startJob(1);

  const runnerd::Job& job = harness.job(1);

  EXPECT_EQ(job.state, runnerd::JobState::kFailed);
  EXPECT_NE(job.failure_message.find("launchProcess failed"), std::string::npos);
  EXPECT_FALSE(job.pid.has_value());
}

TEST(ProcessMonitorTest, CancelsRunningJobAfterDrainingRemainingOutput) {
  ProcessMonitorHarness harness;
  TemporaryPath ready_marker;

  // 先安装 SIGTERM handler 并写出初始输出，再创建 marker 通知测试线程。
  // handler 会在收到取消信号后补写尾部输出，验证 ProcessMonitor 没有提前关 pipe。
  harness.addJob(1, {
                        "/bin/sh",
                        "-c",
                        "trap 'printf tail; printf errtail >&2; exit 0' TERM; "
                        "printf head; printf errhead >&2; : > \"$1\"; "
                        "while :; do sleep 30; done",
                        "runnerd-process-monitor-test",
                        ready_marker.get(),
                    });

  harness.startJob(1);
  ASSERT_TRUE(waitForPath(ready_marker.get()));

  EXPECT_TRUE(harness.requestTerminate(1, runnerd::TerminationCause::kCancelled));

  const runnerd::Job& terminating_job = harness.job(1);
  EXPECT_EQ(terminating_job.state, runnerd::JobState::kTerminating);
  ASSERT_TRUE(terminating_job.termination_cause.has_value());
  EXPECT_EQ(*terminating_job.termination_cause, runnerd::TerminationCause::kCancelled);

  harness.waitForJob(1);

  const runnerd::Job& cancelled_job = harness.job(1);
  EXPECT_EQ(cancelled_job.state, runnerd::JobState::kCancelled);
  ASSERT_TRUE(cancelled_job.exit_code.has_value());
  EXPECT_EQ(*cancelled_job.exit_code, 0);
  EXPECT_FALSE(cancelled_job.exit_signal.has_value());
  EXPECT_EQ(cancelled_job.standard_output, "headtail");
  EXPECT_NE(cancelled_job.standard_error.find("errhead"), std::string::npos);
  EXPECT_NE(cancelled_job.standard_error.find("errtail"), std::string::npos);
  EXPECT_TRUE(cancelled_job.failure_message.empty());
}

}  // namespace

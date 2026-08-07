#include "runnerd/timeout_manager.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace {

class TimeoutManagerHarness {
 public:
  TimeoutManagerHarness() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);

    if (epoll_fd_ == -1) {
      throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }

    try {
      manager_ = std::make_unique<runnerd::TimeoutManager>(epoll_fd_);
    } catch (...) {
      static_cast<void>(::close(epoll_fd_));
      epoll_fd_ = -1;
      throw;
    }
  }

  TimeoutManagerHarness(const TimeoutManagerHarness&) = delete;
  TimeoutManagerHarness& operator=(const TimeoutManagerHarness&) = delete;

  ~TimeoutManagerHarness() {
    manager_.reset();

    if (epoll_fd_ != -1) {
      static_cast<void>(::close(epoll_fd_));
    }
  }

  runnerd::TimeoutManager& manager() {
    return *manager_;
  }

  bool waitUntilReadable(std::chrono::milliseconds timeout) const {
    using Clock = std::chrono::steady_clock;

    const Clock::time_point deadline = Clock::now() + timeout;

    for (;;) {
      const Clock::time_point now = Clock::now();

      if (now >= deadline) {
        return false;
      }

      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
      const int wait_milliseconds =
          remaining > 0 ? static_cast<int>(remaining) : 1;

      epoll_event event{};
      const int ready_count = ::epoll_wait(epoll_fd_, &event, 1, wait_milliseconds);

      if (ready_count == 1) {
        if (event.data.fd != manager_->fileDescriptor()) {
          throw std::logic_error("epoll returned an unknown timeout descriptor");
        }

        if ((event.events & EPOLLIN) == 0) {
          throw std::logic_error("timerfd became ready without EPOLLIN");
        }

        return true;
      }

      if (ready_count == 0) {
        continue;
      }

      const int error_number = errno;

      if (error_number == EINTR) {
        continue;
      }

      throw std::system_error(error_number, std::generic_category(), "epoll_wait for timerfd");
    }
  }

 private:
  int epoll_fd_ = -1;
  std::unique_ptr<runnerd::TimeoutManager> manager_;
};

TEST(TimeoutManagerTest, RejectsInvalidConstructionAndTimeoutValues) {
  EXPECT_THROW(runnerd::TimeoutManager(-1), std::invalid_argument);

  TimeoutManagerHarness harness;

  EXPECT_THROW(harness.manager().schedule(1, runnerd::JobTimeout(0)), std::invalid_argument);
  EXPECT_THROW(harness.manager().schedule(1, runnerd::JobTimeout(-1)), std::invalid_argument);
}

TEST(TimeoutManagerTest, CreatesNonBlockingCloseOnExecTimerFd) {
  TimeoutManagerHarness harness;
  const int timer_fd = harness.manager().fileDescriptor();

  ASSERT_GE(timer_fd, 0);

  const int status_flags = ::fcntl(timer_fd, F_GETFL);
  ASSERT_NE(status_flags, -1) << std::strerror(errno);
  EXPECT_NE(status_flags & O_NONBLOCK, 0);

  const int descriptor_flags = ::fcntl(timer_fd, F_GETFD);
  ASSERT_NE(descriptor_flags, -1) << std::strerror(errno);
  EXPECT_NE(descriptor_flags & FD_CLOEXEC, 0);
}

TEST(TimeoutManagerTest, ExpiresJobsInDeadlineOrder) {
  TimeoutManagerHarness harness;

  harness.manager().schedule(1, runnerd::JobTimeout(200));
  harness.manager().schedule(2, runnerd::JobTimeout(40));

  ASSERT_TRUE(harness.waitUntilReadable(std::chrono::milliseconds(150)));
  EXPECT_EQ(harness.manager().handleReadable(), std::vector<runnerd::JobId>({2}));

  ASSERT_TRUE(harness.waitUntilReadable(std::chrono::milliseconds(250)));
  EXPECT_EQ(harness.manager().handleReadable(), std::vector<runnerd::JobId>({1}));
}

TEST(TimeoutManagerTest, ReturnsEveryJobExpiredBeforeOneWakeup) {
  TimeoutManagerHarness harness;

  harness.manager().schedule(3, runnerd::JobTimeout(30));
  harness.manager().schedule(1, runnerd::JobTimeout(30));
  harness.manager().schedule(2, runnerd::JobTimeout(30));

  // 三次 schedule 的实际 deadline 会相差极短时间。
  // 等它们全部到期后再读取，验证一次唤醒会返回本轮所有到期任务。
  ::usleep(80'000);

  ASSERT_TRUE(harness.waitUntilReadable(std::chrono::milliseconds(100)));

  std::vector<runnerd::JobId> expired_jobs = harness.manager().handleReadable();
  std::sort(expired_jobs.begin(), expired_jobs.end());

  EXPECT_EQ(expired_jobs, std::vector<runnerd::JobId>({1, 2, 3}));
}

TEST(TimeoutManagerTest, ReschedulingJobInvalidatesItsOldDeadline) {
  TimeoutManagerHarness harness;

  harness.manager().schedule(1, runnerd::JobTimeout(100));
  harness.manager().schedule(1, runnerd::JobTimeout(300));

  // generation 1 的旧期限不能再使 timerfd 唤醒。
  EXPECT_FALSE(harness.waitUntilReadable(std::chrono::milliseconds(150)));

  ASSERT_TRUE(harness.waitUntilReadable(std::chrono::milliseconds(250)));
  EXPECT_EQ(harness.manager().handleReadable(), std::vector<runnerd::JobId>({1}));
}

TEST(TimeoutManagerTest, CancelledDeadlineCannotExpireAndCancelIsIdempotent) {
  TimeoutManagerHarness harness;

  harness.manager().schedule(1, runnerd::JobTimeout(30));
  harness.manager().schedule(2, runnerd::JobTimeout(80));

  harness.manager().cancel(1);
  harness.manager().cancel(1);
  harness.manager().cancel(999);

  // cancel() 使用惰性删除。等两个原始期限都过去后一次性处理堆，
  // 已取消的 Job 1 必须被丢弃，仍有效的 Job 2 必须正常返回。
  ::usleep(120'000);

  ASSERT_TRUE(harness.waitUntilReadable(std::chrono::milliseconds(100)));
  EXPECT_EQ(harness.manager().handleReadable(), std::vector<runnerd::JobId>({2}));
}

}  // namespace

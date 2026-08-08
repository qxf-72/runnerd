#include "runnerd/timeout_manager.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace runnerd {

namespace {

constexpr std::int64_t kNanosecondsPerSecond = 1000000000LL;

// 根据保存下来的 errno 构造 system_error。
//
// 必须把 errno 先保存下来，再调用 close 等其他系统调用，
// 否则后面的系统调用可能覆盖原始 errno。
std::system_error makeSystemError(int error_number, const char* operation) {
  return std::system_error(error_number, std::generic_category(), operation);
}

}  // namespace

TimeoutManager::TimeoutManager(int epoll_fd) : epoll_fd_(epoll_fd) {
  if (epoll_fd_ < 0) {
    throw std::invalid_argument("TimeoutManager requires a valid epoll fd");
  }

  // CLOCK_MONOTONIC 表示单调时钟。
  //
  // 它不受用户修改系统时间、NTP 校时等操作影响，
  // 很适合计算“运行了多久”。
  //
  // TFD_NONBLOCK：
  //   timerfd 上没有事件时，read 返回 EAGAIN，不阻塞事件循环。
  //
  // TFD_CLOEXEC：
  //   子进程 execve 后不会继承 runnerd 的 timerfd。
  timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

  if (timer_fd_ == -1) {
    const int error_number = errno;
    throw makeSystemError(error_number, "timerfd_create");
  }

  epoll_event event{};
  event.events = EPOLLIN;
  event.data.fd = timer_fd_;

  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &event) == -1) {
    const int error_number = errno;

    ::close(timer_fd_);
    timer_fd_ = -1;

    throw makeSystemError(error_number, "add timerfd to epoll");
  }
}

TimeoutManager::~TimeoutManager() {
  if (timer_fd_ == -1) {
    return;
  }

  // 析构函数不能抛异常。
  // 即使 EPOLL_CTL_DEL 失败，close 也会让这个 fd 自动离开 epoll。
  static_cast<void>(::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, timer_fd_, nullptr));

  ::close(timer_fd_);
  timer_fd_ = -1;
}

int TimeoutManager::fileDescriptor() const noexcept {
  return timer_fd_;
}

void TimeoutManager::schedule(JobId job_id, JobTimeout timeout) {
  if (timeout.count() <= 0) {
    throw std::invalid_argument("execution timeout must be positive");
  }

  // 0 被保留为无效 generation。
  // uint64_t 自增溢出后会回到 0，所以在使用前检查。
  if (next_generation_ == 0) {
    throw std::overflow_error("timeout generation space exhausted");
  }

  const std::uint64_t generation = next_generation_++;

  // deadline 从此刻开始计算。
  //
  // 当前既可用于 RUNNING 任务的 execution timeout，
  // 也可用于 TERMINATING 任务的 SIGKILL 宽限期。
  const TimePoint deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(timeout);

  const DeadlineEntry entry{
      deadline,
      job_id,
      generation,
  };

  // 先把新记录放入堆。
  deadlines_.push(entry);

  // 新 generation 成为这个 JobId 当前唯一有效的版本。
  //
  // 如果之前存在相同 JobId，旧堆记录不会立即删除，
  // 但 isCurrent() 会把它识别为失效记录。
  active_generations_.insert_or_assign(job_id, generation);

  // 新任务的期限可能比当前 timerfd 等待的期限更早，
  // 因此每次新增期限后都重新设置 timerfd。
  armToNextDeadline();
}

void TimeoutManager::cancel(JobId job_id) noexcept {
  // priority_queue 无法高效删除中间元素。
  //
  // 这里只删除有效 generation。
  // 堆中对应的 DeadlineEntry 会变成 stale entry，
  // 以后到达堆顶时再丢弃。
  active_generations_.erase(job_id);

  // 这里故意不立即重设 timerfd。
  //
  // 最坏情况只是 timerfd 在一个已经取消的旧期限上提前唤醒一次。
  // handleReadable() 会清理旧记录并设置真正的下一个期限。
  //
  // 这样 cancel() 可以保持 noexcept，也使终态清理更简单。
}

bool TimeoutManager::isCurrent(const DeadlineEntry& entry) const {
  const auto active_it = active_generations_.find(entry.job_id);

  if (active_it == active_generations_.end()) {
    // JobId 已经被 cancel()，该记录失效。
    return false;
  }

  // generation 不一致，说明同一个 JobId 后来设置过新期限。
  return active_it->second == entry.generation;
}

void TimeoutManager::discardStaleEntries() {
  while (!deadlines_.empty() && !isCurrent(deadlines_.top())) {
    deadlines_.pop();
  }
}

void TimeoutManager::armToNextDeadline() {
  // 先把堆顶已经取消或被替代的记录移除，
  // 确保堆顶确实是最近的有效期限。
  discardStaleEntries();

  itimerspec timer_spec{};

  if (!deadlines_.empty()) {
    const TimePoint now = Clock::now();
    const TimePoint deadline = deadlines_.top().deadline;

    // timerfd_settime 在没有 TFD_TIMER_ABSTIME 时接收相对时间：
    //
    //   “从现在开始，多久之后触发”
    //
    // 因此需要计算 deadline - now。
    std::chrono::nanoseconds remaining =
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);

    // it_value 全为 0 表示解除定时器。
    // 如果期限已经到达，不能设置为 0，否则不会产生事件。
    //
    // 使用 1 纳秒表示“尽快产生一次事件”。
    if (remaining.count() <= 0) {
      remaining = std::chrono::nanoseconds(1);
    }

    const std::int64_t total_nanoseconds = remaining.count();

    timer_spec.it_value.tv_sec = total_nanoseconds / kNanosecondsPerSecond;

    timer_spec.it_value.tv_nsec = total_nanoseconds % kNanosecondsPerSecond;
  }

  // deadlines_ 为空时，timer_spec 保持全 0，
  // 这会解除当前 timerfd。
  for (;;) {
    if (::timerfd_settime(timer_fd_, 0, &timer_spec, nullptr) == 0) {
      return;
    }

    const int error_number = errno;

    if (error_number == EINTR) {
      continue;
    }

    throw makeSystemError(error_number, "timerfd_settime");
  }
}

std::vector<JobId> TimeoutManager::handleReadable() {
  // timerfd 的 read 结果不是普通字节流，
  // 而是一个 uint64_t，表示自上次读取以来到期了多少次。
  //
  // 目前使用的是一次性定时器，不设置重复间隔，
  // 但仍应按 timerfd 接口要求读取完整的 8 字节。
  for (;;) {
    std::uint64_t expiration_count = 0;

    const ssize_t read_size = ::read(timer_fd_, &expiration_count, sizeof(expiration_count));

    if (read_size == static_cast<ssize_t>(sizeof(expiration_count))) {
      // 一次 read 通常已经清空可读状态。
      // 继续读取到 EAGAIN，使非阻塞 fd 被彻底排空。
      continue;
    }

    if (read_size == -1 && errno == EINTR) {
      continue;
    }

    if (read_size == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }

    if (read_size == -1) {
      const int error_number = errno;

      throw makeSystemError(error_number, "read timerfd");
    }

    // timerfd 正常情况下只能返回完整的 uint64_t。
    throw std::runtime_error("timerfd returned an unexpected read size");
  }

  std::vector<JobId> expired_jobs;

  // 只读取一次“现在”，保证本轮判断使用同一个时间点。
  const TimePoint now = Clock::now();

  discardStaleEntries();

  while (!deadlines_.empty()) {
    const DeadlineEntry entry = deadlines_.top();

    if (entry.deadline > now) {
      // 最早的有效期限都还没到，
      // 后面的期限更不可能到期。
      break;
    }

    deadlines_.pop();

    // discardStaleEntries() 已保证堆顶有效，
    // 但这里仍通过 generation 精确删除当前版本。
    const auto active_it = active_generations_.find(entry.job_id);

    if (active_it == active_generations_.end() || active_it->second != entry.generation) {
      continue;
    }

    active_generations_.erase(active_it);
    expired_jobs.push_back(entry.job_id);

    // 下一条堆记录可能是旧版本，继续清理。
    discardStaleEntries();
  }

  // 当前到期任务全部取出后，
  // 重新等待下一个最近期限。
  armToNextDeadline();

  return expired_jobs;
}

}  // namespace runnerd

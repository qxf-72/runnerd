#ifndef RUNNERD_TIMEOUT_MANAGER_H
#define RUNNERD_TIMEOUT_MANAGER_H

#include <chrono>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

#include "runnerd/job.h"

namespace runnerd {

// TimeoutManager 只负责执行超时的“时间管理”。
//
// 它负责：
// 1. 记录某个 JobId 的执行截止时间；
// 2. 使用一个最小堆找到最近的截止时间；
// 3. 使用一个 timerfd 通知 epoll；
// 4. timerfd 到期后返回所有已经过期的 JobId。
//
// 它不负责：
// 1. 不修改 JobState；
// 2. 不发送 SIGTERM；
// 3. 不回收子进程；
// 4. 不管理调度槽位。
//
// 到期任务应该如何终止，由 daemon 调用
// ProcessMonitor::requestTerminate() 决定。
class TimeoutManager {
 public:
  // 创建一个基于 CLOCK_MONOTONIC 的 timerfd，
  // 并将它注册到传入的 epoll 实例中。
  explicit TimeoutManager(int epoll_fd);

  ~TimeoutManager();

  TimeoutManager(const TimeoutManager&) = delete;
  TimeoutManager& operator=(const TimeoutManager&) = delete;

  // 返回 timerfd。
  //
  // runnerd 的主事件循环通过这个值判断：
  // 当前 epoll 事件是否属于 TimeoutManager。
  int fileDescriptor() const noexcept;

  // 从“现在”开始，为任务设置执行超时。
  //
  // 这个函数只能在任务成功进入 RUNNING 后调用。
  // 因此排队等待时间不会计入 execution_timeout。
  //
  // 如果相同 JobId 再次被 schedule，新的期限会替代旧期限。
  // 旧的堆记录会通过 generation 被识别为失效记录。
  void schedule(JobId job_id, JobTimeout timeout);

  // 取消一个任务尚未触发的超时期限。
  //
  // 该函数是幂等的：
  // 即使这个 JobId 没有超时记录，也不会报错。
  //
  // priority_queue 不支持删除中间元素，因此这里只从
  // active_generations_ 中删除。堆中的旧记录以后会被
  // discardStaleEntries() 自动清理，这称为“惰性删除”。
  void cancel(JobId job_id) noexcept;

  // timerfd 可读时调用。
  //
  // 该函数会：
  // 1. 读取 timerfd 的到期计数；
  // 2. 取出所有已经到期的有效 JobId；
  // 3. 将 timerfd 重新设置为等待下一个期限。
  //
  // 返回值只表示“这些任务到期了”。
  // 调用者仍然需要决定如何终止它们。
  std::vector<JobId> handleReadable();

 private:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  // 最小堆中的一条记录。
  struct DeadlineEntry {
    TimePoint deadline;
    JobId job_id = 0;
    std::uint64_t generation = 0;
  };

  // std::priority_queue 默认把最大元素放在顶部。
  // 这里反转比较关系，使最早的 deadline 位于顶部。
  struct EarlierDeadline {
    bool operator()(const DeadlineEntry& left, const DeadlineEntry& right) const noexcept {
      if (left.deadline != right.deadline) {
        return left.deadline > right.deadline;
      }

      // deadline 相同时使用 JobId 保持稳定顺序。
      return left.job_id > right.job_id;
    }
  };

  int epoll_fd_ = -1;
  int timer_fd_ = -1;

  // 每次 schedule 都分配一个新的版本号。
  //
  // 假设 Job 1 先设置了期限 A，后来又改成期限 B：
  //
  // 堆中可能同时存在：
  //   Job 1, generation 1, deadline A
  //   Job 1, generation 2, deadline B
  //
  // active_generations_[1] == 2，说明 generation 1 已失效。
  std::uint64_t next_generation_ = 1;

  std::priority_queue<DeadlineEntry, std::vector<DeadlineEntry>, EarlierDeadline> deadlines_;

  // 这里保存每个任务当前有效的 generation。
  //
  // 这个 map 才是“某个期限是否有效”的权威数据。
  std::unordered_map<JobId, std::uint64_t> active_generations_;

  // 判断一条堆记录是否仍然有效。
  bool isCurrent(const DeadlineEntry& entry) const;

  // 移除位于堆顶的所有过期版本或已取消记录。
  void discardStaleEntries();

  // 将 timerfd 设置为等待当前最近的有效期限。
  //
  // 如果不存在有效期限，就解除 timerfd。
  void armToNextDeadline();
};

}  // namespace runnerd

#endif  // RUNNERD_TIMEOUT_MANAGER_H

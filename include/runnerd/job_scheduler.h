#ifndef RUNNERD_JOB_SCHEDULER_H
#define RUNNERD_JOB_SCHEDULER_H

#include <cstddef>
#include <deque>
#include <optional>
#include <unordered_set>

#include "runnerd/job.h"

namespace runnerd {

// JobScheduler 只负责“调度顺序”和“并发槽位”。
// 它不启动进程，也不修改 JobState。
class JobScheduler {
 public:
  explicit JobScheduler(std::size_t max_running);

  // 将一个新任务放到 FIFO 等待队列末尾。
  //
  // 调用者必须保证该 JobId 对应的 Job 已经存在，且状态为 QUEUED。
  // 同一个 JobId 不能重复进入等待队列，也不能在已经占用槽位时再次入队。
  void enqueue(JobId job_id);

  // 如果存在空闲槽位且等待队列非空，返回最早提交的 JobId。
  std::optional<JobId> takeNextJobToStart();

  // 通知调度器：某个已经占用槽位的任务已经进入终态。
  void onJobReachedTerminalState(JobId job_id);

  // 从等待队列中移除一个尚未启动的任务。
  bool cancelQueuedJob(JobId job_id);

  std::size_t maxRunning() const noexcept;
  std::size_t runningCount() const noexcept;
  std::size_t waitingCount() const noexcept;

  bool isWaiting(JobId job_id) const;
  bool isRunning(JobId job_id) const;

 private:
  // 最大并发槽位数。构造时保证它大于 0。
  std::size_t max_running_;

  std::deque<JobId> waiting_jobs_;

  // 用 set 快速判断 JobId 是否重复入队。
  // deque 负责顺序，unordered_set 负责 O(1) 的成员检查。
  std::unordered_set<JobId> waiting_job_ids_;

  // 已经占用运行槽位的任务。
  // 任务一旦被 takeNextJobToStart() 返回，就会进入这里。
  std::unordered_set<JobId> active_job_ids_;
};

}  // namespace runnerd

#endif  // RUNNERD_JOB_SCHEDULER_H

#include "runnerd/job_scheduler.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace runnerd {

JobScheduler::JobScheduler(std::size_t max_running) : max_running_(max_running) {
  if (max_running_ == 0) {
    throw std::invalid_argument("max_running must be greater than zero");
  }
}

void JobScheduler::enqueue(JobId job_id) {
  if (job_id == 0) {
    throw std::invalid_argument("job id must be greater than zero");
  }

  if (waiting_job_ids_.find(job_id) != waiting_job_ids_.end() ||
      active_job_ids_.find(job_id) != active_job_ids_.end()) {
    throw std::logic_error("job is already scheduled");
  }

  // 先插入 set，再插入 deque。
  //
  // 如果 deque 的内存分配抛出异常，需要撤销前面对 set 的修改，
  // 让对象保持“要么完全入队，要么完全没入队”的一致状态。
  const bool inserted = waiting_job_ids_.insert(job_id).second;

  if (!inserted) {
    throw std::logic_error("duplicate waiting job id");
  }

  try {
    waiting_jobs_.push_back(job_id);
  } catch (...) {
    waiting_job_ids_.erase(job_id);
    throw;
  }
}

std::optional<JobId> JobScheduler::takeNextJobToStart() {
  if (active_job_ids_.size() >= max_running_) {
    return std::nullopt;
  }

  if (waiting_jobs_.empty()) {
    return std::nullopt;
  }

  const JobId job_id = waiting_jobs_.front();

  // 在从队列移除之前，先让该 Job 占用运行槽位。
  //
  // 这样即使后续代码观察调度器，也不会错误地认为还有空闲容量。
  const bool inserted = active_job_ids_.insert(job_id).second;

  if (!inserted) {
    // 正常情况下不可能发生，因为 enqueue() 已阻止重复 JobId。
    // 如果发生，说明调度器内部状态已经不一致，应立刻报错。
    throw std::logic_error("waiting job already occupies a running slot");
  }

  waiting_jobs_.pop_front();
  waiting_job_ids_.erase(job_id);

  return job_id;
}

void JobScheduler::onJobReachedTerminalState(JobId job_id) {
  // 任务进入终态后，必须释放它占用的运行槽位。
  //
  // 这包括两种情况：
  // 1. 子进程正常启动，后来退出；
  // 2. 调度器已经给任务预留槽位，但启动过程失败，
  //    任务直接进入 FAILED。
  const std::size_t erased_count = active_job_ids_.erase(job_id);

  if (erased_count == 0) {
    throw std::logic_error("terminal job does not occupy a running slot");
  }
}

bool JobScheduler::cancelQueuedJob(JobId job_id) {
  // 只有等待中的 Job 才能在本层被取消。
  const auto waiting_id_it = waiting_job_ids_.find(job_id);

  if (waiting_id_it == waiting_job_ids_.end()) {
    return false;
  }

  // deque 负责顺序，但删除中间元素需要线性查找。
  // 当前项目的等待队列规模有限，这个 O(n) 操作完全足够。
  const auto queue_it = std::find(waiting_jobs_.begin(), waiting_jobs_.end(), job_id);

  if (queue_it == waiting_jobs_.end()) {
    // set 中有、deque 中没有，说明内部数据结构失去一致性。
    throw std::logic_error("waiting job id is missing from FIFO queue");
  }

  waiting_jobs_.erase(queue_it);
  waiting_job_ids_.erase(waiting_id_it);

  return true;
}

std::size_t JobScheduler::maxRunning() const noexcept {
  return max_running_;
}

std::size_t JobScheduler::runningCount() const noexcept {
  return active_job_ids_.size();
}

std::size_t JobScheduler::waitingCount() const noexcept {
  return waiting_jobs_.size();
}

bool JobScheduler::isWaiting(JobId job_id) const {
  return waiting_job_ids_.find(job_id) != waiting_job_ids_.end();
}

bool JobScheduler::isRunning(JobId job_id) const {
  return active_job_ids_.find(job_id) != active_job_ids_.end();
}

}  // namespace runnerd

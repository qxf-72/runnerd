#include "runnerd/job_scheduler.h"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>

namespace {

TEST(JobSchedulerTest, RejectsZeroMaximumConcurrency) {
  // 并发数为 0 时，任何任务都无法启动，因此构造应拒绝。
  EXPECT_THROW(runnerd::JobScheduler scheduler(0), std::invalid_argument);
}

TEST(JobSchedulerTest, StartsJobsInFifoOrderUntilConcurrencyLimit) {
  runnerd::JobScheduler scheduler(2);

  scheduler.enqueue(1);
  scheduler.enqueue(2);
  scheduler.enqueue(3);

  // 容量为 2，因此前两个 Job 可以依次取得启动资格。
  const std::optional<runnerd::JobId> first = scheduler.takeNextJobToStart();

  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, 1);

  const std::optional<runnerd::JobId> second = scheduler.takeNextJobToStart();

  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*second, 2);

  // 两个运行槽位都已占用，第三个任务必须继续等待。
  EXPECT_FALSE(scheduler.takeNextJobToStart().has_value());

  EXPECT_EQ(scheduler.runningCount(), 2U);
  EXPECT_EQ(scheduler.waitingCount(), 1U);
  EXPECT_TRUE(scheduler.isWaiting(3));
  EXPECT_TRUE(scheduler.isRunning(1));
  EXPECT_TRUE(scheduler.isRunning(2));
}

TEST(JobSchedulerTest, TerminalJobFreesSlotForNextWaitingJob) {
  runnerd::JobScheduler scheduler(2);

  scheduler.enqueue(1);
  scheduler.enqueue(2);
  scheduler.enqueue(3);

  ASSERT_TRUE(scheduler.takeNextJobToStart().has_value());
  ASSERT_TRUE(scheduler.takeNextJobToStart().has_value());

  // Job 1 结束，释放一个运行槽位。
  scheduler.onJobReachedTerminalState(1);

  EXPECT_EQ(scheduler.runningCount(), 1U);

  // 此时排队最久的 Job 3 应自动获得启动资格。
  const std::optional<runnerd::JobId> next = scheduler.takeNextJobToStart();

  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(*next, 3);

  EXPECT_EQ(scheduler.runningCount(), 2U);
  EXPECT_EQ(scheduler.waitingCount(), 0U);
  EXPECT_TRUE(scheduler.isRunning(3));
}

TEST(JobSchedulerTest, CancellingQueuedJobPreservesOtherJobsFifoOrder) {
  runnerd::JobScheduler scheduler(1);

  scheduler.enqueue(1);
  scheduler.enqueue(2);
  scheduler.enqueue(3);

  // Job 2 还没运行，因此可以直接从等待队列移除。
  EXPECT_TRUE(scheduler.cancelQueuedJob(2));

  const std::optional<runnerd::JobId> first = scheduler.takeNextJobToStart();

  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, 1);

  // Job 1 结束后，应该轮到 Job 3，而不是已经取消的 Job 2。
  scheduler.onJobReachedTerminalState(1);

  const std::optional<runnerd::JobId> second = scheduler.takeNextJobToStart();

  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*second, 3);

  EXPECT_FALSE(scheduler.cancelQueuedJob(2));
}

TEST(JobSchedulerTest, RejectsDuplicateOrUnknownStateChanges) {
  runnerd::JobScheduler scheduler(1);

  scheduler.enqueue(1);

  // 同一个 Job 不能重复入队。
  EXPECT_THROW(scheduler.enqueue(1), std::logic_error);

  ASSERT_TRUE(scheduler.takeNextJobToStart().has_value());

  // 已经占用槽位的 Job 也不能再次入队。
  EXPECT_THROW(scheduler.enqueue(1), std::logic_error);

  // 未占用槽位的 Job 不能“释放槽位”。
  EXPECT_THROW(scheduler.onJobReachedTerminalState(999), std::logic_error);
}

}  // namespace

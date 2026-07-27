#include "runnerd/job.h"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

// 使用异常报告断言失败，避免 Release 模式定义 NDEBUG 后测试失效。
void expect(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// 验证指定操作必须抛出预期类型的异常。
template <typename Exception, typename Function>
void expectThrows(Function&& function, const std::string& message) {
  try {
    function();
  } catch (const Exception&) {
    return;
  }

  throw std::runtime_error(message);
}

// 验证不设置超时和设置正数超时的 JobSpec 都是合法的。
void testValidJobSpecs() {
  runnerd::JobSpec without_timeout;
  without_timeout.argv = {"/bin/echo", "hello"};

  runnerd::validateJobSpec(without_timeout);
  expect(!without_timeout.execution_timeout.has_value(),
         "job without timeout unexpectedly has a timeout");

  runnerd::JobSpec with_timeout = without_timeout;
  with_timeout.execution_timeout = runnerd::JobTimeout(1000);

  runnerd::validateJobSpec(with_timeout);
  expect(with_timeout.execution_timeout->count() == 1000,
         "positive execution timeout was not preserved");
}

// 验证缺少命令、参数含 NUL 字节或超时不为正数时会拒绝 JobSpec。
void testInvalidJobSpecs() {
  runnerd::JobSpec empty_argv;
  expectThrows<std::invalid_argument>([&empty_argv]() { runnerd::validateJobSpec(empty_argv); },
                                      "job with empty argv was accepted");

  runnerd::JobSpec empty_program;
  empty_program.argv = {"", "hello"};
  expectThrows<std::invalid_argument>(
      [&empty_program]() { runnerd::validateJobSpec(empty_program); },
      "job with empty argv[0] was accepted");

  runnerd::JobSpec program_name_only;
  program_name_only.argv = {"echo", "hello"};
  expectThrows<std::invalid_argument>(
      [&program_name_only]() { runnerd::validateJobSpec(program_name_only); },
      "job with a program name instead of an absolute path was accepted");

  runnerd::JobSpec relative_program;
  relative_program.argv = {"./echo", "hello"};
  expectThrows<std::invalid_argument>(
      [&relative_program]() { runnerd::validateJobSpec(relative_program); },
      "job with a relative executable path was accepted");

  runnerd::JobSpec nul_argument;
  nul_argument.argv = {"/bin/echo", std::string("hel\0lo", 6)};
  expectThrows<std::invalid_argument>([&nul_argument]() { runnerd::validateJobSpec(nul_argument); },
                                      "job argument containing a NUL byte was accepted");

  runnerd::JobSpec zero_timeout;
  zero_timeout.argv = {"/bin/echo", "hello"};
  zero_timeout.execution_timeout = runnerd::JobTimeout(0);
  expectThrows<std::invalid_argument>([&zero_timeout]() { runnerd::validateJobSpec(zero_timeout); },
                                      "job with zero execution timeout was accepted");

  runnerd::JobSpec negative_timeout;
  negative_timeout.argv = {"/bin/echo", "hello"};
  negative_timeout.execution_timeout = runnerd::JobTimeout(-1);
  expectThrows<std::invalid_argument>(
      [&negative_timeout]() { runnerd::validateJobSpec(negative_timeout); },
      "job with negative execution timeout was accepted");
}

struct StateTransition {
  runnerd::JobState from;
  runnerd::JobState to;
};

std::string transitionName(const StateTransition& transition) {
  return std::string(runnerd::jobStateName(transition.from)) + " -> " +
         std::string(runnerd::jobStateName(transition.to));
}

// 验证状态机当前定义的每一条合法迁移。
void testAllValidTransitions() {
  const std::array<StateTransition, 10> transitions{{
      {runnerd::JobState::kQueued, runnerd::JobState::kRunning},
      {runnerd::JobState::kQueued, runnerd::JobState::kCancelled},
      {runnerd::JobState::kQueued, runnerd::JobState::kInterrupted},
      {runnerd::JobState::kRunning, runnerd::JobState::kSucceeded},
      {runnerd::JobState::kRunning, runnerd::JobState::kFailed},
      {runnerd::JobState::kRunning, runnerd::JobState::kTerminating},
      {runnerd::JobState::kRunning, runnerd::JobState::kInterrupted},
      {runnerd::JobState::kTerminating, runnerd::JobState::kCancelled},
      {runnerd::JobState::kTerminating, runnerd::JobState::kTimedOut},
      {runnerd::JobState::kTerminating, runnerd::JobState::kInterrupted},
  }};

  for (const StateTransition& transition : transitions) {
    const std::string name = transitionName(transition);
    expect(runnerd::canTransition(transition.from, transition.to),
           "valid transition was rejected: " + name);

    runnerd::Job job;
    job.state = transition.from;
    runnerd::transitionJob(job, transition.to);
    expect(job.state == transition.to, "job state was not updated: " + name);
  }
}

// 验证几种容易误用的非法迁移会被判断并拒绝。
void testTypicalInvalidTransitions() {
  const std::array<StateTransition, 4> transitions{{
      {runnerd::JobState::kQueued, runnerd::JobState::kSucceeded},
      {runnerd::JobState::kRunning, runnerd::JobState::kCancelled},
      {runnerd::JobState::kSucceeded, runnerd::JobState::kRunning},
      {runnerd::JobState::kTimedOut, runnerd::JobState::kFailed},
  }};

  for (const StateTransition& transition : transitions) {
    const std::string name = transitionName(transition);
    expect(!runnerd::canTransition(transition.from, transition.to),
           "invalid transition was accepted: " + name);

    runnerd::Job job;
    job.state = transition.from;
    expectThrows<std::logic_error>(
        [&job, &transition]() { runnerd::transitionJob(job, transition.to); },
        "invalid transition did not throw: " + name);
    expect(job.state == transition.from,
           "rejected transition unexpectedly changed job state: " + name);
  }
}

// 验证所有结束状态都是终态，并补充确认进行中的状态不是终态。
void testTerminalStates() {
  const std::array<runnerd::JobState, 5> terminal_states{{
      runnerd::JobState::kSucceeded,
      runnerd::JobState::kFailed,
      runnerd::JobState::kCancelled,
      runnerd::JobState::kTimedOut,
      runnerd::JobState::kInterrupted,
  }};

  for (runnerd::JobState state : terminal_states) {
    expect(runnerd::isTerminal(state),
           "terminal state was not recognized: " + std::string(runnerd::jobStateName(state)));
  }

  const std::array<runnerd::JobState, 3> non_terminal_states{{
      runnerd::JobState::kQueued,
      runnerd::JobState::kRunning,
      runnerd::JobState::kTerminating,
  }};

  for (runnerd::JobState state : non_terminal_states) {
    expect(!runnerd::isTerminal(state), "non-terminal state was treated as terminal: " +
                                            std::string(runnerd::jobStateName(state)));
  }
}

}  // namespace

int main() {
  try {
    // 每个测试函数只验证一类任务模型行为，失败时由异常统一报告。
    testValidJobSpecs();
    testInvalidJobSpecs();
    testAllValidTransitions();
    testTypicalInvalidTransitions();
    testTerminalStates();

    std::cout << "job tests passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "job test failed: " << exception.what() << '\n';
    return 1;
  }
}

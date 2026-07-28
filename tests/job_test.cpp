#include "runnerd/job.h"

#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <string>

namespace {

struct StateTransition {
  runnerd::JobState from;
  runnerd::JobState to;
};

std::string transitionName(const StateTransition& transition) {
  return std::string(runnerd::jobStateName(transition.from)) + " -> " +
         std::string(runnerd::jobStateName(transition.to));
}

TEST(JobSpecTest, AcceptsAbsoluteCommandWithoutTimeout) {
  runnerd::JobSpec spec;
  spec.argv = {"/bin/echo", "hello"};

  EXPECT_NO_THROW(runnerd::validateJobSpec(spec));
  EXPECT_FALSE(spec.execution_timeout.has_value());
}

TEST(JobSpecTest, AcceptsPositiveTimeout) {
  runnerd::JobSpec spec;
  spec.argv = {"/bin/echo", "hello"};
  spec.execution_timeout = runnerd::JobTimeout(1000);

  EXPECT_NO_THROW(runnerd::validateJobSpec(spec));
  ASSERT_TRUE(spec.execution_timeout.has_value());
  EXPECT_EQ(spec.execution_timeout->count(), 1000);
}

TEST(JobSpecTest, RejectsEmptyArgv) {
  const runnerd::JobSpec spec;

  EXPECT_THROW(runnerd::validateJobSpec(spec), std::invalid_argument);
}

TEST(JobSpecTest, RejectsEmptyProgramPath) {
  runnerd::JobSpec spec;
  spec.argv = {"", "hello"};

  EXPECT_THROW(runnerd::validateJobSpec(spec), std::invalid_argument);
}

TEST(JobSpecTest, RejectsProgramNameWithoutAbsolutePath) {
  runnerd::JobSpec spec;
  spec.argv = {"echo", "hello"};

  EXPECT_THROW(runnerd::validateJobSpec(spec), std::invalid_argument);
}

TEST(JobSpecTest, RejectsRelativeProgramPath) {
  runnerd::JobSpec spec;
  spec.argv = {"./echo", "hello"};

  EXPECT_THROW(runnerd::validateJobSpec(spec), std::invalid_argument);
}

TEST(JobSpecTest, RejectsArgumentContainingNulByte) {
  runnerd::JobSpec spec;
  spec.argv = {"/bin/echo", std::string("hel\0lo", 6)};

  EXPECT_THROW(runnerd::validateJobSpec(spec), std::invalid_argument);
}

TEST(JobSpecTest, RejectsZeroTimeout) {
  runnerd::JobSpec spec;
  spec.argv = {"/bin/echo", "hello"};
  spec.execution_timeout = runnerd::JobTimeout(0);

  EXPECT_THROW(runnerd::validateJobSpec(spec), std::invalid_argument);
}

TEST(JobSpecTest, RejectsNegativeTimeout) {
  runnerd::JobSpec spec;
  spec.argv = {"/bin/echo", "hello"};
  spec.execution_timeout = runnerd::JobTimeout(-1);

  EXPECT_THROW(runnerd::validateJobSpec(spec), std::invalid_argument);
}

TEST(JobStateTest, AllowsEveryDefinedValidTransition) {
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
    SCOPED_TRACE(transitionName(transition));
    EXPECT_TRUE(runnerd::canTransition(transition.from, transition.to));

    runnerd::Job job;
    job.state = transition.from;

    EXPECT_NO_THROW(runnerd::transitionJob(job, transition.to));
    EXPECT_EQ(job.state, transition.to);
  }
}

TEST(JobStateTest, RejectsRepresentativeInvalidTransitions) {
  const std::array<StateTransition, 4> transitions{{
      {runnerd::JobState::kQueued, runnerd::JobState::kSucceeded},
      {runnerd::JobState::kRunning, runnerd::JobState::kCancelled},
      {runnerd::JobState::kSucceeded, runnerd::JobState::kRunning},
      {runnerd::JobState::kTimedOut, runnerd::JobState::kFailed},
  }};

  for (const StateTransition& transition : transitions) {
    SCOPED_TRACE(transitionName(transition));
    EXPECT_FALSE(runnerd::canTransition(transition.from, transition.to));

    runnerd::Job job;
    job.state = transition.from;

    EXPECT_THROW(runnerd::transitionJob(job, transition.to), std::logic_error);
    EXPECT_EQ(job.state, transition.from);
  }
}

TEST(JobStateTest, RecognizesTerminalAndNonTerminalStates) {
  const std::array<runnerd::JobState, 5> terminal_states{{
      runnerd::JobState::kSucceeded,
      runnerd::JobState::kFailed,
      runnerd::JobState::kCancelled,
      runnerd::JobState::kTimedOut,
      runnerd::JobState::kInterrupted,
  }};

  for (runnerd::JobState state : terminal_states) {
    SCOPED_TRACE(std::string(runnerd::jobStateName(state)));
    EXPECT_TRUE(runnerd::isTerminal(state));
  }

  const std::array<runnerd::JobState, 3> non_terminal_states{{
      runnerd::JobState::kQueued,
      runnerd::JobState::kRunning,
      runnerd::JobState::kTerminating,
  }};

  for (runnerd::JobState state : non_terminal_states) {
    SCOPED_TRACE(std::string(runnerd::jobStateName(state)));
    EXPECT_FALSE(runnerd::isTerminal(state));
  }
}

}  // namespace

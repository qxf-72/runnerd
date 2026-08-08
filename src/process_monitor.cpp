#include "runnerd/process_monitor.h"

#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "runnerd/process_launcher.h"

namespace runnerd {

namespace {

void closeFd(int& fd) noexcept {
  if (fd != -1) {
    static_cast<void>(::close(fd));
    fd = -1;
  }
}

void killAndReapProcess(pid_t pid, pid_t process_group_id) noexcept {
  if (pid <= 0) {
    return;
  }

  if (process_group_id > 0) {
    static_cast<void>(::kill(-process_group_id, SIGKILL));
  } else if (pid > 0) {
    static_cast<void>(::kill(pid, SIGKILL));
  }

  int status = 0;

  while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {
  }
}

class LaunchedProcessGuard {
 public:
  explicit LaunchedProcessGuard(LaunchedProcess process) : process_(process) {}

  LaunchedProcessGuard(const LaunchedProcessGuard&) = delete;
  LaunchedProcessGuard& operator=(const LaunchedProcessGuard&) = delete;

  ~LaunchedProcessGuard() {
    if (!owns_process_) {
      return;
    }

    closeFd(process_.stdout_fd);
    closeFd(process_.stderr_fd);
    closeFd(process_.startup_error_fd);
    killAndReapProcess(process_.pid, process_.process_group_id);
  }

  LaunchedProcess& get() {
    return process_;
  }

  void release() noexcept {
    owns_process_ = false;
  }

 private:
  LaunchedProcess process_;
  bool owns_process_ = true;
};

const char* startupStageName(ChildStartupStage stage) {
  switch (stage) {
    case ChildStartupStage::kParentDied:
      return "parent-death check";
    case ChildStartupStage::kSetParentDeathSignal:
      return "PR_SET_PDEATHSIG";
    case ChildStartupStage::kSetProcessGroup:
      return "setpgid";
    case ChildStartupStage::kDuplicateStdin:
      return "dup2 stdin";
    case ChildStartupStage::kDuplicateStdout:
      return "dup2 stdout";
    case ChildStartupStage::kDuplicateStderr:
      return "dup2 stderr";
    case ChildStartupStage::kExecve:
      return "execve";
  }

  return "unknown startup stage";
}

}  // namespace

ProcessMonitor::ProcessMonitor(int epoll_fd, JobTable& jobs) : epoll_fd_(epoll_fd), jobs_(jobs) {
  sigset_t signal_mask{};

  if (::sigemptyset(&signal_mask) == -1) {
    const int error_number = errno;
    throw std::system_error(error_number, std::generic_category(), "empty SIGCHLD signal mask");
  }

  if (::sigaddset(&signal_mask, SIGCHLD) == -1) {
    const int error_number = errno;
    throw std::system_error(error_number, std::generic_category(), "add SIGCHLD to signal mask");
  }

  // 必须在启动任何任务前屏蔽 SIGCHLD，避免子进程很快退出时漏掉通知。
  if (::sigprocmask(SIG_BLOCK, &signal_mask, nullptr) == -1) {
    const int error_number = errno;
    throw std::system_error(error_number, std::generic_category(), "block SIGCHLD");
  }

  sigchld_fd_ = ::signalfd(-1, &signal_mask, SFD_NONBLOCK | SFD_CLOEXEC);

  if (sigchld_fd_ == -1) {
    const int error_number = errno;
    throw std::system_error(error_number, std::generic_category(), "create signalfd");
  }

  epoll_event event{};
  event.events = EPOLLIN;
  event.data.fd = sigchld_fd_;

  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, sigchld_fd_, &event) == -1) {
    const int error_number = errno;
    closeFd(sigchld_fd_);

    throw std::system_error(error_number, std::generic_category(), "add signalfd to epoll");
  }
}

ProcessMonitor::~ProcessMonitor() {
  for (const auto& [fd, ignored] : tracked_fds_) {
    static_cast<void>(ignored);
    static_cast<void>(::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr));
    static_cast<void>(::close(fd));
  }

  if (sigchld_fd_ != -1) {
    static_cast<void>(::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, sigchld_fd_, nullptr));
    closeFd(sigchld_fd_);
  }
}

bool ProcessMonitor::ownsFileDescriptor(int fd) const {
  return fd == sigchld_fd_ || tracked_fds_.find(fd) != tracked_fds_.end();
}

void ProcessMonitor::setTerminalJobCallback(TerminalJobCallback callback) {
  if (!callback) {
    throw std::invalid_argument("terminal job callback must not be empty");
  }

  // 回调代表 ProcessMonitor 与 daemon 的协作关系。
  // 运行中替换它，会让不同任务的终态通知去向不一致，
  // 因此只允许在尚未启动任何任务时设置。
  if (!active_jobs_.empty()) {
    throw std::logic_error("terminal job callback must be set before starting jobs");
  }

  // 重复设置通常意味着 daemon 的初始化顺序有问题。
  if (terminal_job_callback_) {
    throw std::logic_error("terminal job callback is already set");
  }

  terminal_job_callback_ = std::move(callback);
}

void ProcessMonitor::failQueuedJob(Job& job, const std::string& message) {
  transitionJob(job, JobState::kFailed);
  job.failure_message = message;

  std::cerr << "job " << job.id << " failed before startup: " << message << '\n';
}

void ProcessMonitor::registerProcessFd(int fd, JobId job_id, ProcessFdKind kind) {
  const bool inserted = tracked_fds_.emplace(fd, TrackedFd{job_id, kind}).second;

  if (!inserted) {
    throw std::logic_error("process fd is already registered");
  }

  epoll_event event{};
  event.events = EPOLLIN;
  event.data.fd = fd;

  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
    const int error_number = errno;
    tracked_fds_.erase(fd);

    throw std::system_error(error_number, std::generic_category(), "add process pipe to epoll");
  }
}

void ProcessMonitor::abandonStartedJob(JobId job_id, const std::string& message) {
  auto active_it = active_jobs_.find(job_id);

  if (active_it == active_jobs_.end()) {
    return;
  }

  ActiveProcess& process = active_it->second;

  const std::array<int, 3> fds{
      process.stdout_fd,
      process.stderr_fd,
      process.startup_error_fd,
  };

  for (const int fd : fds) {
    if (fd == -1) {
      continue;
    }

    static_cast<void>(::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr));
    tracked_fds_.erase(fd);
    static_cast<void>(::close(fd));
  }

  killAndReapProcess(process.pid, process.process_group_id);

  pid_to_job_.erase(process.pid);
  active_jobs_.erase(active_it);

  Job& job = jobs_.at(job_id);
  transitionJob(job, JobState::kFailed);
  job.failure_message = message;

  std::cerr << "job " << job_id << " failed during registration: " << message << '\n';
}

void ProcessMonitor::startJob(JobId job_id) {
  Job& job = jobs_.at(job_id);

  if (job.state != JobState::kQueued) {
    throw std::logic_error("only QUEUED jobs can be started");
  }

  try {
    LaunchedProcessGuard launched_guard(launchProcess(job.spec));
    LaunchedProcess& launched = launched_guard.get();

    const bool inserted =
        active_jobs_
            .emplace(job_id,
                     ActiveProcess(launched.pid, launched.process_group_id, launched.stdout_fd,
                                   launched.stderr_fd, launched.startup_error_fd))
            .second;

    if (!inserted) {
      throw std::logic_error("job already has an active process");
    }

    // 从这里开始，active_jobs_ 负责管理子进程和三个 pipe fd。
    launched_guard.release();
  } catch (const std::exception& exception) {
    failQueuedJob(job, std::string("launchProcess failed: ") + exception.what());
    return;
  }

  ActiveProcess& process = active_jobs_.at(job_id);
  job.pid = process.pid;
  job.process_group_id = process.process_group_id;

  try {
    const bool pid_inserted = pid_to_job_.emplace(process.pid, job_id).second;

    if (!pid_inserted) {
      throw std::logic_error("child pid is already registered");
    }

    registerProcessFd(process.stdout_fd, job_id, ProcessFdKind::kStdout);
    registerProcessFd(process.stderr_fd, job_id, ProcessFdKind::kStderr);
    registerProcessFd(process.startup_error_fd, job_id, ProcessFdKind::kStartupError);
    transitionJob(job, JobState::kRunning);
  } catch (const std::exception& exception) {
    abandonStartedJob(job_id, std::string("failed to monitor child process: ") + exception.what());
    return;
  }

  std::cout << "started job " << job_id << " with pid " << process.pid << '\n';
}

bool ProcessMonitor::finishExitedChildBeforeSignal(JobId job_id) {
  auto active_it = active_jobs_.find(job_id);

  if (active_it == active_jobs_.end()) {
    throw std::logic_error("job has no active process before signal");
  }

  ActiveProcess& process = active_it->second;

  // 子进程可能已经退出并成为僵尸进程，
  // 只是 signalfd 事件尚未被主循环处理。
  //
  // 此时只检查 kill() 是否成功并不可靠：
  // 僵尸进程的 PID 仍然存在，kill() 的行为不能替代 waitpid()。
  //
  // 所以在发送 SIGTERM 或 SIGKILL 前，先主动检查一次直接子进程。
  if (!process.child_exited) {
    for (;;) {
      int wait_status = 0;

      const pid_t wait_result = ::waitpid(process.pid, &wait_status, WNOHANG);

      if (wait_result == process.pid) {
        // 直接子进程已经退出。
        // 保存 waitpid 状态，后面由 tryFinalizeJob() 统一解释。
        process.child_exited = true;
        process.wait_status = wait_status;
        break;
      }

      if (wait_result == 0) {
        // 子进程仍在运行，可以继续发送信号。
        break;
      }

      const int error_number = errno;

      if (error_number == EINTR) {
        // waitpid 被信号中断，重新调用。
        continue;
      }

      if (error_number == ECHILD) {
        // 该 PID 应该始终由 ProcessMonitor 回收。
        // 出现 ECHILD 表示内部回收状态不一致。
        throw std::logic_error("active child was reaped without being recorded");
      }

      throw std::system_error(error_number, std::generic_category(),
                              "check child before sending signal");
    }
  }

  if (!process.child_exited) {
    // 子进程仍在运行，还不能最终结算。
    return false;
  }

  // 子进程已经退出，但 stdout/stderr/startup-error pipe
  // 中可能仍然有最后一批数据。
  //
  // drainProcessFd() 可能在读到 EOF 后关闭 fd，
  // 因此先复制三个 fd，避免后续访问被修改的字段。
  const std::array<int, 3> process_fds{
      process.stdout_fd,
      process.stderr_fd,
      process.startup_error_fd,
  };

  for (const int fd : process_fds) {
    if (fd != -1) {
      drainProcessFd(fd);
    }
  }

  // 只有 child_exited 和三条 pipe EOF 全部成立，
  // tryFinalizeJob() 才会真正修改为终态。
  tryFinalizeJob(job_id);

  const Job& job = jobs_.at(job_id);

  if (isTerminal(job.state)) {
    // 任务已经按照真实退出结果完成。
    // 调用者不应该再发送 SIGTERM 或 SIGKILL。
    return true;
  }

  // 直接子进程虽然已经退出，但可能仍有后代进程持有 pipe。
  //
  // 例如：
  //   /bin/sh -c "sleep 100 & exit 0"
  //
  // 此时直接 shell 已退出，但 sleep 仍属于原进程组并持有 pipe。
  // 任务还不能最终结算，仍需要向整个进程组发送信号。
  return false;
}

bool ProcessMonitor::sendSignalToProcessGroup(JobId job_id, int signal_number,
                                              const char* operation) {
  const auto active_it = active_jobs_.find(job_id);

  if (active_it == active_jobs_.end()) {
    throw std::logic_error("job has no active process for signal");
  }

  const ActiveProcess& process = active_it->second;

  if (process.process_group_id <= 0) {
    throw std::logic_error("job has an invalid process group id");
  }

  // 负的进程组 ID 表示：
  //
  //   向 process_group_id 对应进程组中的所有进程发送信号。
  //
  // 不能只执行：
  //   kill(process.pid, signal_number)
  //
  // 否则 shell 创建的子进程和孙进程可能继续运行。
  for (;;) {
    if (::kill(-process.process_group_id, signal_number) == 0) {
      return true;
    }

    const int error_number = errno;

    if (error_number == EINTR) {
      continue;
    }

    if (error_number == ESRCH) {
      // 进程组可能刚好在 waitpid 检查与 kill 之间消失。
      //
      // 这里不擅自修改 JobState。
      // 后续 SIGCHLD 和 pipe EOF 仍然负责真实结算。
      return false;
    }

    throw std::system_error(error_number, std::generic_category(), operation);
  }
}

bool ProcessMonitor::requestTerminate(JobId job_id, TerminationCause cause) {
  Job& job = jobs_.at(job_id);

  if (job.state != JobState::kRunning) {
    // 状态检查保证同一个任务不会重复发送 SIGTERM。
    //
    // 第一次请求成功后，任务立即进入 TERMINATING，
    // 后续取消或超时请求就不能再次进入这里。
    throw std::logic_error("only RUNNING jobs can be terminated");
  }

  if (finishExitedChildBeforeSignal(job_id)) {
    // 子进程在终止请求前已经自然结束，
    // 任务已经按照真实 waitpid 结果结算。
    return false;
  }

  if (!sendSignalToProcessGroup(job_id, SIGTERM, "send SIGTERM to job process group")) {
    // 进程组在检查和发送信号之间消失。
    // 不修改状态，等待真实退出事件。
    return false;
  }

  // 只有 SIGTERM 确实发送成功后，才修改 Job 状态。
  //
  // 这样如果 kill() 失败，Job 不会错误地停留在 TERMINATING。
  transitionJob(job, JobState::kTerminating);

  job.termination_cause = cause;

  std::cout << "requested graceful termination of job " << job_id << '\n';

  return true;
}

bool ProcessMonitor::forceKill(JobId job_id) {
  Job& job = jobs_.at(job_id);

  if (job.state != JobState::kTerminating) {
    // 只有已经成功发送过 SIGTERM 的任务才允许升级为 SIGKILL。
    //
    // 这个状态检查阻止：
    // - 对 RUNNING 任务跳过 SIGTERM 直接强杀；
    // - 对已经进入终态的任务发送 SIGKILL。
    //
    // 正常事件循环通过 TimeoutManager 的 generation 和单次期限机制
    // 保证同一个强杀期限只处理一次；这里的状态检查本身并不能阻止
    // 外部代码对仍处于 TERMINATING 的任务重复调用 forceKill()。
    throw std::logic_error("only TERMINATING jobs can be force killed");
  }

  if (!job.termination_cause.has_value()) {
    throw std::logic_error("terminating job has no termination cause");
  }

  if (finishExitedChildBeforeSignal(job_id)) {
    // 任务已经在宽限期内退出并完成结算。
    // 不再发送 SIGKILL。
    return false;
  }

  if (!sendSignalToProcessGroup(job_id, SIGKILL, "send SIGKILL to job process group")) {
    // 进程组已经消失。
    // 等待 SIGCHLD 和 pipe EOF 完成最终结算。
    return false;
  }

  // forceKill 不修改 JobState 和 termination_cause。
  //
  // 状态仍然是 TERMINATING，
  // 原因仍然是第一次成功进入 TERMINATING 时记录的原因。
  //
  // 后续 waitpid 可能记录：
  //   exit_signal = SIGKILL
  //
  // 但最终业务状态仍然是：
  //   kCancelled -> CANCELLED
  //   kTimedOut  -> TIMED_OUT
  std::cout << "sent SIGKILL to process group of job " << job_id << '\n';

  return true;
}

void ProcessMonitor::closeTrackedFd(int fd) {
  const auto tracked_it = tracked_fds_.find(fd);

  if (tracked_it == tracked_fds_.end()) {
    return;
  }

  const TrackedFd tracked = tracked_it->second;
  const auto active_it = active_jobs_.find(tracked.job_id);

  if (active_it != active_jobs_.end()) {
    ActiveProcess& process = active_it->second;

    switch (tracked.kind) {
      case ProcessFdKind::kStdout:
        process.stdout_fd = -1;
        process.stdout_eof = true;
        break;
      case ProcessFdKind::kStderr:
        process.stderr_fd = -1;
        process.stderr_eof = true;
        break;
      case ProcessFdKind::kStartupError:
        process.startup_error_fd = -1;
        process.startup_error_eof = true;
        break;
    }
  }

  static_cast<void>(::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr));
  static_cast<void>(::close(fd));
  tracked_fds_.erase(tracked_it);
}

void ProcessMonitor::drainProcessFd(int fd) {
  const auto tracked_it = tracked_fds_.find(fd);

  if (tracked_it == tracked_fds_.end()) {
    return;
  }

  const TrackedFd tracked = tracked_it->second;
  ActiveProcess& process = active_jobs_.at(tracked.job_id);

  char buffer[4096];

  for (;;) {
    const ssize_t size = ::read(fd, buffer, sizeof(buffer));

    if (size > 0) {
      const std::size_t byte_count = static_cast<std::size_t>(size);

      switch (tracked.kind) {
        case ProcessFdKind::kStdout:
          process.standard_output.append(buffer, byte_count);
          break;
        case ProcessFdKind::kStderr:
          process.standard_error.append(buffer, byte_count);
          break;
        case ProcessFdKind::kStartupError:
          process.startup_error_bytes.append(buffer, byte_count);
          break;
      }

      continue;
    }

    if (size == 0) {
      closeTrackedFd(fd);
      return;
    }

    const int error_number = errno;

    if (error_number == EINTR) {
      continue;
    }

    if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
      return;
    }

    throw std::system_error(error_number, std::generic_category(), "read child process pipe");
  }
}

void ProcessMonitor::reapExitedChildren() {
  for (;;) {
    int wait_status = 0;
    const pid_t pid = ::waitpid(-1, &wait_status, WNOHANG);

    if (pid > 0) {
      const auto pid_it = pid_to_job_.find(pid);

      if (pid_it == pid_to_job_.end()) {
        continue;
      }

      const JobId job_id = pid_it->second;
      ActiveProcess& process = active_jobs_.at(job_id);

      process.child_exited = true;
      process.wait_status = wait_status;

      tryFinalizeJob(job_id);
      continue;
    }

    if (pid == 0) {
      return;
    }

    const int error_number = errno;

    if (error_number == EINTR) {
      continue;
    }

    if (error_number == ECHILD) {
      return;
    }

    throw std::system_error(error_number, std::generic_category(), "waitpid");
  }
}

void ProcessMonitor::handleSigchld() {
  std::array<signalfd_siginfo, 8> signals{};

  for (;;) {
    const ssize_t size = ::read(sigchld_fd_, signals.data(), sizeof(signals));

    if (size > 0) {
      if (size % static_cast<ssize_t>(sizeof(signalfd_siginfo)) != 0) {
        throw std::runtime_error("signalfd returned a partial record");
      }

      continue;
    }

    if (size == 0) {
      throw std::runtime_error("unexpected EOF from signalfd");
    }

    const int error_number = errno;

    if (size == -1 && error_number == EINTR) {
      continue;
    }

    if (size == -1 && (error_number == EAGAIN || error_number == EWOULDBLOCK)) {
      break;
    }

    throw std::system_error(error_number, std::generic_category(), "read signalfd");
  }

  // SIGCHLD 可能合并，因此必须循环 waitpid，而不是每次信号只回收一个。
  reapExitedChildren();
}

void ProcessMonitor::tryFinalizeJob(JobId job_id) {
  const auto active_it = active_jobs_.find(job_id);

  if (active_it == active_jobs_.end()) {
    return;
  }

  ActiveProcess& process = active_it->second;

  if (!process.child_exited || !process.stdout_eof || !process.stderr_eof ||
      !process.startup_error_eof) {
    return;
  }

  Job& job = jobs_.at(job_id);

  job.standard_output.swap(process.standard_output);
  job.standard_error.swap(process.standard_error);

  // TERMINATING 状态拥有更高优先级。
  if (job.state == JobState::kTerminating) {
    if (!job.termination_cause.has_value()) {
      throw std::logic_error("terminating job has no termination cause");
    }

    // 仍然保留真实 waitpid 结果。
    if (WIFEXITED(process.wait_status)) {
      job.exit_code = WEXITSTATUS(process.wait_status);
    } else if (WIFSIGNALED(process.wait_status)) {
      job.exit_signal = WTERMSIG(process.wait_status);
    } else {
      throw std::logic_error("terminating job has unexpected waitpid status");
    }

    switch (*job.termination_cause) {
      case TerminationCause::kCancelled:
        transitionJob(job, JobState::kCancelled);
        break;

      case TerminationCause::kTimedOut:
        // TimeoutManager 在执行期限到达时调用
        // requestTerminate(job_id, kTimedOut)。
        //
        // 此处已经确认子进程退出且所有输出管道 EOF，
        // 因此可以完成 TERMINATING -> TIMED_OUT。
        transitionJob(job, JobState::kTimedOut);
        break;
    }
  } else if (process.startup_error_bytes.empty()) {
    // 没有收到 execve / 子进程初始化错误，
    // 按普通任务的退出码或信号结算。
    if (WIFEXITED(process.wait_status)) {
      job.exit_code = WEXITSTATUS(process.wait_status);

      if (*job.exit_code == 0) {
        transitionJob(job, JobState::kSucceeded);
      } else {
        transitionJob(job, JobState::kFailed);

        job.failure_message = "process exited with code " + std::to_string(*job.exit_code);
      }
    } else if (WIFSIGNALED(process.wait_status)) {
      job.exit_signal = WTERMSIG(process.wait_status);

      transitionJob(job, JobState::kFailed);

      job.failure_message = "process terminated by signal " + std::to_string(*job.exit_signal);
    } else {
      transitionJob(job, JobState::kFailed);

      job.failure_message = "unexpected waitpid status";
    }
  } else if (process.startup_error_bytes.size() == sizeof(ChildStartupError)) {
    ChildStartupError startup_error{};

    std::memcpy(&startup_error, process.startup_error_bytes.data(), sizeof(startup_error));

    transitionJob(job, JobState::kFailed);

    job.failure_message = std::string("child startup failed at ") +
                          startupStageName(startup_error.stage) + ": " +
                          std::strerror(startup_error.error_number);
  } else {
    transitionJob(job, JobState::kFailed);
    job.failure_message = "malformed child startup error record";
  }

  std::cout << "job " << job.id << " finished: " << jobStateName(job.state)
            << ", stdout=" << job.standard_output.size()
            << " bytes, stderr=" << job.standard_error.size() << " bytes\n"
            << std::flush;

  const pid_t child_pid = process.pid;
  const JobId finalized_job_id = job.id;

  // 到这里，Job 已经完成：
  //
  // - child_exited 为 true；
  // - stdout / stderr / startup-error 都已 EOF；
  // - Job 已进入某个终态；
  // - 输出已经从 ActiveProcess 转移到 Job。
  //

  pid_to_job_.erase(child_pid);
  active_jobs_.erase(active_it);

  if (terminal_job_callback_) {
    terminal_job_callback_(finalized_job_id);
  }
}

void ProcessMonitor::handleFileDescriptorEvent(int fd, std::uint32_t event_mask) {
  if (fd == sigchld_fd_) {
    handleSigchld();
    return;
  }

  if ((event_mask & (EPOLLIN | EPOLLHUP | EPOLLERR)) == 0) {
    return;
  }

  const auto tracked_it = tracked_fds_.find(fd);

  if (tracked_it == tracked_fds_.end()) {
    return;
  }

  const JobId job_id = tracked_it->second.job_id;

  // 即使收到 EPOLLHUP，也必须先 read 到 EOF；
  // 否则可能丢掉管道里的最后一段输出。
  drainProcessFd(fd);
  tryFinalizeJob(job_id);
}

}  // namespace runnerd

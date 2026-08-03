#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runnerd/fd_utils.h"
#include "runnerd/job.h"
#include "runnerd/job_scheduler.h"
#include "runnerd/process_monitor.h"
#include "runnerd/protocol.h"
#include "runnerd/unix_socket.h"

namespace {

constexpr const char* kDefaultSocketPath = "/tmp/runnerd.sock";

// daemon 自己的启动参数。
// 以后若加入 --data-dir，也继续放在这里，而不是散落在 main() 中。
struct DaemonOptions {
  std::string socket_path = kDefaultSocketPath;

  // 默认一次只允许一个任务占用运行槽位。
  std::size_t max_running = 1;
};

void printUsage(const char* program_name) {
  std::cerr << "Usage: " << program_name
            << " [--socket <path>] [--max-running <positive-integer>]\n";
}

// 解析正整数，并且在乘 10 前检查 size_t 溢出。
bool parsePositiveMaxRunning(const std::string& text, std::size_t& result, std::string& error) {
  if (text.empty()) {
    error = "--max-running requires a positive integer";
    return false;
  }

  std::size_t value = 0;
  const std::size_t maximum = std::numeric_limits<std::size_t>::max();

  for (const char character : text) {
    if (character < '0' || character > '9') {
      error = "--max-running must be a positive integer";
      return false;
    }

    const std::size_t digit = static_cast<std::size_t>(character - '0');

    // 在 value * 10 + digit 之前检查：
    if (value > (maximum - digit) / 10U) {
      error = "--max-running is too large";
      return false;
    }

    value = value * 10U + digit;
  }

  if (value == 0) {
    error = "--max-running must be greater than zero";
    return false;
  }

  result = value;
  return true;
}

// 支持参数任意顺序。同一个选项出现两次时拒绝。
bool parseCommandLine(int argc, char* argv[], DaemonOptions& options, std::string& error) {
  options = DaemonOptions{};

  bool socket_seen = false;
  bool max_running_seen = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];

    if (argument == "--socket") {
      if (socket_seen) {
        error = "--socket may only be specified once";
        return false;
      }

      if (index + 1 >= argc || argv[index + 1][0] == '\0') {
        error = "--socket requires a non-empty path";
        return false;
      }

      options.socket_path = argv[index + 1];
      socket_seen = true;
      ++index;
      continue;
    }

    if (argument == "--max-running") {
      if (max_running_seen) {
        error = "--max-running may only be specified once";
        return false;
      }

      if (index + 1 >= argc) {
        error = "--max-running requires a value";
        return false;
      }

      if (!parsePositiveMaxRunning(argv[index + 1], options.max_running, error)) {
        return false;
      }

      max_running_seen = true;
      ++index;
      continue;
    }

    error = "unknown argument: " + argument;
    return false;
  }

  return true;
}

// 连接状态
struct Connection {
  // 每个连接独立保存解码器，使不完整帧可以跨多次 epoll 事件继续接收。
  runnerd::FrameDecoder decoder;

  // 尚未发送完的响应，以及下一次 write 应该开始的位置。
  std::vector<char> write_buffer;
  std::size_t write_offset = 0;

  // read 返回 0 后，对端不会再发送数据，但仍可能在等待服务端响应。
  bool read_closed = false;
};

using Connections = std::unordered_map<int, Connection>;

// 任务属于 daemon，而不是某一条客户端连接。
using Jobs = runnerd::JobTable;

// 尽可能多地启动等待任务，直到：
// 1. 等待队列为空；或
// 2. 所有运行槽位都已被预留。
//
// 注意：这个函数只负责把 Scheduler 和 ProcessMonitor 串起来。
// JobScheduler 不知道进程，ProcessMonitor 也不知道 FIFO 策略。
void pumpScheduler(Jobs& jobs, runnerd::JobScheduler& scheduler,
                   runnerd::ProcessMonitor& process_monitor) {
  for (;;) {
    // takeNextJobToStart() 成功时，Scheduler 已经为该任务预留了一个槽位。
    const std::optional<runnerd::JobId> next_job_id = scheduler.takeNextJobToStart();

    if (!next_job_id.has_value()) {
      // 没有等待任务，或者没有空闲槽位。
      return;
    }

    const runnerd::JobId job_id = *next_job_id;

    // startJob() 的正常结果有两类：
    //
    // 1. 启动成功：
    //    Job 从 QUEUED 变为 RUNNING，槽位继续被 Scheduler 保留。
    //
    // 2. 同步启动失败：
    //    例如 fork 失败、监控 fd 注册失败。
    //    ProcessMonitor 会把 Job 变为 FAILED。
    process_monitor.startJob(job_id);

    const runnerd::Job& job = jobs.at(job_id);

    if (runnerd::isTerminal(job.state)) {
      // 此任务在 startJob() 内同步失败，已经不会再收到后续的
      // SIGCHLD / pipe / EOF 事件，因此必须立刻归还刚才预留的槽位。
      scheduler.onJobReachedTerminalState(job_id);

      // 归还槽位后，可能可以立刻启动队列中的下一个任务。
      continue;
    }

    if (job.state != runnerd::JobState::kRunning) {
      // 这是内部不变量检查：
      // 调度器取出的 QUEUED Job，调用 startJob 后，要么 FAILED，
      // 要么 RUNNING；出现第三种状态说明模块之间的约定被破坏。
      throw std::logic_error("started job is neither RUNNING nor terminal");
    }
  }
}

bool hasPendingWrite(const Connection& connection) {
  return connection.write_offset < connection.write_buffer.size();
}

bool handleClientWrite(int client_fd, Connection& connection) {
  while (hasPendingWrite(connection)) {
    const char* data = connection.write_buffer.data() + connection.write_offset;

    const std::size_t remaining = connection.write_buffer.size() - connection.write_offset;

    const ssize_t n = ::write(client_fd, data, remaining);

    if (n > 0) {
      // 非阻塞 write 可能只写入一部分，记录偏移后继续发送剩余数据。
      connection.write_offset += static_cast<std::size_t>(n);
      continue;
    }

    if (n == -1 && errno == EINTR) {
      continue;
    }

    if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // 当前暂时不可写，保留缓冲区和偏移，等待下一次 EPOLLOUT。
      return true;
    }

    return false;
  }

  connection.write_buffer.clear();
  connection.write_offset = 0;
  return true;
}

bool updateClientEvents(int epoll_fd, int client_fd, const Connection& connection) {
  epoll_event event{};

  // 对端发送方向尚未关闭时，继续关注新数据和半关闭事件。
  if (!connection.read_closed) {
    event.events |= EPOLLIN | EPOLLRDHUP;
  }

  // 只有存在待写数据时才关注 EPOLLOUT，避免 socket 一直可写造成忙循环。
  if (hasPendingWrite(connection)) {
    event.events |= EPOLLOUT;
  }

  event.data.fd = client_fd;

  return ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &event) == 0;
}

void closeClient(int epoll_fd, int client_fd, Connections& connections) {
  static_cast<void>(::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr));

  ::close(client_fd);
  connections.erase(client_fd);
}

void acceptClients(int listen_fd, int epoll_fd, Connections& connections) {
  for (;;) {
    const int client_fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);

    if (client_fd == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }

      if (errno == EINTR) {
        continue;
      }

      throw std::runtime_error("accept4 failed");
    }

    try {
      runnerd::setNonBlocking(client_fd);

      const bool inserted = connections.emplace(client_fd, Connection{}).second;

      if (!inserted) {
        throw std::runtime_error("client fd already exists");
      }

      epoll_event event{};
      event.events = EPOLLIN | EPOLLRDHUP;
      event.data.fd = client_fd;

      if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {
        throw std::runtime_error("add client to epoll failed");
      }
    } catch (...) {
      connections.erase(client_fd);
      ::close(client_fd);
      throw;
    }
  }
}

// STATUS 返回单个任务的详细摘要，但暂时不返回 stdout/stderr 内容。
std::string formatJobStatus(const runnerd::Job& job) {
  std::string result =
      "id=" + std::to_string(job.id) + " state=" + std::string(runnerd::jobStateName(job.state));

  if (job.spec.execution_timeout.has_value()) {
    result += " timeout_ms=" + std::to_string(job.spec.execution_timeout->count());
  }

  if (job.pid.has_value()) {
    result += " pid=" + std::to_string(*job.pid);
  }

  if (job.exit_code.has_value()) {
    result += " exit_code=" + std::to_string(*job.exit_code);
  }

  if (job.exit_signal.has_value()) {
    result += " exit_signal=" + std::to_string(*job.exit_signal);
  }

  // ProcessMonitor 在任务最终结算时才把活动进程缓冲区移动到 Job，
  // 因此只有终态任务的输出大小是完整、可信的。
  if (runnerd::isTerminal(job.state)) {
    result += " stdout_bytes=" + std::to_string(job.standard_output.size());
    result += " stderr_bytes=" + std::to_string(job.standard_error.size());
  }

  if (!job.failure_message.empty()) {
    result += " error=" + job.failure_message;
  }

  return result;
}

std::string makeStatusResponse(const runnerd::Job& job) {
  const std::string response = "OK " + formatJobStatus(job);

  if (response.size() > runnerd::kMaxFrameSize) {
    return "ERR status response exceeds maximum frame size";
  }

  return response;
}

std::string makeListResponse(const Jobs& jobs) {
  // unordered_map 的遍历顺序不稳定，先对 JobId 排序以获得稳定输出。
  std::vector<runnerd::JobId> job_ids;
  job_ids.reserve(jobs.size());

  for (const auto& entry : jobs) {
    job_ids.push_back(entry.first);
  }

  std::sort(job_ids.begin(), job_ids.end());

  std::string response = "OK";

  for (const runnerd::JobId job_id : job_ids) {
    const runnerd::Job& job = jobs.at(job_id);
    const std::string line = "\nid=" + std::to_string(job.id) +
                             " state=" + std::string(runnerd::jobStateName(job.state));

    // LIST 尚未设计分页。超过单帧上限时返回明确错误，不能返回残缺列表。
    if (line.size() > runnerd::kMaxFrameSize - response.size()) {
      return "ERR too many jobs to list";
    }

    response += line;
  }

  return response;
}

std::string handleRequest(const std::string& request, Jobs& jobs, runnerd::JobId& next_job_id,
                          runnerd::JobScheduler& scheduler,
                          runnerd::ProcessMonitor& process_monitor) {
  if (request == "PING") {
    std::cout << "received PING\n";
    return "PONG";
  }

  if (request == "LIST") {
    return makeListResponse(jobs);
  }

  if (runnerd::isStatusRequest(request)) {
    try {
      const runnerd::JobId job_id = runnerd::decodeStatusRequest(request);
      const auto iterator = jobs.find(job_id);

      if (iterator == jobs.end()) {
        return "ERR job not found";
      }

      return makeStatusResponse(iterator->second);
    } catch (const std::invalid_argument& exception) {
      return std::string("ERR ") + exception.what();
    }
  }

  if (!runnerd::isSubmitRequest(request)) {
    std::cerr << "received unknown request\n";

    return "ERR unknown request";
  }

  try {
    runnerd::JobSpec spec = runnerd::decodeSubmitRequest(request);

    // 客户端已经校验过也不能信任。
    runnerd::validateJobSpec(spec);

    // JobId 从 1 开始。
    // uint64_t 溢出后会回到 0。
    if (next_job_id == 0) {
      throw std::overflow_error("job id space exhausted");
    }

    const runnerd::JobId job_id = next_job_id;

    // 先准备响应，避免任务已经插入后，
    // 构造响应字符串才发生异常。
    const std::string response = "OK " + std::to_string(job_id);

    runnerd::Job job;
    job.id = job_id;
    job.spec = std::move(spec);

    // state 不需要手动设置，
    // Job 默认状态就是 kQueued。

    const bool inserted = jobs.emplace(job_id, std::move(job)).second;

    if (!inserted) {
      throw std::logic_error("duplicate job id");
    }

    try {
      scheduler.enqueue(job_id);
    } catch (...) {
      jobs.erase(job_id);
      throw;
    }

    // Job 同时成功进入 JobTable 和 Scheduler 后，
    // 才真正消耗这个 JobId。
    ++next_job_id;

    // 根据当前空闲槽位，尽可能启动 FIFO 队列中的任务。
    pumpScheduler(jobs, scheduler, process_monitor);

    std::cout << "accepted job " << job_id << '\n';

    return response;
  } catch (const std::invalid_argument& exception) {
    // 请求结构或 JobSpec 不合法：
    // 返回 ERR，但不终止 daemon。
    std::cerr << "rejected SUBMIT: " << exception.what() << '\n';

    return std::string("ERR ") + exception.what();
  }
}

// 返回值：
// true：本轮读取正常结束，继续保留连接。
// false：发生无法恢复的读取错误，需要清理连接。
bool handleClientRead(int client_fd, Connection& connection, Jobs& jobs,
                      runnerd::JobId& next_job_id, runnerd::JobScheduler& scheduler,
                      runnerd::ProcessMonitor& process_monitor) {
  char buffer[4096];
  for (;;) {
    const ssize_t n = ::read(client_fd, buffer, sizeof(buffer));

    if (n > 0) {
      connection.decoder.feed(buffer, static_cast<std::size_t>(n));
      // 一次 read 可能包含多个完整帧，因此要把当前所有完整请求依次取出。
      // 末尾若还有半帧，会继续留在该连接的 decoder 中等待下次 EPOLLIN。
      while (connection.decoder.hasFrame()) {
        const std::string request = connection.decoder.popFrame();

        const std::string response_payload =
            handleRequest(request, jobs, next_job_id, scheduler, process_monitor);

        const std::vector<char> response = runnerd::encodeFrame(response_payload);

        connection.write_buffer.insert(connection.write_buffer.end(), response.begin(),
                                       response.end());
      }
      continue;
    }

    if (n == 0) {
      // 这里只表示对端不会继续发送数据，不能立即关闭连接。
      // write_buffer 中可能还有已经生成但尚未发送的响应。
      connection.read_closed = true;
      return true;
    }

    if (errno == EINTR) {
      continue;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }

    return false;
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  DaemonOptions options;
  std::string command_line_error;

  if (!parseCommandLine(argc, argv, options, command_line_error)) {
    std::cerr << "runnerd error: " << command_line_error << '\n';
    printUsage(argv[0]);
    return 1;
  }

  const std::string& socket_path = options.socket_path;

  // 客户端提前断开连接时，
  // write 可能触发 SIGPIPE。
  // 忽略 SIGPIPE，让 write 返回错误即可。
  std::signal(SIGPIPE, SIG_IGN);

  int listen_fd = -1;
  int epoll_fd = -1;
  bool owns_socket_path = false;

  try {
    listen_fd = runnerd::createUnixListener(socket_path);
    owns_socket_path = true;
    runnerd::setNonBlocking(listen_fd);

    std::cout << "runnerd is listening on " << socket_path << '\n';

    epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
      throw std::runtime_error("epoll_create1 failed");
    }

    Connections connections;

    // 任务表与 daemon 生命周期相同。
    // 客户端断开时不能删除任务。
    Jobs jobs;

    // ProcessMonitor 必须在启动任何 Job 前构造，
    // 因为构造函数会屏蔽 SIGCHLD 并创建 signalfd。
    runnerd::ProcessMonitor process_monitor(epoll_fd, jobs);

    // Scheduler 不持有 JobTable，也不持有 ProcessMonitor。
    // daemon 只是把两个独立模块放在同一个事件循环中协作。
    runnerd::JobScheduler scheduler(options.max_running);

    runnerd::JobId next_job_id = 1;

    std::cout << "maximum concurrent jobs: " << scheduler.maxRunning() << '\n';

    epoll_event listen_event{};
    listen_event.events = EPOLLIN;
    listen_event.data.fd = listen_fd;

    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_event) == -1) {
      throw std::runtime_error("add listener to epoll failed");
    }

    for (;;) {
      epoll_event events[64];
      const int count = ::epoll_wait(epoll_fd, events, 64, -1);

      if (count == -1) {
        if (errno == EINTR) {
          continue;
        }

        throw std::runtime_error("epoll_wait failed");
      }

      for (int i = 0; i < count; ++i) {
        const int fd = events[i].data.fd;
        const std::uint32_t event_mask = events[i].events;

        if (fd == listen_fd) {
          acceptClients(listen_fd, epoll_fd, connections);
          continue;
        }

        if (process_monitor.ownsFileDescriptor(fd)) {
          process_monitor.handleFileDescriptorEvent(fd, event_mask);
          continue;
        }

        // 正常情况下客户端 fd 一定存在。
        // 这里防止已经进入 events 数组的旧事件访问已删除的连接状态。
        auto it = connections.find(fd);
        if (it == connections.end()) {
          continue;
        }

        bool connection_alive = true;

        try {
          // EPOLLERR 表示 socket 出错。这个事件即使没有显式注册也会被报告，
          // 此时不再继续读写，统一走下面的连接清理流程。
          if ((event_mask & EPOLLERR) != 0) {
            connection_alive = false;
          }

          // EPOLLIN 表示有数据可读；EPOLLRDHUP 表示对端关闭了发送方向。
          // 两者可能同时出现，所以仍要调用 read 把对端最后发送的数据读完。
          if (connection_alive && (event_mask & (EPOLLIN | EPOLLRDHUP)) != 0) {
            connection_alive =
                handleClientRead(fd, it->second, jobs, next_job_id, scheduler, process_monitor);
          }

          // EPOLLOUT 表示当前可以继续写。handleClientWrite 会从 write_offset
          // 开始发送，遇到 EAGAIN 时保留现场，等待下一次 EPOLLOUT。
          if (connection_alive && (event_mask & EPOLLOUT) != 0) {
            connection_alive = handleClientWrite(fd, it->second);
          }

          // EPOLLHUP 表示连接已经完全挂断，与只关闭发送方向的
          // EPOLLRDHUP 不同，此时不再保留连接。
          if (connection_alive && (event_mask & EPOLLHUP) != 0) {
            connection_alive = false;
          }

          if (connection_alive) {
            const bool needs_write = hasPendingWrite(it->second);

            // 对端不再发送数据且响应已经全部写完，连接的工作已经完成。
            // 必须在 epoll_ctl(MOD) 之前判断，否则可能尝试注册空事件集合。
            if (it->second.read_closed && !needs_write) {
              connection_alive = false;
            } else if (!updateClientEvents(epoll_fd, fd, it->second)) {
              connection_alive = false;
            }
          }
        } catch (const std::exception& exception) {
          std::cerr << "client error: " << exception.what() << '\n';
          connection_alive = false;
        }

        if (!connection_alive) {
          closeClient(epoll_fd, fd, connections);
        }
      }
    }
  } catch (const std::exception& exception) {
    std::cerr << "runnerd error: " << exception.what() << '\n';
  }

  if (epoll_fd != -1) {
    ::close(epoll_fd);
  }

  if (listen_fd != -1) {
    ::close(listen_fd);
  }

  // 只有成功创建过自己的 socket，
  // 才允许删除这个路径。
  if (owns_socket_path) {
    ::unlink(socket_path.c_str());
  }

  return 1;
}

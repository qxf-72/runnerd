#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "runnerd/job.h"
#include "runnerd/protocol.h"
#include "runnerd/unix_socket.h"

namespace {

constexpr const char* kDefaultSocketPath = "/tmp/runnerd.sock";

enum class Command { kPing, kSubmit, kStatus, kList, kCancel };

struct CommandLine {
  std::string socket_path = kDefaultSocketPath;

  Command command = Command::kPing;

  runnerd::JobSpec job_spec;

  // status 和 cancel 命令都会使用 JobId。
  runnerd::JobId job_id = 0;
};

void printUsage(const char* program_name) {
  std::cerr << "Usage:\n"
            << "  " << program_name << " [--socket <path>] ping\n"
            << "  " << program_name << " [--socket <path>] submit "
            << "[--timeout <milliseconds>] "
            << "-- <absolute-path> [arguments...]\n"
            << "  " << program_name << " [--socket <path>] status <job_id>\n"
            << "  " << program_name << " [--socket <path>] list\n"
            << "  " << program_name << " [--socket <path>] cancel <job_id>\n";
}

// 只接受十进制正整数，并在计算过程中检查 uint32_t 溢出。
bool parsePositiveTimeout(const std::string& text, runnerd::JobTimeout& timeout,
                          std::string& error) {
  if (text.empty()) {
    error = "--timeout requires a positive integer";
    return false;
  }

  std::uint32_t value = 0;

  const std::uint32_t maximum = std::numeric_limits<std::uint32_t>::max();

  for (const char character : text) {
    if (character < '0' || character > '9') {
      error = "--timeout must be a positive integer";
      return false;
    }

    const std::uint32_t digit = static_cast<std::uint32_t>(character - '0');

    // 在执行 value * 10 + digit 前检查溢出。
    if (value > (maximum - digit) / 10U) {
      error = "--timeout is too large";
      return false;
    }

    value = value * 10U + digit;
  }

  if (value == 0) {
    error = "--timeout must be greater than zero";
    return false;
  }

  timeout = runnerd::JobTimeout(static_cast<runnerd::JobTimeout::rep>(value));

  return true;
}

bool parseJobId(const std::string& text, runnerd::JobId& job_id, std::string& error) {
  if (text.empty()) {
    error = "job id must not be empty";
    return false;
  }

  runnerd::JobId value = 0;
  const runnerd::JobId maximum = std::numeric_limits<runnerd::JobId>::max();

  for (const char character : text) {
    if (character < '0' || character > '9') {
      error = "job id must be a positive integer";
      return false;
    }

    const runnerd::JobId digit = static_cast<runnerd::JobId>(character - '0');

    if (value > (maximum - digit) / 10U) {
      error = "job id is too large";
      return false;
    }

    value = value * 10U + digit;
  }

  if (value == 0) {
    error = "job id must be greater than zero";
    return false;
  }

  job_id = value;
  return true;
}

bool parseCommandLine(int argc, char* argv[], CommandLine& command_line, std::string& error) {
  command_line = CommandLine{};

  int index = 1;

  // --socket 只允许出现在命令前。
  if (index < argc && std::string(argv[index]) == "--socket") {
    if (index + 1 >= argc || argv[index + 1][0] == '\0') {
      error = "--socket requires a non-empty path";
      return false;
    }

    command_line.socket_path = argv[index + 1];

    index += 2;
  }

  if (index >= argc) {
    error = "missing command";
    return false;
  }

  const std::string command = argv[index];

  ++index;

  if (command == "ping") {
    command_line.command = Command::kPing;

    if (index != argc) {
      error =
          "ping does not accept "
          "additional arguments";
      return false;
    }

    return true;
  }

  if (command == "list") {
    command_line.command = Command::kList;

    if (index != argc) {
      error = "list does not accept additional arguments";
      return false;
    }

    return true;
  }

  if (command == "status") {
    command_line.command = Command::kStatus;

    if (index >= argc) {
      error = "status requires a job id";
      return false;
    }

    if (!parseJobId(argv[index], command_line.job_id, error)) {
      return false;
    }

    ++index;

    if (index != argc) {
      error = "status accepts exactly one job id";
      return false;
    }

    return true;
  }

  if (command == "cancel") {
    command_line.command = Command::kCancel;

    if (index >= argc) {
      error = "cancel requires a job id";
      return false;
    }

    if (!parseJobId(argv[index], command_line.job_id, error)) {
      return false;
    }

    ++index;

    if (index != argc) {
      error = "cancel accepts exactly one job id";
      return false;
    }

    return true;
  }

  if (command != "submit") {
    error = "unknown command: " + command;
    return false;
  }

  command_line.command = Command::kSubmit;

  // timeout 必须位于 submit 和 -- 之间。
  if (index < argc && std::string(argv[index]) == "--timeout") {
    if (index + 1 >= argc) {
      error = "--timeout requires a value";
      return false;
    }

    runnerd::JobTimeout timeout{0};

    if (!parsePositiveTimeout(argv[index + 1], timeout, error)) {
      return false;
    }

    command_line.job_spec.execution_timeout = timeout;

    index += 2;
  }

  // -- 后面的内容全部属于被执行程序。
  if (index >= argc || std::string(argv[index]) != "--") {
    error =
        "submit requires -- before "
        "the executable path";
    return false;
  }

  ++index;

  if (index >= argc) {
    error =
        "submit requires an executable "
        "path after --";
    return false;
  }

  for (; index < argc; ++index) {
    command_line.job_spec.argv.emplace_back(argv[index]);
  }

  // 这里只是为了尽早向用户报告错误。
  // 服务端收到请求后还会再次校验。
  try {
    runnerd::validateJobSpec(command_line.job_spec);
  } catch (const std::invalid_argument& exception) {
    error = exception.what();
    return false;
  }

  return true;
}

std::string makeRequestPayload(const CommandLine& command_line) {
  switch (command_line.command) {
    case Command::kPing:
      return "PING";
    case Command::kSubmit:
      return runnerd::encodeSubmitRequest(command_line.job_spec);
    case Command::kStatus:
      return runnerd::encodeStatusRequest(command_line.job_id);
    case Command::kList:
      return "LIST";
    case Command::kCancel:
      return runnerd::encodeCancelRequest(command_line.job_id);
  }

  throw std::logic_error("unknown command");
}

bool writeAll(int fd, const void* buffer, std::size_t size) {
  const auto* data = static_cast<const char*>(buffer);

  std::size_t total_written = 0;

  while (total_written < size) {
    const ssize_t write_size = ::write(fd, data + total_written, size - total_written);

    if (write_size > 0) {
      total_written += static_cast<std::size_t>(write_size);
      continue;
    }

    if (write_size == -1 && errno == EINTR) {
      continue;
    }

    return false;
  }

  return true;
}

bool readFrame(int fd, runnerd::FrameDecoder& decoder, std::string& payload) {
  std::array<char, 4096> buffer{};

  for (;;) {
    if (decoder.hasFrame()) {
      payload = decoder.popFrame();
      return true;
    }

    const ssize_t read_size = ::read(fd, buffer.data(), buffer.size());

    if (read_size > 0) {
      decoder.feed(buffer.data(), static_cast<std::size_t>(read_size));
      continue;
    }

    if (read_size == 0) {
      return false;
    }

    if (errno == EINTR) {
      continue;
    }

    const int saved_errno = errno;

    throw std::system_error(saved_errno, std::generic_category(), "read response frame");
  }
}

bool isErrorResponse(const std::string& response) {
  return response == "ERR" || response.compare(0, 4, "ERR ") == 0;
}

bool parseSubmitSuccessResponse(const std::string& response, runnerd::JobId& job_id) {
  constexpr std::size_t kPrefixSize = 3;

  if (response.compare(0, kPrefixSize, "OK ") != 0 || response.size() == kPrefixSize) {
    return false;
  }

  runnerd::JobId value = 0;

  const runnerd::JobId maximum = std::numeric_limits<runnerd::JobId>::max();

  for (std::size_t index = kPrefixSize; index < response.size(); ++index) {
    const char character = response[index];

    if (character < '0' || character > '9') {
      return false;
    }

    const runnerd::JobId digit = static_cast<runnerd::JobId>(character - '0');

    if (value > (maximum - digit) / 10U) {
      return false;
    }

    value = value * 10U + digit;
  }

  // JobId 从 1 开始。
  if (value == 0) {
    return false;
  }

  job_id = value;
  return true;
}

bool handleResponse(const CommandLine& command_line, const std::string& response) {
  const Command command = command_line.command;
  if (isErrorResponse(response)) {
    if (response == "ERR") {
      std::cerr << "server error\n";
    } else {
      std::cerr << "server error: " << response.substr(4) << '\n';
    }

    return false;
  }

  switch (command) {
    case Command::kPing:
      if (response != "PONG") {
        std::cerr << "unexpected response: " << response << '\n';
        return false;
      }

      std::cout << "PONG\n";
      return true;

    case Command::kSubmit: {
      runnerd::JobId job_id = 0;

      if (!parseSubmitSuccessResponse(response, job_id)) {
        std::cerr << "unexpected response: " << response << '\n';
        return false;
      }

      // stdout 只打印 JobId，便于脚本捕获。
      std::cout << job_id << '\n';
      return true;
    }

    case Command::kStatus:
      if (response.size() <= 3 || response.compare(0, 3, "OK ") != 0) {
        std::cerr << "unexpected response: " << response << '\n';
        return false;
      }

      std::cout << response.substr(3) << '\n';
      return true;

    case Command::kList:
      if (response == "OK") {
        std::cout << "No jobs\n";
        return true;
      }

      if (response.compare(0, 3, "OK\n") != 0) {
        std::cerr << "unexpected response: " << response << '\n';
        return false;
      }

      std::cout << response.substr(3) << '\n';
      return true;

    case Command::kCancel:
      if (response == "OK cancelled") {
        // 排队任务直接进入终态。
        std::cout << "Cancelled job " << command_line.job_id << '\n';

        return true;
      }

      if (response == "OK terminating") {
        // 运行中任务只是完成了“请求终止”。
        // 最终状态仍需通过 status 查询。
        std::cout << "Cancellation requested for job " << command_line.job_id << '\n';

        return true;
      }

      std::cerr << "unexpected response: " << response << '\n';

      return false;
  }

  return false;
}

}  // namespace

int main(int argc, char* argv[]) {
  CommandLine command_line;
  std::string command_line_error;

  if (!parseCommandLine(argc, argv, command_line, command_line_error)) {
    std::cerr << "runnerctl error: " << command_line_error << '\n';

    printUsage(argv[0]);
    return 1;
  }

  std::signal(SIGPIPE, SIG_IGN);

  int socket_fd = -1;

  try {
    socket_fd = runnerd::connectUnixSocket(command_line.socket_path);

    const std::string request_payload = makeRequestPayload(command_line);

    const std::vector<char> request = runnerd::encodeFrame(request_payload);

    if (!writeAll(socket_fd, request.data(), request.size())) {
      throw std::runtime_error("failed to send request");
    }

    runnerd::FrameDecoder decoder;
    std::string response;

    if (!readFrame(socket_fd, decoder, response)) {
      throw std::runtime_error(
          "server disconnected before "
          "sending a complete response");
    }

    const bool succeeded = handleResponse(command_line, response);

    ::close(socket_fd);

    return succeeded ? 0 : 1;
  } catch (const std::exception& exception) {
    if (socket_fd != -1) {
      ::close(socket_fd);
    }

    std::cerr << "runnerctl error: " << exception.what() << '\n';

    return 1;
  }
}

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "runnerd/protocol.h"
#include "runnerd/unix_socket.h"

namespace {

// 向 fd 中写入恰好 size 个字节。
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
      // 被信号中断，重新调用 write。
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
    throw std::system_error(saved_errno, std::generic_category(), "read request frame");
  }
}

void handleClient(int client_fd) {
  runnerd::FrameDecoder decoder;
  std::string request;

  if (!readFrame(client_fd, decoder, request)) {
    std::cerr << "client disconnected before sending "
              << "a complete request\n";
    return;
  }

  std::string response_payload;

  if (request == "PING") {
    std::cout << "received PING\n";
    response_payload = "PONG";
  } else {
    std::cerr << "received unknown request\n";
    response_payload = "ERR!";
  }

  const std::vector<char> response = runnerd::encodeFrame(response_payload);

  if (!writeAll(client_fd, response.data(), response.size())) {
    std::cerr << "failed to send " << response_payload << '\n';
  }
}

}  // namespace

int main() {
  // 客户端提前断开连接时，
  // write 可能触发 SIGPIPE。
  // 忽略 SIGPIPE，让 write 返回错误即可。
  std::signal(SIGPIPE, SIG_IGN);

  const std::string socket_path = "/tmp/runnerd.sock";

  int listen_fd = -1;
  bool owns_socket_path = false;

  try {
    listen_fd = runnerd::createUnixListener(socket_path);
    owns_socket_path = true;

    std::cout << "runnerd is listening on " << socket_path << '\n';

    for (;;) {
      int client_fd = -1;

      do {
        client_fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
      } while (client_fd == -1 && errno == EINTR);

      if (client_fd == -1) {
        throw std::runtime_error("accept4 failed");
      }

      try {
        handleClient(client_fd);
      } catch (const std::exception& exception) {
        // 单个客户端的非法数据或 I/O 错误不应终止整个服务。
        std::cerr << "client error: " << exception.what() << '\n';
      }

      ::close(client_fd);
    }
  } catch (const std::exception& exception) {
    std::cerr << "runnerd error: " << exception.what() << '\n';
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

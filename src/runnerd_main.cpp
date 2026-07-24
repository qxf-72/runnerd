#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
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

void closeClient(int epoll_fd, int client_fd) {
  // close 本身也会让 fd 离开 epoll，但先显式删除更能表达这里的清理意图。
  static_cast<void>(::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr));
  ::close(client_fd);
}

void acceptClients(int listen_fd, int epoll_fd) {
  while (true) {
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

      epoll_event event{};
      event.events = EPOLLIN;
      event.data.fd = client_fd;

      if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {
        throw std::runtime_error("add client to epoll failed");
      }
    } catch (...) {
      ::close(client_fd);
      throw;
    }
  }
}

void handleClient(int client_fd, int epoll_fd) {
  char buffer[1024];

  runnerd::FrameDecoder decoder;

  while (true) {
    const ssize_t n = ::read(client_fd, buffer, sizeof(buffer));

    if (n > 0) {
      // 当前阶段先直接写响应；非阻塞写缓冲将在连接管理阶段实现。
      decoder.feed(buffer, static_cast<std::size_t>(n));

      if (!decoder.hasFrame()) {
        continue;
      }

      const std::string request = decoder.popFrame();
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
        closeClient(epoll_fd, client_fd);
        return;
      }
    } else if (n == 0) {
      // 客户端已经关闭连接。
      closeClient(epoll_fd, client_fd);
      break;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }

      if (errno == EINTR) {
        continue;
      }

      throw std::runtime_error("read client failed");
    }
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

        if (fd == listen_fd) {
          acceptClients(fd, epoll_fd);
        } else {
          try {
            handleClient(fd, epoll_fd);
          } catch (const std::exception& exception) {
            // 单个客户端的非法数据或 I/O 错误不应终止整个服务。
            std::cerr << "client error: " << exception.what() << '\n';
            closeClient(epoll_fd, fd);
          }
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

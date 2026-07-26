#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "runnerd/protocol.h"
#include "runnerd/unix_socket.h"

namespace {

constexpr const char* kDefaultSocketPath = "/tmp/runnerd.sock";

bool parseCommandLine(int argc, char* argv[], std::string& socket_path) {
  socket_path = kDefaultSocketPath;

  if (argc == 1) {
    return true;
  }

  if (argc == 3 && std::string(argv[1]) == "--socket" &&
      argv[2][0] != '\0') {
    socket_path = argv[2];
    return true;
  }

  return false;
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

// 返回值：
// true：本轮读取正常结束，继续保留连接。
// false：发生无法恢复的读取错误，需要清理连接。
bool handleClientRead(int client_fd, Connection& connection) {
  char buffer[4096];
  for (;;) {
    const ssize_t n = ::read(client_fd, buffer, sizeof(buffer));

    if (n > 0) {
      connection.decoder.feed(buffer, static_cast<std::size_t>(n));
      // 一次 read 可能包含多个完整帧，因此要把当前所有完整请求依次取出。
      // 末尾若还有半帧，会继续留在该连接的 decoder 中等待下次 EPOLLIN。
      while (connection.decoder.hasFrame()) {
        const std::string request = connection.decoder.popFrame();

        std::string response_payload;

        if (request == "PING") {
          std::cout << "received PING\n";
          response_payload = "PONG";
        } else {
          std::cerr << "received unknown request\n";
          response_payload = "ERR!";
        }

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
  std::string socket_path;

  if (!parseCommandLine(argc, argv, socket_path)) {
    std::cerr << "Usage: " << argv[0] << " [--socket <path>]\n";
    return 1;
  }

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

        // data.fd 表示“哪个 fd 就绪”，events 表示“发生了哪些事件”。
        // 同一个 fd 一次可能同时带有 EPOLLIN、EPOLLOUT、EPOLLRDHUP 等多个标志。
        const std::uint32_t event_mask = events[i].events;

        if (fd == listen_fd) {
          acceptClients(listen_fd, epoll_fd, connections);
          continue;
        }

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
          if (connection_alive &&
              (event_mask & (EPOLLIN | EPOLLRDHUP)) != 0) {
            connection_alive = handleClientRead(fd, it->second);
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

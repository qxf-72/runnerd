#include "runnerd/unix_socket.h"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <exception>
#include <iostream>
#include <string>

namespace {

constexpr std::array<char, 4> kPing{
    'P', 'I', 'N', 'G'
};

constexpr std::array<char, 4> kPong{
    'P', 'O', 'N', 'G'
};

constexpr std::array<char, 4> kError{
    'E', 'R', 'R', '!'
};

// 从 fd 中读取恰好 size 个字节。
bool readExact(int fd, void* buffer, std::size_t size)
{
    auto* data = static_cast<char*>(buffer);
    std::size_t total_read = 0;

    while (total_read < size) {
        ssize_t read_size = ::read(
            fd,
            data + total_read,
            size - total_read);

        if (read_size > 0) {
            total_read +=
                static_cast<std::size_t>(read_size);
            continue;
        }

        if (read_size == 0) {
            // 对端已经关闭连接。
            return false;
        }

        if (errno == EINTR) {
            // 被信号中断，重新调用 read。
            continue;
        }

        return false;
    }

    return true;
}

// 向 fd 中写入恰好 size 个字节。
bool writeAll(int fd, const void* buffer, std::size_t size)
{
    const auto* data =
        static_cast<const char*>(buffer);

    std::size_t total_written = 0;

    while (total_written < size) {
        ssize_t write_size = ::write(
            fd,
            data + total_written,
            size - total_written);

        if (write_size > 0) {
            total_written +=
                static_cast<std::size_t>(write_size);
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

}  // namespace

int main()
{
    // 客户端提前断开连接时，
    // write 可能触发 SIGPIPE。
    // 忽略 SIGPIPE，让 write 返回错误即可。
    std::signal(SIGPIPE, SIG_IGN);

    const std::string socket_path =
        "/tmp/runnerd.sock";

    int listen_fd = -1;
    bool owns_socket_path = false;

    try {
        listen_fd =
            runnerd::createUnixListener(socket_path);

        owns_socket_path = true;

        std::cout
            << "runnerd is listening on "
            << socket_path
            << '\n';

        for (;;) {
            int client_fd = -1;

            do {
                client_fd = ::accept4(
                    listen_fd,
                    nullptr,
                    nullptr,
                    SOCK_CLOEXEC);
            } while (client_fd == -1
                     && errno == EINTR);

            if (client_fd == -1) {
                throw std::runtime_error(
                    "accept4 failed");
            }

            std::array<char, 4> request{};

            const bool request_received =
                readExact(
                    client_fd,
                    request.data(),
                    request.size());

            if (!request_received) {
                std::cerr
                    << "client disconnected before "
                    << "sending a complete request\n";

                ::close(client_fd);
                continue;
            }

            if (request == kPing) {
                std::cout
                    << "received PING\n";

                if (!writeAll(
                        client_fd,
                        kPong.data(),
                        kPong.size())) {
                    std::cerr
                        << "failed to send PONG\n";
                }
            } else {
                std::cerr
                    << "received unknown request\n";

                if (!writeAll(
                        client_fd,
                        kError.data(),
                        kError.size())) {
                    std::cerr
                        << "failed to send ERR!\n";
                }
            }

            ::close(client_fd);
        }
    } catch (const std::exception& exception) {
        std::cerr
            << "runnerd error: "
            << exception.what()
            << '\n';
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

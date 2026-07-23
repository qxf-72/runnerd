#include "runnerd/unix_socket.h"

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
            return false;
        }

        if (errno == EINTR) {
            continue;
        }

        return false;
    }

    return true;
}

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
            continue;
        }

        return false;
    }

    return true;
}

}  // namespace

int main(int argc, char* argv[])
{
    std::signal(SIGPIPE, SIG_IGN);

    if (argc != 2
        || std::string(argv[1]) != "ping") {
        std::cerr
            << "Usage: "
            << argv[0]
            << " ping\n";

        return 1;
    }

    const std::string socket_path =
        "/tmp/runnerd.sock";

    int socket_fd = -1;

    try {
        socket_fd =
            runnerd::connectUnixSocket(socket_path);

        if (!writeAll(
                socket_fd,
                kPing.data(),
                kPing.size())) {
            std::cerr
                << "failed to send PING\n";

            ::close(socket_fd);
            return 1;
        }

        std::array<char, 4> response{};

        if (!readExact(
                socket_fd,
                response.data(),
                response.size())) {
            std::cerr
                << "failed to read response\n";

            ::close(socket_fd);
            return 1;
        }

        if (response != kPong) {
            std::cerr
                << "unexpected response\n";

            ::close(socket_fd);
            return 1;
        }

        std::cout << "PONG\n";

        ::close(socket_fd);
        return 0;
    } catch (const std::exception& exception) {
        if (socket_fd != -1) {
            ::close(socket_fd);
        }

        std::cerr
            << "runnerctl error: "
            << exception.what()
            << '\n';

        return 1;
    }
}

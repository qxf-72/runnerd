#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "runnerd/protocol.h"
#include "runnerd/unix_socket.h"

namespace {

constexpr const char* kDefaultSocketPath = "/tmp/runnerd.sock";

bool parseCommandLine(int argc, char* argv[], std::string& socket_path) {
  socket_path = kDefaultSocketPath;

  if (argc == 2 && std::string(argv[1]) == "ping") {
    return true;
  }

  if (argc == 4 && std::string(argv[1]) == "--socket" && argv[2][0] != '\0' &&
      std::string(argv[3]) == "ping") {
    socket_path = argv[2];
    return true;
  }

  return false;
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

}  // namespace

int main(int argc, char* argv[]) {
  std::string socket_path;

  if (!parseCommandLine(argc, argv, socket_path)) {
    std::cerr << "Usage: " << argv[0] << " [--socket <path>] ping\n";
    return 1;
  }

  std::signal(SIGPIPE, SIG_IGN);

  int socket_fd = -1;

  try {
    socket_fd = runnerd::connectUnixSocket(socket_path);

    const std::vector<char> request = runnerd::encodeFrame("PING");

    if (!writeAll(socket_fd, request.data(), request.size())) {
      std::cerr << "failed to send PING\n";
      ::close(socket_fd);
      return 1;
    }

    runnerd::FrameDecoder decoder;
    std::string response;

    if (!readFrame(socket_fd, decoder, response)) {
      std::cerr << "server disconnected before sending "
                << "a complete response\n";
      ::close(socket_fd);
      return 1;
    }

    if (response != "PONG") {
      std::cerr << "unexpected response: " << response << '\n';
      ::close(socket_fd);
      return 1;
    }

    std::cout << response << '\n';

    ::close(socket_fd);
    return 0;
  } catch (const std::exception& exception) {
    if (socket_fd != -1) {
      ::close(socket_fd);
    }

    std::cerr << "runnerctl error: " << exception.what() << '\n';

    return 1;
  }
}

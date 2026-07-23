#include "runnerd/unix_socket.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>

namespace runnerd {

namespace {

// 根据 errno 生成错误信息。
//
// 例如：
// bind Unix socket: Address already in use
std::runtime_error makeSystemError(const std::string& action,
                                   int error_code = errno)
{
    return std::runtime_error(
        action + ": " + std::strerror(error_code));
}

// 根据文件路径构造 sockaddr_un。
sockaddr_un makeUnixAddress(const std::string& socket_path,
                            socklen_t& address_length)
{
    if (socket_path.empty()) {
        throw std::runtime_error("Unix socket path is empty");
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;

    // sun_path 是固定长度数组，所以必须检查路径是否过长。
    if (socket_path.size() >= sizeof(address.sun_path)) {
        throw std::runtime_error(
            "Unix socket path is too long: " + socket_path);
    }

    // 连同末尾的 '\0' 一起复制。
    std::memcpy(address.sun_path,
                socket_path.c_str(),
                socket_path.size() + 1);

    // sockaddr_un 的有效长度：
    // sun_path 字段之前的长度 + 实际路径长度 + '\0'
    address_length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path)
        + socket_path.size()
        + 1);

    return address;
}

// 在 bind 之前处理已有的 socket 文件。
void removeStaleSocketIfNeeded(const std::string& socket_path)
{
    struct stat file_stat {};

    if (::lstat(socket_path.c_str(), &file_stat) == -1) {
        if (errno == ENOENT) {
            // 路径不存在，说明可以直接创建。
            return;
        }

        throw makeSystemError(
            "lstat existing Unix socket");
    }

    // 路径存在，但不是 socket 文件。
    // 不能随便删除普通文件。
    if (!S_ISSOCK(file_stat.st_mode)) {
        throw std::runtime_error(
            "Path already exists and is not a Unix socket: "
            + socket_path);
    }

    // 路径不是当前用户拥有，不能删除。
    if (file_stat.st_uid != ::geteuid()) {
        throw std::runtime_error(
            "Existing Unix socket is not owned by current user: "
            + socket_path);
    }

    // 路径是当前用户的 socket。
    // 尝试连接它，判断是否已经有 runnerd 正在运行。
    int probe_fd = ::socket(
        AF_UNIX,
        SOCK_STREAM | SOCK_CLOEXEC,
        0);

    if (probe_fd == -1) {
        throw makeSystemError(
            "create probe Unix socket");
    }

    socklen_t address_length = 0;
    sockaddr_un address =
        makeUnixAddress(socket_path, address_length);

    int connect_result = ::connect(
        probe_fd,
        reinterpret_cast<const sockaddr*>(&address),
        address_length);

    if (connect_result == 0) {
        // 可以连接，说明已经有服务端在监听。
        ::close(probe_fd);

        throw std::runtime_error(
            "runnerd is already running: " + socket_path);
    }

    const int connect_error = errno;
    ::close(probe_fd);

    if (connect_error == ECONNREFUSED
        || connect_error == ENOENT) {
        // 文件存在，但已经没有服务端监听。
        // 这是上一次异常退出遗留的旧 socket。
        if (::unlink(socket_path.c_str()) == -1
            && errno != ENOENT) {
            throw makeSystemError(
                "remove stale Unix socket");
        }

        return;
    }

    throw makeSystemError(
        "check existing Unix socket",
        connect_error);
}

}  // namespace

int createUnixListener(const std::string& socket_path)
{
    // bind 之前先检查旧 socket。
    removeStaleSocketIfNeeded(socket_path);

    socklen_t address_length = 0;
    sockaddr_un address =
        makeUnixAddress(socket_path, address_length);

    int listen_fd = ::socket(
        AF_UNIX,
        SOCK_STREAM | SOCK_CLOEXEC,
        0);

    if (listen_fd == -1) {
        throw makeSystemError(
            "create listener Unix socket");
    }

    if (::bind(
            listen_fd,
            reinterpret_cast<const sockaddr*>(&address),
            address_length)
        == -1) {
        const int saved_errno = errno;
        ::close(listen_fd);

        throw makeSystemError(
            "bind Unix socket",
            saved_errno);
    }

    // 只允许当前用户访问。
    if (::chmod(socket_path.c_str(), 0600) == -1) {
        const int saved_errno = errno;

        ::close(listen_fd);
        ::unlink(socket_path.c_str());

        throw makeSystemError(
            "chmod Unix socket",
            saved_errno);
    }

    if (::listen(listen_fd, SOMAXCONN) == -1) {
        const int saved_errno = errno;

        ::close(listen_fd);
        ::unlink(socket_path.c_str());

        throw makeSystemError(
            "listen Unix socket",
            saved_errno);
    }

    return listen_fd;
}

int connectUnixSocket(const std::string& socket_path)
{
    socklen_t address_length = 0;
    sockaddr_un address =
        makeUnixAddress(socket_path, address_length);

    int socket_fd = ::socket(
        AF_UNIX,
        SOCK_STREAM | SOCK_CLOEXEC,
        0);

    if (socket_fd == -1) {
        throw makeSystemError(
            "create client Unix socket");
    }

    if (::connect(
            socket_fd,
            reinterpret_cast<const sockaddr*>(&address),
            address_length)
        == -1) {
        const int saved_errno = errno;
        ::close(socket_fd);

        throw makeSystemError(
            "connect Unix socket",
            saved_errno);
    }

    return socket_fd;
}

}  // namespace runnerd

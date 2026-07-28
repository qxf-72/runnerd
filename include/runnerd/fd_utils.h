#ifndef RUNNERD_FD_UTILS_H
#define RUNNERD_FD_UTILS_H

namespace runnerd {

// 保留 fd 原有状态标志，并额外设置 O_NONBLOCK。
// 失败时抛出 std::system_error。
void setNonBlocking(int fd);

}  // namespace runnerd

#endif  // RUNNERD_FD_UTILS_H

#ifndef RUNNERD_UNIX_SOCKET_H
#define RUNNERD_UNIX_SOCKET_H

#include <string>

namespace runnerd {

// 创建服务端 Unix Domain Socket。
//
// 成功：返回监听文件描述符。
// 失败：抛出 std::runtime_error。
int createUnixListener(const std::string& socket_path);

// 连接服务端 Unix Domain Socket。
//
// 成功：返回已连接的文件描述符。
// 失败：抛出 std::runtime_error。
int connectUnixSocket(const std::string& socket_path);

}  // namespace runnerd


#endif

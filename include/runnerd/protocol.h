#ifndef RUNNERD_PROTOCOL_H
#define RUNNERD_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace runnerd {

constexpr std::size_t kFrameHeaderSize = sizeof(std::uint32_t);
constexpr std::size_t kMaxFrameSize = 64U * 1024U;

// 将 payload 编码为“4 字节大端长度 + payload”。
std::vector<char> encodeFrame(const std::string& payload);

// 长度前缀帧的增量解码器。
class FrameDecoder {
 public:
  // 输入新收到的数据。size 为 0 时 data 可以为空。
  void feed(const char* data, std::size_t size);

  // 是否已经得到至少一个完整帧。
  bool hasFrame() const;

  // 取出最前面的完整帧。
  std::string popFrame();

 private:
  // 读取并验证当前缓冲区最前面的长度字段。
  std::size_t payloadSize() const;

  std::vector<char> buffer_;
};

}  // namespace runnerd

#endif

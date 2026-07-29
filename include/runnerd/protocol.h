#ifndef RUNNERD_PROTOCOL_H
#define RUNNERD_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "runnerd/job.h"

namespace runnerd {

constexpr std::size_t kFrameHeaderSize = sizeof(std::uint32_t);
constexpr std::size_t kMaxFrameSize = 64U * 1024U;

// 判断 payload 是否使用 SUBMIT 请求格式。
bool isSubmitRequest(const std::string& payload);

// 将任务参数编码为 SUBMIT payload。
// 返回值不包含外层的 4 字节帧头。
std::string encodeSubmitRequest(const JobSpec& spec);

// 从 SUBMIT payload 中还原任务参数。
// 这里只检查二进制结构，不负责服务端业务校验。
JobSpec decodeSubmitRequest(const std::string& payload);

// 判断 payload 是否使用 STATUS 请求格式。
bool isStatusRequest(const std::string& payload);

// 将 JobId 编码为 STATUS payload。
std::string encodeStatusRequest(JobId job_id);

// 从 STATUS payload 中还原 JobId。
JobId decodeStatusRequest(const std::string& payload);

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

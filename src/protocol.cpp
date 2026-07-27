#include "runnerd/protocol.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace runnerd {

namespace {

constexpr char kSubmitCommand[] = "SUBMIT";
constexpr std::size_t kSubmitCommandSize = sizeof(kSubmitCommand) - 1;

// 将 uint32_t 以大端序追加到字符串。
// std::string 可以保存 '\0'，因此可以作为二进制缓冲区。
void appendUint32(std::string& output, std::uint32_t value) {
  const std::uint32_t network_value = ::htonl(value);

  output.append(reinterpret_cast<const char*>(&network_value), sizeof(network_value));
}

// 从 input[offset] 开始读取一个大端序 uint32_t。
// 成功后自动向后移动 offset。
std::uint32_t readUint32(const std::string& input, std::size_t& offset) {
  if (offset > input.size() || input.size() - offset < sizeof(std::uint32_t)) {
    throw std::invalid_argument("submit request is truncated");
  }

  std::uint32_t network_value = 0;

  std::memcpy(&network_value, input.data() + offset, sizeof(network_value));

  offset += sizeof(network_value);

  return ::ntohl(network_value);
}

}  // namespace

bool isSubmitRequest(const std::string& payload) {
  return payload.size() >= kSubmitCommandSize &&
         payload.compare(0, kSubmitCommandSize, kSubmitCommand) == 0;
}

std::string encodeSubmitRequest(const JobSpec& spec) {
  // 编码前先检查客户端提供的 JobSpec。
  // 服务端收到请求后仍然必须重新校验。
  validateJobSpec(spec);

  if (spec.argv.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::length_error("submit request has too many arguments");
  }

  std::uint32_t timeout_ms = 0;

  if (spec.execution_timeout.has_value()) {
    const JobTimeout::rep timeout_count = spec.execution_timeout->count();

    if (timeout_count > static_cast<JobTimeout::rep>(std::numeric_limits<std::uint32_t>::max())) {
      throw std::length_error("execution timeout exceeds protocol limit");
    }

    timeout_ms = static_cast<std::uint32_t>(timeout_count);
  }

  // SUBMIT + timeout + argc。
  std::size_t payload_size = kSubmitCommandSize + sizeof(std::uint32_t) * 2U;

  // 先计算完整大小，避免构造到一半才发现超过 64 KiB。
  for (const std::string& argument : spec.argv) {
    if (argument.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
      throw std::length_error("submit argument exceeds protocol limit");
    }

    // 使用减法，避免溢出。
    if (payload_size > kMaxFrameSize - sizeof(std::uint32_t)) {
      throw std::length_error("submit request exceeds maximum frame size");
    }

    payload_size += sizeof(std::uint32_t);

    if (argument.size() > kMaxFrameSize - payload_size) {
      throw std::length_error("submit request exceeds maximum frame size");
    }

    payload_size += argument.size();
  }

  std::string payload;
  payload.reserve(payload_size);

  payload.append(kSubmitCommand, kSubmitCommandSize);

  appendUint32(payload, timeout_ms);

  appendUint32(payload, static_cast<std::uint32_t>(spec.argv.size()));

  for (const std::string& argument : spec.argv) {
    appendUint32(payload, static_cast<std::uint32_t>(argument.size()));

    payload.append(argument);
  }

  return payload;
}

JobSpec decodeSubmitRequest(const std::string& payload) {
  if (payload.size() > kMaxFrameSize) {
    throw std::length_error("submit request exceeds maximum frame size");
  }

  if (!isSubmitRequest(payload)) {
    throw std::invalid_argument("payload is not a SUBMIT request");
  }

  std::size_t offset = kSubmitCommandSize;

  const std::uint32_t timeout_ms = readUint32(payload, offset);

  const std::uint32_t argument_count = readUint32(payload, offset);

  // 每个参数至少需要一个 4 字节长度字段。
  // 先做检查，避免根据恶意 argc 进行巨大分配或循环。
  if (static_cast<std::size_t>(argument_count) >
      (payload.size() - offset) / sizeof(std::uint32_t)) {
    throw std::invalid_argument(
        "submit argument count exceeds "
        "remaining payload");
  }

  JobSpec spec;

  if (timeout_ms != 0) {
    spec.execution_timeout = JobTimeout(timeout_ms);
  }

  for (std::uint32_t index = 0; index < argument_count; ++index) {
    const std::uint32_t argument_size = readUint32(payload, offset);

    if (static_cast<std::size_t>(argument_size) > payload.size() - offset) {
      throw std::invalid_argument("submit argument is truncated");
    }

    if (argument_size == 0) {
      spec.argv.emplace_back();
    } else {
      spec.argv.emplace_back(payload.data() + offset, static_cast<std::size_t>(argument_size));
    }

    offset += static_cast<std::size_t>(argument_size);
  }

  if (offset != payload.size()) {
    throw std::invalid_argument("submit request has trailing bytes");
  }

  // 解码只负责二进制结构。
  // 服务端接下来还要调用 validateJobSpec()。
  return spec;
}

std::vector<char> encodeFrame(const std::string& payload) {
  if (payload.size() > kMaxFrameSize) {
    throw std::length_error("frame payload exceeds 64 KiB");
  }

  const std::uint32_t network_length = ::htonl(static_cast<std::uint32_t>(payload.size()));

  std::vector<char> frame(kFrameHeaderSize + payload.size());
  std::memcpy(frame.data(), &network_length, kFrameHeaderSize);

  if (!payload.empty()) {
    std::memcpy(frame.data() + kFrameHeaderSize, payload.data(), payload.size());
  }

  return frame;
}

void FrameDecoder::feed(const char* data, std::size_t size) {
  if (size == 0) {
    return;
  }

  if (data == nullptr) {
    throw std::invalid_argument("frame data is null while size is non-zero");
  }

  std::size_t offset = 0;

  // 只先补齐长度字段。这样遇到恶意长度时，不会先复制后续 payload。
  if (buffer_.size() < kFrameHeaderSize) {
    const std::size_t header_bytes = std::min(size, kFrameHeaderSize - buffer_.size());

    buffer_.insert(buffer_.end(), data, data + header_bytes);

    offset = header_bytes;

    if (buffer_.size() < kFrameHeaderSize) {
      return;
    }

    // 执行内部校验。显式丢弃返回值。
    static_cast<void>(payloadSize());
  }

  const std::size_t remaining = size - offset;

  if (remaining == 0) {
    return;
  }

  if (remaining > buffer_.max_size() - buffer_.size()) {
    throw std::length_error("frame decoder buffer is too large");
  }

  buffer_.insert(buffer_.end(), data + offset, data + size);
}

bool FrameDecoder::hasFrame() const {
  if (buffer_.size() < kFrameHeaderSize) {
    return false;
  }

  const std::size_t payload_size = payloadSize();

  // 使用减法，避免“头长度 + payload 长度”发生整数溢出。
  return buffer_.size() - kFrameHeaderSize >= payload_size;
}

std::string FrameDecoder::popFrame() {
  if (!hasFrame()) {
    throw std::runtime_error("no complete frame");
  }

  const std::size_t payload_size = payloadSize();
  std::string result;

  if (payload_size != 0) {
    result.assign(buffer_.data() + kFrameHeaderSize, payload_size);
  }

  const std::size_t frame_size = kFrameHeaderSize + payload_size;

  buffer_.erase(buffer_.begin(),
                buffer_.begin() + static_cast<std::vector<char>::difference_type>(frame_size));

  return result;
}

std::size_t FrameDecoder::payloadSize() const {
  if (buffer_.size() < kFrameHeaderSize) {
    throw std::logic_error("frame header is incomplete");
  }

  std::uint32_t network_length = 0;
  std::memcpy(&network_length, buffer_.data(), kFrameHeaderSize);

  const std::size_t payload_size = static_cast<std::size_t>(::ntohl(network_length));

  if (payload_size > kMaxFrameSize) {
    throw std::runtime_error("declared frame payload exceeds 64 KiB");
  }

  return payload_size;
}

}  // namespace runnerd

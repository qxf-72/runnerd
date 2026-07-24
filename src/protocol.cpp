#include "runnerd/protocol.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace runnerd {

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

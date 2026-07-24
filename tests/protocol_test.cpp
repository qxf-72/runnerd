#include "runnerd/protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// 使用异常报告断言失败，避免 Release 模式定义 NDEBUG 后测试失效。
void expect(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// 验证指定操作必须抛出预期类型的异常。
template <typename Exception, typename Function>
void expectThrows(Function&& function, const std::string& message) {
  try {
    function();
  } catch (const Exception&) {
    return;
  }

  throw std::runtime_error(message);
}

// 验证帧头使用 4 字节大端序记录 payload 长度。
void testEncodingUsesBigEndianLength() {
  const std::vector<char> frame = runnerd::encodeFrame("PING");

  expect(frame.size() == 8, "PING frame size is incorrect");
  expect(frame[0] == 0, "length byte 0 is incorrect");
  expect(frame[1] == 0, "length byte 1 is incorrect");
  expect(frame[2] == 0, "length byte 2 is incorrect");
  expect(frame[3] == 4, "length byte 3 is incorrect");
  expect(std::string(frame.data() + runnerd::kFrameHeaderSize, 4) == "PING",
         "encoded payload is incorrect");
}

// 模拟一次 read() 只收到一个字节，验证解码器能够处理拆包。
void testFragmentedFrame() {
  const std::vector<char> frame = runnerd::encodeFrame("PING");
  runnerd::FrameDecoder decoder;

  for (std::size_t i = 0; i < frame.size(); ++i) {
    // 在最后一个字节到达前，都不应该产生完整帧。
    decoder.feed(frame.data() + i, 1);

    if (i + 1 < frame.size()) {
      expect(!decoder.hasFrame(), "fragmented frame completed too early");
    }
  }

  expect(decoder.hasFrame(), "fragmented frame was not decoded");
  expect(decoder.popFrame() == "PING", "decoded payload is incorrect");
  expect(!decoder.hasFrame(), "decoder should be empty after pop");
}

// 模拟一次 read() 收到两个连续帧，验证解码器能够处理粘包。
void testMultipleFramesInOneFeed() {
  const std::vector<char> first = runnerd::encodeFrame("PING");
  const std::vector<char> second = runnerd::encodeFrame("PONG");

  std::vector<char> combined;
  combined.reserve(first.size() + second.size());
  combined.insert(combined.end(), first.begin(), first.end());
  combined.insert(combined.end(), second.begin(), second.end());

  runnerd::FrameDecoder decoder;
  decoder.feed(combined.data(), combined.size());

  expect(decoder.hasFrame(), "first combined frame is missing");
  expect(decoder.popFrame() == "PING", "first combined frame is wrong");
  expect(decoder.hasFrame(), "second combined frame is missing");
  expect(decoder.popFrame() == "PONG", "second combined frame is wrong");
  expect(!decoder.hasFrame(), "combined frame buffer should be empty");
}

// 验证长度为 0 的 payload，以及包含 '\0' 的二进制 payload。
void testEmptyAndBinaryPayloads() {
  runnerd::FrameDecoder empty_decoder;
  const std::vector<char> empty_frame = runnerd::encodeFrame("");

  empty_decoder.feed(empty_frame.data(), empty_frame.size());
  expect(empty_decoder.hasFrame(), "empty frame was not decoded");
  expect(empty_decoder.popFrame().empty(), "empty payload is not empty");

  // std::string 可以保存 '\0'，因此协议不能把 payload 当作 C 字符串。
  const std::string binary_payload("A\0B", 3);
  const std::vector<char> binary_frame = runnerd::encodeFrame(binary_payload);
  runnerd::FrameDecoder binary_decoder;

  binary_decoder.feed(binary_frame.data(), binary_frame.size());
  expect(binary_decoder.hasFrame(), "binary frame was not decoded");
  expect(binary_decoder.popFrame() == binary_payload, "embedded NUL byte was not preserved");
}

// 验证 64 KiB 合法边界，以及超过边界一个字节时编码失败。
void testFrameSizeLimits() {
  const std::string maximum_payload(runnerd::kMaxFrameSize, 'x');
  const std::vector<char> maximum_frame = runnerd::encodeFrame(maximum_payload);
  runnerd::FrameDecoder decoder;

  decoder.feed(maximum_frame.data(), maximum_frame.size());
  expect(decoder.hasFrame(), "maximum-size frame was rejected");
  expect(decoder.popFrame() == maximum_payload, "maximum-size payload was corrupted");

  const std::string oversized_payload(runnerd::kMaxFrameSize + 1, 'x');

  expectThrows<std::length_error>(
      [&oversized_payload]() { static_cast<void>(runnerd::encodeFrame(oversized_payload)); },
      "oversized payload was accepted by encoder");
}

// 验证解码器会在读取帧头后立即拒绝非法的声明长度。
void testInvalidDeclaredLengths() {
  // 大端字节 00 01 00 01 表示 65537，刚好比 64 KiB 大 1。
  const std::array<char, 4> just_over_limit{
      0x00,
      0x01,
      0x00,
      0x01,
  };
  runnerd::FrameDecoder oversized_decoder;

  expectThrows<std::runtime_error>(
      [&oversized_decoder, &just_over_limit]() {
        oversized_decoder.feed(just_over_limit.data(), just_over_limit.size());
      },
      "oversized declared length was accepted");

  // 0xffffffff 用于回归测试，防止“帧头长度 + payload 长度”整数溢出。
  const std::array<char, 4> maximum_uint32{
      static_cast<char>(0xff),
      static_cast<char>(0xff),
      static_cast<char>(0xff),
      static_cast<char>(0xff),
  };
  runnerd::FrameDecoder overflow_decoder;

  expectThrows<std::runtime_error>(
      [&overflow_decoder, &maximum_uint32]() {
        overflow_decoder.feed(maximum_uint32.data(), maximum_uint32.size());
      },
      "overflowing declared length was accepted");
}

// 验证不完整帧和无效指针参数不会被误认为合法输入。
void testIncompleteAndInvalidInput() {
  const std::vector<char> frame = runnerd::encodeFrame("PING");
  runnerd::FrameDecoder decoder;

  decoder.feed(frame.data(), 3);
  expect(!decoder.hasFrame(), "incomplete header was accepted");
  expectThrows<std::runtime_error>([&decoder]() { static_cast<void>(decoder.popFrame()); },
                                   "popFrame accepted an incomplete frame");

  runnerd::FrameDecoder empty_decoder;
  empty_decoder.feed(nullptr, 0);
  expect(!empty_decoder.hasFrame(), "zero-byte feed changed decoder");

  expectThrows<std::invalid_argument>([&empty_decoder]() { empty_decoder.feed(nullptr, 1); },
                                      "non-zero null input was accepted");
}

}  // namespace

int main() {
  try {
    // 每个测试函数只验证一类协议行为，失败时由异常统一报告。
    testEncodingUsesBigEndianLength();
    testFragmentedFrame();
    testMultipleFramesInOneFeed();
    testEmptyAndBinaryPayloads();
    testFrameSizeLimits();
    testInvalidDeclaredLengths();
    testIncompleteAndInvalidInput();

    std::cout << "protocol tests passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "protocol test failed: " << exception.what() << '\n';
    return 1;
  }
}

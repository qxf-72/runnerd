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

// 正常编解码往返
void testSubmitRequestRoundTrip() {
  runnerd::JobSpec original;
  original.argv = {
      "/bin/echo",
      "hello world",
      "",
  };

  const std::string payload = runnerd::encodeSubmitRequest(original);

  expect(runnerd::isSubmitRequest(payload), "encoded SUBMIT request was not recognized");

  const runnerd::JobSpec decoded = runnerd::decodeSubmitRequest(payload);

  expect(decoded.argv == original.argv, "SUBMIT argv was not preserved");

  expect(!decoded.execution_timeout.has_value(), "SUBMIT without timeout gained a timeout");

  original.execution_timeout = runnerd::JobTimeout(5000);

  const runnerd::JobSpec decoded_with_timeout =
      runnerd::decodeSubmitRequest(runnerd::encodeSubmitRequest(original));

  expect(decoded_with_timeout.argv == original.argv, "timed SUBMIT argv was not preserved");

  expect(decoded_with_timeout.execution_timeout.has_value(), "SUBMIT timeout is missing");

  expect(decoded_with_timeout.execution_timeout->count() == 5000,
         "SUBMIT timeout was not preserved");
}

// 验证大端序
void testSubmitEncodingUsesBigEndianIntegers() {
  runnerd::JobSpec spec;
  spec.argv = {"/bin/echo"};
  spec.execution_timeout = runnerd::JobTimeout(5000);

  const std::string payload = runnerd::encodeSubmitRequest(spec);

  expect(payload.size() == 27, "SUBMIT payload size is incorrect");

  expect(payload.compare(0, 6, "SUBMIT") == 0, "SUBMIT command marker is incorrect");

  // 5000 == 0x00001388。
  expect(static_cast<unsigned char>(payload[6]) == 0x00U, "timeout byte 0 is incorrect");

  expect(static_cast<unsigned char>(payload[7]) == 0x00U, "timeout byte 1 is incorrect");

  expect(static_cast<unsigned char>(payload[8]) == 0x13U, "timeout byte 2 is incorrect");

  expect(static_cast<unsigned char>(payload[9]) == 0x88U, "timeout byte 3 is incorrect");

  // argc == 1，所以第 14 个字节是 1。
  expect(static_cast<unsigned char>(payload[13]) == 0x01U, "argument count is not big-endian");

  // strlen("/bin/echo") == 9。
  expect(static_cast<unsigned char>(payload[17]) == 0x09U, "argument length is not big-endian");

  expect(payload.substr(18) == "/bin/echo", "encoded argument bytes are incorrect");
}

// 畸形请求
void testMalformedSubmitRequests() {
  runnerd::JobSpec spec;
  spec.argv = {"/bin/echo"};

  const std::string valid = runnerd::encodeSubmitRequest(spec);

  expectThrows<std::invalid_argument>(
      []() { static_cast<void>(runnerd::decodeSubmitRequest("SUBMI")); },
      "incomplete SUBMIT marker was accepted");

  const std::string truncated_timeout = valid.substr(0, 9);

  expectThrows<std::invalid_argument>(
      [&truncated_timeout]() {
        static_cast<void>(runnerd::decodeSubmitRequest(truncated_timeout));
      },
      "truncated timeout was accepted");

  const std::string truncated_argc = valid.substr(0, 13);

  expectThrows<std::invalid_argument>(
      [&truncated_argc]() { static_cast<void>(runnerd::decodeSubmitRequest(truncated_argc)); },
      "truncated argc was accepted");

  const std::string truncated_argument_length = valid.substr(0, 16);

  expectThrows<std::invalid_argument>(
      [&truncated_argument_length]() {
        static_cast<void>(runnerd::decodeSubmitRequest(truncated_argument_length));
      },
      "truncated argument length was accepted");

  // 把 argv[0] 的长度从 9 改成 100，
  // 但后面实际没有 100 字节内容。
  std::string oversized_argument = valid;
  oversized_argument[14] = 0;
  oversized_argument[15] = 0;
  oversized_argument[16] = 0;
  oversized_argument[17] = 100;

  expectThrows<std::invalid_argument>(
      [&oversized_argument]() {
        static_cast<void>(runnerd::decodeSubmitRequest(oversized_argument));
      },
      "argument larger than remaining "
      "payload was accepted");

  // 原来 argc 是 1，这里改成 2，
  // 但没有提供第二个参数。
  std::string argc_mismatch = valid;
  argc_mismatch[10] = 0;
  argc_mismatch[11] = 0;
  argc_mismatch[12] = 0;
  argc_mismatch[13] = 2;

  expectThrows<std::invalid_argument>(
      [&argc_mismatch]() { static_cast<void>(runnerd::decodeSubmitRequest(argc_mismatch)); },
      "argc larger than actual argument "
      "count was accepted");

  std::string trailing_bytes = valid;
  trailing_bytes.push_back('x');

  expectThrows<std::invalid_argument>(
      [&trailing_bytes]() { static_cast<void>(runnerd::decodeSubmitRequest(trailing_bytes)); },
      "trailing SUBMIT bytes were accepted");
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
    testSubmitRequestRoundTrip();
    testSubmitEncodingUsesBigEndianIntegers();
    testMalformedSubmitRequests();
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

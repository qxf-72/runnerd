#include "runnerd/protocol.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

TEST(SubmitProtocolTest, RoundTripsArgumentsAndOptionalTimeout) {
  runnerd::JobSpec original;
  original.argv = {
      "/bin/echo",
      "hello world",
      "",
  };

  const std::string payload = runnerd::encodeSubmitRequest(original);

  EXPECT_TRUE(runnerd::isSubmitRequest(payload));

  const runnerd::JobSpec decoded = runnerd::decodeSubmitRequest(payload);

  EXPECT_EQ(decoded.argv, original.argv);
  EXPECT_FALSE(decoded.execution_timeout.has_value());

  original.execution_timeout = runnerd::JobTimeout(5000);

  const runnerd::JobSpec decoded_with_timeout =
      runnerd::decodeSubmitRequest(runnerd::encodeSubmitRequest(original));

  EXPECT_EQ(decoded_with_timeout.argv, original.argv);
  ASSERT_TRUE(decoded_with_timeout.execution_timeout.has_value());
  EXPECT_EQ(decoded_with_timeout.execution_timeout->count(), 5000);
}

TEST(SubmitProtocolTest, UsesBigEndianIntegers) {
  runnerd::JobSpec spec;
  spec.argv = {"/bin/echo"};
  spec.execution_timeout = runnerd::JobTimeout(5000);

  const std::string payload = runnerd::encodeSubmitRequest(spec);

  ASSERT_EQ(payload.size(), 27U);
  EXPECT_EQ(payload.compare(0, 6, "SUBMIT"), 0);

  // 5000 == 0x00001388。
  EXPECT_EQ(static_cast<unsigned char>(payload[6]), 0x00U);
  EXPECT_EQ(static_cast<unsigned char>(payload[7]), 0x00U);
  EXPECT_EQ(static_cast<unsigned char>(payload[8]), 0x13U);
  EXPECT_EQ(static_cast<unsigned char>(payload[9]), 0x88U);

  // argc == 1，strlen("/bin/echo") == 9。
  EXPECT_EQ(static_cast<unsigned char>(payload[13]), 0x01U);
  EXPECT_EQ(static_cast<unsigned char>(payload[17]), 0x09U);
  EXPECT_EQ(payload.substr(18), "/bin/echo");
}

TEST(SubmitProtocolTest, RejectsMalformedPayloads) {
  runnerd::JobSpec spec;
  spec.argv = {"/bin/echo"};

  const std::string valid = runnerd::encodeSubmitRequest(spec);

  EXPECT_THROW(runnerd::decodeSubmitRequest("SUBMI"), std::invalid_argument);

  const std::string truncated_timeout = valid.substr(0, 9);
  EXPECT_THROW(runnerd::decodeSubmitRequest(truncated_timeout), std::invalid_argument);

  const std::string truncated_argc = valid.substr(0, 13);
  EXPECT_THROW(runnerd::decodeSubmitRequest(truncated_argc), std::invalid_argument);

  const std::string truncated_argument_length = valid.substr(0, 16);
  EXPECT_THROW(runnerd::decodeSubmitRequest(truncated_argument_length), std::invalid_argument);

  // 把 argv[0] 的长度从 9 改成 100，但不提供对应内容。
  std::string oversized_argument = valid;
  oversized_argument[14] = 0;
  oversized_argument[15] = 0;
  oversized_argument[16] = 0;
  oversized_argument[17] = 100;
  EXPECT_THROW(runnerd::decodeSubmitRequest(oversized_argument), std::invalid_argument);

  // 把 argc 从 1 改成 2，但不提供第二个参数。
  std::string argc_mismatch = valid;
  argc_mismatch[10] = 0;
  argc_mismatch[11] = 0;
  argc_mismatch[12] = 0;
  argc_mismatch[13] = 2;
  EXPECT_THROW(runnerd::decodeSubmitRequest(argc_mismatch), std::invalid_argument);

  std::string trailing_bytes = valid;
  trailing_bytes.push_back('x');
  EXPECT_THROW(runnerd::decodeSubmitRequest(trailing_bytes), std::invalid_argument);
}

TEST(StatusProtocolTest, RoundTripsBigEndianJobId) {
  const runnerd::JobId job_id = static_cast<runnerd::JobId>(0x0102030405060708ULL);

  const std::string payload = runnerd::encodeStatusRequest(job_id);

  ASSERT_EQ(payload.size(), 14U);
  EXPECT_EQ(payload.compare(0, 6, "STATUS"), 0);

  EXPECT_EQ(static_cast<unsigned char>(payload[6]), 0x01U);
  EXPECT_EQ(static_cast<unsigned char>(payload[7]), 0x02U);
  EXPECT_EQ(static_cast<unsigned char>(payload[8]), 0x03U);
  EXPECT_EQ(static_cast<unsigned char>(payload[9]), 0x04U);
  EXPECT_EQ(static_cast<unsigned char>(payload[10]), 0x05U);
  EXPECT_EQ(static_cast<unsigned char>(payload[11]), 0x06U);
  EXPECT_EQ(static_cast<unsigned char>(payload[12]), 0x07U);
  EXPECT_EQ(static_cast<unsigned char>(payload[13]), 0x08U);

  EXPECT_TRUE(runnerd::isStatusRequest(payload));
  EXPECT_EQ(runnerd::decodeStatusRequest(payload), job_id);
}

TEST(StatusProtocolTest, RejectsMalformedRequests) {
  EXPECT_THROW(runnerd::encodeStatusRequest(0), std::invalid_argument);
  EXPECT_THROW(runnerd::decodeStatusRequest("STATUS"), std::invalid_argument);

  std::string zero_job_id("STATUS", 6);
  zero_job_id.append(sizeof(runnerd::JobId), '\0');
  EXPECT_THROW(runnerd::decodeStatusRequest(zero_job_id), std::invalid_argument);

  std::string trailing_bytes = runnerd::encodeStatusRequest(1);
  trailing_bytes.push_back('x');
  EXPECT_THROW(runnerd::decodeStatusRequest(trailing_bytes), std::invalid_argument);

  std::string wrong_marker = runnerd::encodeStatusRequest(1);
  wrong_marker[0] = 'X';
  EXPECT_THROW(runnerd::decodeStatusRequest(wrong_marker), std::invalid_argument);
}

TEST(CancelProtocolTest, RoundTripsBigEndianJobId) {
  const runnerd::JobId job_id = static_cast<runnerd::JobId>(0x0102030405060708ULL);

  const std::string payload = runnerd::encodeCancelRequest(job_id);

  ASSERT_EQ(payload.size(), 14U);
  EXPECT_EQ(payload.compare(0, 6, "CANCEL"), 0);

  // JobId 按照网络大端序写入：最高有效字节 0x01 最先出现。
  EXPECT_EQ(static_cast<unsigned char>(payload[6]), 0x01U);
  EXPECT_EQ(static_cast<unsigned char>(payload[7]), 0x02U);
  EXPECT_EQ(static_cast<unsigned char>(payload[8]), 0x03U);
  EXPECT_EQ(static_cast<unsigned char>(payload[9]), 0x04U);
  EXPECT_EQ(static_cast<unsigned char>(payload[10]), 0x05U);
  EXPECT_EQ(static_cast<unsigned char>(payload[11]), 0x06U);
  EXPECT_EQ(static_cast<unsigned char>(payload[12]), 0x07U);
  EXPECT_EQ(static_cast<unsigned char>(payload[13]), 0x08U);

  EXPECT_TRUE(runnerd::isCancelRequest(payload));
  EXPECT_EQ(runnerd::decodeCancelRequest(payload), job_id);
}

TEST(CancelProtocolTest, RejectsMalformedRequests) {
  EXPECT_THROW(runnerd::encodeCancelRequest(0), std::invalid_argument);

  // 连命令前缀都不完整时，不应被识别成 CANCEL。
  EXPECT_FALSE(runnerd::isCancelRequest("CANCE"));
  EXPECT_THROW(runnerd::decodeCancelRequest("CANCE"), std::invalid_argument);

  // 只有 CANCEL 前缀、没有 JobId，属于已经识别但结构被截断的请求。
  EXPECT_TRUE(runnerd::isCancelRequest("CANCEL"));
  EXPECT_THROW(runnerd::decodeCancelRequest("CANCEL"), std::invalid_argument);

  std::string zero_job_id("CANCEL", 6);
  zero_job_id.append(sizeof(runnerd::JobId), '\0');
  EXPECT_THROW(runnerd::decodeCancelRequest(zero_job_id), std::invalid_argument);

  std::string trailing_bytes = runnerd::encodeCancelRequest(1);
  trailing_bytes.push_back('x');
  EXPECT_THROW(runnerd::decodeCancelRequest(trailing_bytes), std::invalid_argument);

  std::string wrong_marker = runnerd::encodeCancelRequest(1);
  wrong_marker[0] = 'X';
  EXPECT_THROW(runnerd::decodeCancelRequest(wrong_marker), std::invalid_argument);
}

TEST(FrameEncodingTest, UsesBigEndianPayloadLength) {
  const std::vector<char> frame = runnerd::encodeFrame("PING");

  ASSERT_EQ(frame.size(), 8U);
  EXPECT_EQ(frame[0], 0);
  EXPECT_EQ(frame[1], 0);
  EXPECT_EQ(frame[2], 0);
  EXPECT_EQ(frame[3], 4);
  EXPECT_EQ(std::string(frame.data() + runnerd::kFrameHeaderSize, 4), "PING");
}

TEST(FrameDecoderTest, DecodesFrameArrivingOneByteAtATime) {
  const std::vector<char> frame = runnerd::encodeFrame("PING");
  runnerd::FrameDecoder decoder;

  for (std::size_t index = 0; index < frame.size(); ++index) {
    decoder.feed(frame.data() + index, 1);

    if (index + 1 < frame.size()) {
      EXPECT_FALSE(decoder.hasFrame());
    }
  }

  ASSERT_TRUE(decoder.hasFrame());
  EXPECT_EQ(decoder.popFrame(), "PING");
  EXPECT_FALSE(decoder.hasFrame());
}

TEST(FrameDecoderTest, DecodesMultipleFramesFromOneFeed) {
  const std::vector<char> first = runnerd::encodeFrame("PING");
  const std::vector<char> second = runnerd::encodeFrame("PONG");

  std::vector<char> combined;
  combined.reserve(first.size() + second.size());
  combined.insert(combined.end(), first.begin(), first.end());
  combined.insert(combined.end(), second.begin(), second.end());

  runnerd::FrameDecoder decoder;
  decoder.feed(combined.data(), combined.size());

  ASSERT_TRUE(decoder.hasFrame());
  EXPECT_EQ(decoder.popFrame(), "PING");
  ASSERT_TRUE(decoder.hasFrame());
  EXPECT_EQ(decoder.popFrame(), "PONG");
  EXPECT_FALSE(decoder.hasFrame());
}

TEST(FrameDecoderTest, PreservesEmptyAndBinaryPayloads) {
  runnerd::FrameDecoder empty_decoder;
  const std::vector<char> empty_frame = runnerd::encodeFrame("");

  empty_decoder.feed(empty_frame.data(), empty_frame.size());
  ASSERT_TRUE(empty_decoder.hasFrame());
  EXPECT_TRUE(empty_decoder.popFrame().empty());

  const std::string binary_payload("A\0B", 3);
  const std::vector<char> binary_frame = runnerd::encodeFrame(binary_payload);
  runnerd::FrameDecoder binary_decoder;

  binary_decoder.feed(binary_frame.data(), binary_frame.size());
  ASSERT_TRUE(binary_decoder.hasFrame());
  EXPECT_EQ(binary_decoder.popFrame(), binary_payload);
}

TEST(FrameDecoderTest, EnforcesMaximumFrameSize) {
  const std::string maximum_payload(runnerd::kMaxFrameSize, 'x');
  const std::vector<char> maximum_frame = runnerd::encodeFrame(maximum_payload);
  runnerd::FrameDecoder decoder;

  decoder.feed(maximum_frame.data(), maximum_frame.size());
  ASSERT_TRUE(decoder.hasFrame());
  EXPECT_EQ(decoder.popFrame(), maximum_payload);

  const std::string oversized_payload(runnerd::kMaxFrameSize + 1, 'x');
  EXPECT_THROW(runnerd::encodeFrame(oversized_payload), std::length_error);
}

TEST(FrameDecoderTest, RejectsInvalidDeclaredLengths) {
  const std::array<char, 4> just_over_limit{
      0x00,
      0x01,
      0x00,
      0x01,
  };
  runnerd::FrameDecoder oversized_decoder;

  EXPECT_THROW(oversized_decoder.feed(just_over_limit.data(), just_over_limit.size()),
               std::runtime_error);

  const std::array<char, 4> maximum_uint32{
      static_cast<char>(0xff),
      static_cast<char>(0xff),
      static_cast<char>(0xff),
      static_cast<char>(0xff),
  };
  runnerd::FrameDecoder overflow_decoder;

  EXPECT_THROW(overflow_decoder.feed(maximum_uint32.data(), maximum_uint32.size()),
               std::runtime_error);
}

TEST(FrameDecoderTest, RejectsIncompleteAndInvalidInput) {
  const std::vector<char> frame = runnerd::encodeFrame("PING");
  runnerd::FrameDecoder decoder;

  decoder.feed(frame.data(), 3);
  EXPECT_FALSE(decoder.hasFrame());
  EXPECT_THROW(decoder.popFrame(), std::runtime_error);

  runnerd::FrameDecoder empty_decoder;
  EXPECT_NO_THROW(empty_decoder.feed(nullptr, 0));
  EXPECT_FALSE(empty_decoder.hasFrame());
  EXPECT_THROW(empty_decoder.feed(nullptr, 1), std::invalid_argument);
}

}  // namespace

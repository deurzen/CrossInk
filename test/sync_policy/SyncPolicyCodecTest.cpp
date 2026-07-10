#include <SyncPolicyCodec.h>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {
using namespace DeviceSync;

struct MemoryOutput {
  std::vector<uint8_t> bytes;
  size_t failAtSize = SIZE_MAX;
};

struct MemoryInput {
  const uint8_t* bytes = nullptr;
  size_t size = 0;
  size_t position = 0;
};

bool writeMemory(void* context, const void* data, const size_t length) {
  auto* output = static_cast<MemoryOutput*>(context);
  if (output->bytes.size() + length > output->failAtSize) return false;
  const auto* bytes = static_cast<const uint8_t*>(data);
  output->bytes.insert(output->bytes.end(), bytes, bytes + length);
  return true;
}

bool readMemory(void* context, void* data, const size_t length) {
  auto* input = static_cast<MemoryInput*>(context);
  if (length > input->size - input->position) return false;
  std::memcpy(data, input->bytes + input->position, length);
  input->position += length;
  return true;
}

std::vector<uint8_t> encodePolicy(const SyncPolicy& policy) {
  MemoryOutput output;
  output.bytes.reserve(SyncPolicyCodec::MAX_ENCODED_SIZE);
  const SyncPolicyCodec::Output sink{&output, writeMemory};
  EXPECT_TRUE(SyncPolicyCodec::encode(policy, sink));
  return output.bytes;
}

SyncPolicyCodec::DecodeResult decodePolicy(const std::vector<uint8_t>& bytes, SyncPolicy& policy) {
  MemoryInput memory{bytes.data(), bytes.size(), 0};
  const SyncPolicyCodec::Input input{&memory, readMemory, bytes.size()};
  return SyncPolicyCodec::decode(input, policy);
}

SyncPolicyCodec::DecodeResult validatePolicy(const std::vector<uint8_t>& bytes) {
  MemoryInput memory{bytes.data(), bytes.size(), 0};
  const SyncPolicyCodec::Input input{&memory, readMemory, bytes.size()};
  return SyncPolicyCodec::validate(input);
}

void expectPoliciesEqual(const SyncPolicy& expected, const SyncPolicy& actual) {
  EXPECT_EQ(actual.mirrorDeletions(), expected.mirrorDeletions());
  for (size_t i = 0; i < CATEGORY_COUNT; ++i) {
    EXPECT_EQ(actual.direction(static_cast<Category>(i)), expected.direction(static_cast<Category>(i))) << i;
  }
  ASSERT_EQ(actual.pathRuleCount(), expected.pathRuleCount());
  for (size_t i = 0; i < expected.pathRuleCount(); ++i) {
    const PathRule* expectedRule = expected.pathRule(i);
    const PathRule* actualRule = actual.pathRule(i);
    ASSERT_NE(expectedRule, nullptr);
    ASSERT_NE(actualRule, nullptr);
    EXPECT_EQ(actualRule->action, expectedRule->action) << i;
    EXPECT_STREQ(actualRule->pattern, expectedRule->pattern) << i;
  }
}

TEST(SyncPolicyCodecTest, RoundTripsAllBoundedFields) {
  SyncPolicy expected;
  expected.setDefaults();
  expected.setMirrorDeletions(true);
  ASSERT_TRUE(expected.setDirection(Category::BookContent, Direction::SendOnly));
  ASSERT_TRUE(expected.setDirection(Category::ReadingStats, Direction::ReceiveOnly));
  ASSERT_TRUE(expected.addPathRule(PathRuleAction::Include, "/Shared/**"));

  const std::vector<uint8_t> bytes = encodePolicy(expected);
  EXPECT_EQ(bytes.size(), SyncPolicyCodec::encodedSize(expected));

  SyncPolicy actual;
  ASSERT_EQ(decodePolicy(bytes, actual), SyncPolicyCodec::DecodeResult::Ok);
  expectPoliciesEqual(expected, actual);
}

TEST(SyncPolicyCodecTest, ValidationOnlyUsesTheSameStrictFormatChecks) {
  SyncPolicy policy;
  policy.setDefaults();
  const std::vector<uint8_t> complete = encodePolicy(policy);
  EXPECT_EQ(validatePolicy(complete), SyncPolicyCodec::DecodeResult::Ok);

  std::vector<uint8_t> corrupt = complete;
  corrupt.back() ^= 0x80;
  EXPECT_EQ(validatePolicy(corrupt), SyncPolicyCodec::DecodeResult::Invalid);

  std::vector<uint8_t> future = complete;
  future[4] = static_cast<uint8_t>(SyncPolicyCodec::FORMAT_VERSION + 1);
  future[5] = 0;
  future.insert(future.end(), SyncPolicyCodec::MAX_ENCODED_SIZE, 0xA5);
  EXPECT_EQ(validatePolicy(future), SyncPolicyCodec::DecodeResult::Unsupported);
}

TEST(SyncPolicyCodecTest, EmptyPolicyUsesMinimumEncoding) {
  const SyncPolicy policy;
  const std::vector<uint8_t> bytes = encodePolicy(policy);
  EXPECT_EQ(bytes.size(), SyncPolicyCodec::MIN_ENCODED_SIZE);
}

TEST(SyncPolicyCodecTest, RejectsEveryTruncationBoundary) {
  SyncPolicy policy;
  policy.setDefaults();
  const std::vector<uint8_t> complete = encodePolicy(policy);
  for (size_t length = 0; length < complete.size(); ++length) {
    std::vector<uint8_t> truncated(complete.begin(), complete.begin() + length);
    SyncPolicy decoded;
    EXPECT_NE(decodePolicy(truncated, decoded), SyncPolicyCodec::DecodeResult::Ok) << length;
    EXPECT_EQ(decoded.direction(Category::BookContent), Direction::Disabled) << length;
    EXPECT_EQ(decoded.pathRuleCount(), 0u) << length;
  }
}

TEST(SyncPolicyCodecTest, RejectsCorruptionAtEveryByte) {
  SyncPolicy policy;
  policy.setDefaults();
  const std::vector<uint8_t> complete = encodePolicy(policy);
  for (size_t index = 0; index < complete.size(); ++index) {
    std::vector<uint8_t> corrupt = complete;
    corrupt[index] ^= 0x80;
    SyncPolicy decoded;
    EXPECT_NE(decodePolicy(corrupt, decoded), SyncPolicyCodec::DecodeResult::Ok) << index;
  }
}

TEST(SyncPolicyCodecTest, ReportsNewerVersionWithoutParsingPayload) {
  SyncPolicy policy;
  policy.setDefaults();
  std::vector<uint8_t> bytes = encodePolicy(policy);
  bytes[4] = static_cast<uint8_t>(SyncPolicyCodec::FORMAT_VERSION + 1);
  bytes[5] = 0;

  SyncPolicy decoded;
  decoded.setDefaults();
  EXPECT_EQ(decodePolicy(bytes, decoded), SyncPolicyCodec::DecodeResult::Unsupported);
  EXPECT_EQ(decoded.direction(Category::BookContent), Direction::Disabled);
  EXPECT_EQ(decoded.pathRuleCount(), 0u);
}

TEST(SyncPolicyCodecTest, RejectsDeclaredLengthMismatch) {
  SyncPolicy policy;
  policy.setDefaults();
  std::vector<uint8_t> bytes = encodePolicy(policy);
  bytes[6] ^= 0x01;

  SyncPolicy decoded;
  EXPECT_EQ(decodePolicy(bytes, decoded), SyncPolicyCodec::DecodeResult::Invalid);
}

TEST(SyncPolicyCodecTest, PropagatesShortWriteFailure) {
  MemoryOutput memory;
  memory.failAtSize = 8;
  const SyncPolicyCodec::Output output{&memory, writeMemory};

  SyncPolicy policy;
  policy.setDefaults();
  EXPECT_FALSE(SyncPolicyCodec::encode(policy, output));
}

TEST(SyncPolicyCodecTest, MaximumPolicyFitsDeclaredBound) {
  SyncPolicy policy;
  for (size_t i = 0; i < MAX_POLICY_RULES; ++i) {
    ASSERT_TRUE(policy.addPathRule(PathRuleAction::Include,
                                   "/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/**"));
  }
  EXPECT_LE(SyncPolicyCodec::encodedSize(policy), SyncPolicyCodec::MAX_ENCODED_SIZE);
  EXPECT_EQ(encodePolicy(policy).size(), SyncPolicyCodec::encodedSize(policy));
}

}  // namespace

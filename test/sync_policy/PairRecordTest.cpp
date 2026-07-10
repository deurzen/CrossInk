#include <PairRecordCodec.h>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {
using namespace DeviceSync;

struct MemoryOutput {
  std::vector<uint8_t> bytes;
};

struct MemoryInput {
  const uint8_t* bytes = nullptr;
  size_t size = 0;
  size_t position = 0;
};

bool writeMemory(void* context, const void* data, const size_t length) {
  auto* output = static_cast<MemoryOutput*>(context);
  const auto* bytes = static_cast<const uint8_t*>(data);
  output->bytes.insert(output->bytes.end(), bytes, bytes + length);
  return true;
}

bool readMemory(void* context, void* data, const size_t length) {
  auto* input = static_cast<MemoryInput*>(context);
  if (input->position > input->size || length > input->size - input->position) return false;
  std::memcpy(data, input->bytes + input->position, length);
  input->position += length;
  return true;
}

PairRecord validRecord(const char* name = "X4") {
  PairRecord record;
  for (size_t i = 0; i < record.peerDeviceId.size(); ++i) record.peerDeviceId[i] = static_cast<uint8_t>(i + 1);
  EXPECT_TRUE(record.setPeerDisplayName(name));
  for (size_t i = 0; i < record.pairSecret.size(); ++i) record.pairSecret[i] = static_cast<uint8_t>(0x80 + i);
  record.firstPairedLocalGeneration = 42;
  for (size_t i = 0; i < record.lastSuccessfulSessionId.size(); ++i) {
    record.lastSuccessfulSessionId[i] = static_cast<uint8_t>(0x40 + i);
  }
  record.hasLastSuccessfulSession = true;
  record.capabilitySnapshot = 0x1122334455667788ULL;
  for (size_t i = 0; i < record.lastObservedPolicyDigest.size(); ++i) {
    record.lastObservedPolicyDigest[i] = static_cast<uint8_t>(0x20 + i);
  }
  record.nextLocalHandshakeCounter = 123;
  record.lastAcceptedPeerHandshakeCounter = 98;
  return record;
}

std::vector<uint8_t> encodeRecord(const PairRecord& record) {
  MemoryOutput memory;
  memory.bytes.reserve(PairRecordCodec::MAX_ENCODED_SIZE);
  const CodecOutput output{&memory, writeMemory};
  EXPECT_TRUE(PairRecordCodec::encode(record, output));
  return memory.bytes;
}

PairRecordCodec::DecodeResult decodeRecord(const std::vector<uint8_t>& bytes, PairRecord& record) {
  MemoryInput memory{bytes.data(), bytes.size(), 0};
  return PairRecordCodec::decode(CodecInput{&memory, readMemory, bytes.size()}, record);
}

PairRecordCodec::DecodeResult validateRecord(const std::vector<uint8_t>& bytes) {
  MemoryInput memory{bytes.data(), bytes.size(), 0};
  return PairRecordCodec::validate(CodecInput{&memory, readMemory, bytes.size()});
}

TEST(PairRecordTest, RoundTripsAllFields) {
  const PairRecord expected = validRecord("Ink Reader \xE2\x9C\x93");
  const std::vector<uint8_t> bytes = encodeRecord(expected);
  EXPECT_EQ(bytes.size(), PairRecordCodec::encodedSize(expected));

  PairRecord actual;
  ASSERT_EQ(decodeRecord(bytes, actual), PairRecordCodec::DecodeResult::Ok);
  EXPECT_EQ(actual.peerDeviceId, expected.peerDeviceId);
  EXPECT_STREQ(actual.peerDisplayName, expected.peerDisplayName);
  EXPECT_EQ(actual.pairSecret, expected.pairSecret);
  EXPECT_EQ(actual.firstPairedLocalGeneration, expected.firstPairedLocalGeneration);
  EXPECT_EQ(actual.lastSuccessfulSessionId, expected.lastSuccessfulSessionId);
  EXPECT_EQ(actual.hasLastSuccessfulSession, expected.hasLastSuccessfulSession);
  EXPECT_EQ(actual.capabilitySnapshot, expected.capabilitySnapshot);
  EXPECT_EQ(actual.lastObservedPolicyDigest, expected.lastObservedPolicyDigest);
  EXPECT_EQ(actual.nextLocalHandshakeCounter, expected.nextLocalHandshakeCounter);
  EXPECT_EQ(actual.lastAcceptedPeerHandshakeCounter, expected.lastAcceptedPeerHandshakeCounter);
}

TEST(PairRecordTest, SupportsNoPriorSessionAndEncodingBounds) {
  PairRecord minimum = validRecord("X");
  minimum.lastSuccessfulSessionId.fill(0);
  minimum.hasLastSuccessfulSession = false;
  EXPECT_EQ(encodeRecord(minimum).size(), PairRecordCodec::MIN_ENCODED_SIZE);

  PairRecord maximum = validRecord("1234567890123456789012345678");
  EXPECT_EQ(encodeRecord(maximum).size(), PairRecordCodec::MAX_ENCODED_SIZE);
}

TEST(PairRecordTest, RejectsUnsafeNamesAndInvalidRequiredFields) {
  PairRecord record = validRecord();
  EXPECT_FALSE(record.setPeerDisplayName(""));
  EXPECT_FALSE(record.setPeerDisplayName("line\nbreak"));
  EXPECT_FALSE(record.setPeerDisplayName("\xC0\xAF"));

  record.peerDeviceId.fill(0);
  EXPECT_FALSE(record.valid());
  EXPECT_EQ(PairRecordCodec::encodedSize(record), 0u);
  EXPECT_FALSE(PairRecordCodec::encode(record, CodecOutput{}));

  record = validRecord();
  record.pairSecret.fill(0);
  EXPECT_FALSE(record.valid());
  record = validRecord();
  record.nextLocalHandshakeCounter = 0;
  EXPECT_FALSE(record.valid());
  record = validRecord();
  record.lastSuccessfulSessionId.fill(0);
  EXPECT_FALSE(record.valid());
}

TEST(PairRecordTest, RejectsEveryTruncationAndCorruptionBoundary) {
  const std::vector<uint8_t> complete = encodeRecord(validRecord());
  for (size_t length = 0; length < complete.size(); ++length) {
    const std::vector<uint8_t> truncated(complete.begin(), complete.begin() + length);
    PairRecord output = validRecord();
    EXPECT_NE(decodeRecord(truncated, output), PairRecordCodec::DecodeResult::Ok) << length;
    EXPECT_FALSE(output.valid()) << length;
  }
  for (size_t index = 0; index < complete.size(); ++index) {
    std::vector<uint8_t> corrupt = complete;
    corrupt[index] ^= 0x80;
    EXPECT_NE(validateRecord(corrupt), PairRecordCodec::DecodeResult::Ok) << index;
  }
}

TEST(PairRecordTest, PreservesExtendedFutureFormat) {
  std::vector<uint8_t> future = encodeRecord(validRecord());
  future[4] = static_cast<uint8_t>(PairRecordCodec::FORMAT_VERSION + 1);
  future[5] = 0;
  future.insert(future.end(), PairRecordCodec::MAX_ENCODED_SIZE, 0xA5);

  PairRecord output = validRecord();
  EXPECT_EQ(decodeRecord(future, output), PairRecordCodec::DecodeResult::Unsupported);
  EXPECT_FALSE(output.valid());
  EXPECT_EQ(validateRecord(future), PairRecordCodec::DecodeResult::Unsupported);
}

}  // namespace

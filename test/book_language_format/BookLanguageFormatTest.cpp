#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "BookLanguageFormat.h"

namespace {
using dictionary::book_language::FormatError;
using dictionary::book_language::Header;

void writeU16(std::vector<uint8_t>& data, const size_t offset, const uint16_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

void writeU32(std::vector<uint8_t>& data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8U);
  data[offset + 2] = static_cast<uint8_t>(value >> 16U);
  data[offset + 3] = static_cast<uint8_t>(value >> 24U);
}

void refreshCrcs(std::vector<uint8_t>& data) {
  writeU32(data, 100,
           dictionary::book_language::updateCrc32(0, data.data() + dictionary::book_language::kHeaderSize,
                                                  data.size() - dictionary::book_language::kHeaderSize));
  writeU32(data, 104, dictionary::book_language::updateCrc32(0, data.data(), 104));
}

std::vector<uint8_t> makeValidArtifact() {
  // Header + one spine + one shard directory entry + one candidate + one
  // local lemma + one global-to-local entry + four metadata bytes.
  std::vector<uint8_t> data(164, 0);
  std::memcpy(data.data(), "CXLG", 4);
  writeU16(data, 4, dictionary::book_language::kFormatVersion);
  writeU16(data, 6, dictionary::book_language::kHeaderSize);
  writeU16(data, 12, 1);  // Tokenizer version.
  writeU16(data, 14, 1);  // Analyzer version.
  for (size_t i = 0; i < 16; ++i) data[16 + i] = static_cast<uint8_t>(i + 1);
  std::memcpy(data.data() + 32, "de", 2);
  std::memcpy(data.data() + 40, "en", 2);
  writeU16(data, 48, 1);    // Spine count.
  writeU32(data, 52, 1);    // Shard count.
  writeU32(data, 56, 1);    // Shard candidate count.
  writeU32(data, 60, 1);    // Local lemma count.
  writeU32(data, 64, 1);    // Local surface count.
  writeU32(data, 68, 108);  // Spine directory.
  writeU32(data, 72, 116);  // Shard directory.
  writeU32(data, 76, 132);  // Shard candidates.
  writeU32(data, 80, 148);  // Local lemmas.
  writeU32(data, 84, 152);  // Global-to-local map.
  writeU32(data, 88, 160);  // Surface details (empty).
  writeU32(data, 92, 160);  // Metadata.
  writeU32(data, 96, static_cast<uint32_t>(data.size()));
  data[160] = 't';
  data[161] = 'e';
  data[162] = 's';
  data[163] = 't';
  refreshCrcs(data);
  return data;
}

bool parse(const std::vector<uint8_t>& data, Header& header, FormatError& error) {
  return dictionary::book_language::parseHeader(data.data(), data.size(), data.size(), header, error);
}

}  // namespace

TEST(BookLanguageFormat, ParsesValidHeaderAndPayload) {
  const std::vector<uint8_t> data = makeValidArtifact();
  Header header;
  FormatError error;

  ASSERT_TRUE(parse(data, header, error)) << dictionary::book_language::formatErrorName(error);
  EXPECT_EQ(header.spineCount, 1);
  EXPECT_EQ(header.shardCount, 1U);
  EXPECT_EQ(header.localLemmaCount, 1U);
  EXPECT_STREQ(header.sourceLanguage, "de");
  EXPECT_STREQ(header.targetLanguage, "en");
  EXPECT_TRUE(dictionary::book_language::validatePayload(data.data(), data.size(), header, error));
}

TEST(BookLanguageFormat, CrcCanBeUpdatedInChunks) {
  const std::array<uint8_t, 9> bytes = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  const uint32_t first = dictionary::book_language::updateCrc32(0, bytes.data(), 4);
  const uint32_t chunked = dictionary::book_language::updateCrc32(first, bytes.data() + 4, bytes.size() - 4);

  EXPECT_EQ(dictionary::book_language::updateCrc32(0, bytes.data(), bytes.size()), 0xCBF43926U);
  EXPECT_EQ(chunked, 0xCBF43926U);
}

TEST(BookLanguageFormat, RejectsTruncatedAndUnknownHeaders) {
  std::vector<uint8_t> data = makeValidArtifact();
  Header header;
  FormatError error;

  EXPECT_FALSE(dictionary::book_language::parseHeader(data.data(), 107, data.size(), header, error));
  EXPECT_EQ(error, FormatError::HEADER_TRUNCATED);

  data[0] = 'X';
  EXPECT_FALSE(parse(data, header, error));
  EXPECT_EQ(error, FormatError::BAD_MAGIC);
}

TEST(BookLanguageFormat, RejectsHeaderCorruption) {
  std::vector<uint8_t> data = makeValidArtifact();
  Header header;
  FormatError error;

  data[14] ^= 1U;
  EXPECT_FALSE(parse(data, header, error));
  EXPECT_EQ(error, FormatError::BAD_HEADER_CRC);
}

TEST(BookLanguageFormat, RejectsUnsupportedVersionAndFlags) {
  std::vector<uint8_t> data = makeValidArtifact();
  Header header;
  FormatError error;

  writeU16(data, 4, 2);
  refreshCrcs(data);
  EXPECT_FALSE(parse(data, header, error));
  EXPECT_EQ(error, FormatError::UNSUPPORTED_VERSION);

  data = makeValidArtifact();
  writeU32(data, 8, 1);
  refreshCrcs(data);
  EXPECT_FALSE(parse(data, header, error));
  EXPECT_EQ(error, FormatError::UNSUPPORTED_FLAGS);
}

TEST(BookLanguageFormat, RejectsInvalidIdentityAndLanguage) {
  std::vector<uint8_t> data = makeValidArtifact();
  Header header;
  FormatError error;

  std::fill(data.begin() + 16, data.begin() + 32, 0);
  refreshCrcs(data);
  EXPECT_FALSE(parse(data, header, error));
  EXPECT_EQ(error, FormatError::INVALID_DICTIONARY_UUID);

  data = makeValidArtifact();
  data[34] = '/';
  refreshCrcs(data);
  EXPECT_FALSE(parse(data, header, error));
  EXPECT_EQ(error, FormatError::INVALID_LANGUAGE);
}

TEST(BookLanguageFormat, RejectsCountsBeyondDeviceCaps) {
  std::vector<uint8_t> data = makeValidArtifact();
  Header header;
  FormatError error;

  writeU32(data, 60, dictionary::book_language::kMaxLocalLemmaCount + 1);
  refreshCrcs(data);
  EXPECT_FALSE(parse(data, header, error));
  EXPECT_EQ(error, FormatError::COUNT_OUT_OF_RANGE);
}

TEST(BookLanguageFormat, RejectsMismatchedFileSize) {
  const std::vector<uint8_t> data = makeValidArtifact();
  Header header;
  FormatError error;

  EXPECT_FALSE(dictionary::book_language::parseHeader(data.data(), data.size(), data.size() + 1, header, error));
  EXPECT_EQ(error, FormatError::FILE_SIZE_MISMATCH);
}

TEST(BookLanguageFormat, RejectsUnalignedAndOverlappingTables) {
  std::vector<uint8_t> data = makeValidArtifact();
  Header header;
  FormatError error;

  writeU32(data, 72, 117);
  refreshCrcs(data);
  EXPECT_FALSE(parse(data, header, error));
  EXPECT_EQ(error, FormatError::OFFSET_UNALIGNED);

  data = makeValidArtifact();
  writeU32(data, 72, 112);  // The 8-byte spine table starting at 108 no longer fits.
  refreshCrcs(data);
  EXPECT_FALSE(parse(data, header, error));
  EXPECT_EQ(error, FormatError::TABLE_OUT_OF_BOUNDS);
}

TEST(BookLanguageFormat, RejectsPayloadCorruption) {
  std::vector<uint8_t> data = makeValidArtifact();
  Header header;
  FormatError error;
  ASSERT_TRUE(parse(data, header, error));

  data.back() ^= 1U;
  EXPECT_FALSE(dictionary::book_language::validatePayload(data.data(), data.size(), header, error));
  EXPECT_EQ(error, FormatError::BAD_PAYLOAD_CRC);
}

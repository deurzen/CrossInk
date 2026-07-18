#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "ContextualRuntimeFormat.h"
#include "Crc32.h"

namespace {
using dictionary::RandomAccessSource;
using dictionary::contextual::RuntimeFormatError;

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

void writeU64(std::vector<uint8_t>& data, const size_t offset, const uint64_t value) {
  writeU32(data, offset, static_cast<uint32_t>(value));
  writeU32(data, offset + 4, static_cast<uint32_t>(value >> 32U));
}

bool readVector(void* context, const uint32_t offset, void* output, const size_t length) {
  const auto& data = *static_cast<const std::vector<uint8_t>*>(context);
  if (static_cast<uint64_t>(offset) + length > data.size()) return false;
  std::memcpy(output, data.data() + offset, length);
  return true;
}

RandomAccessSource sourceFor(const std::vector<uint8_t>& data) {
  return {const_cast<std::vector<uint8_t>*>(&data), data.size(), readVector};
}

struct CanonicalFixture {
  std::vector<uint8_t> meta;
  std::vector<uint8_t> lexemes;
  std::vector<uint8_t> headwords;
};

void refreshCanonicalMeta(CanonicalFixture& fixture) {
  writeU32(fixture.meta, 52, dictionary::updateCrc32(0, fixture.lexemes.data(), fixture.lexemes.size()));
  writeU32(fixture.meta, 56, dictionary::updateCrc32(0, fixture.headwords.data(), fixture.headwords.size()));
  writeU32(fixture.meta, 108, dictionary::updateCrc32(0, fixture.meta.data(), 108));
}

CanonicalFixture makeCanonicalFixture() {
  CanonicalFixture fixture;
  fixture.meta.resize(dictionary::contextual::kCanonicalMetaSize, 0);
  fixture.lexemes.resize(2 * dictionary::contextual::kCanonicalLexemeRecordSize, 0);
  fixture.headwords = {'L', 'a', 'd', 'e', 'n', 'l', 'a', 'd', 'e', 'n'};

  writeU32(fixture.lexemes, 0, 0);
  writeU64(fixture.lexemes, 4, 0x1111111111111111ULL);
  writeU16(fixture.lexemes, 12, 5);
  fixture.lexemes[14] = 1;
  writeU32(fixture.lexemes, 16, 5);
  writeU64(fixture.lexemes, 20, 0x2222222222222222ULL);
  writeU16(fixture.lexemes, 28, 5);
  fixture.lexemes[30] = 2;
  fixture.lexemes[31] = 1;

  std::memcpy(fixture.meta.data(), "CXCL", 4);
  writeU16(fixture.meta, 4, dictionary::contextual::kCanonicalFormatVersion);
  writeU16(fixture.meta, 6, dictionary::contextual::kCanonicalMetaSize);
  for (size_t i = 0; i < 16; ++i) fixture.meta[12 + i] = static_cast<uint8_t>(i + 1);
  std::memcpy(fixture.meta.data() + 28, "de", 2);
  writeU32(fixture.meta, 36, 2);
  writeU16(fixture.meta, 40, dictionary::contextual::kCanonicalLexemeRecordSize);
  writeU16(fixture.meta, 42, dictionary::contextual::kCanonicalPosVersion);
  writeU32(fixture.meta, 44, fixture.lexemes.size());
  writeU32(fixture.meta, 48, fixture.headwords.size());
  for (size_t i = 0; i < 32; ++i) fixture.meta[60 + i] = static_cast<uint8_t>(i + 1);
  refreshCanonicalMeta(fixture);
  return fixture;
}

struct DefinitionFixture {
  std::vector<uint8_t> meta;
  std::vector<uint8_t> index;
  std::vector<uint8_t> entries;
  std::array<uint8_t, 16> canonicalUuid{};
};

void refreshDefinitionMeta(DefinitionFixture& fixture) {
  writeU32(fixture.meta, 108, dictionary::updateCrc32(0, fixture.index.data(), fixture.index.size()));
  writeU32(fixture.meta, 112, dictionary::updateCrc32(0, fixture.entries.data(), fixture.entries.size()));
  writeU32(fixture.meta, 140, dictionary::updateCrc32(0, fixture.meta.data(), 140));
}

DefinitionFixture makeDefinitionFixture() {
  DefinitionFixture fixture;
  fixture.meta.resize(dictionary::contextual::kDefinitionMetaSize, 0);
  fixture.index.resize(3 * dictionary::contextual::kDefinitionIndexRecordSize, 0);
  fixture.entries = {1, 0, 1, 0, 1, 0, 1, 0};
  for (size_t i = 0; i < fixture.canonicalUuid.size(); ++i) {
    fixture.canonicalUuid[i] = static_cast<uint8_t>(i + 1);
  }

  writeU32(fixture.index, 0, 0);
  writeU32(fixture.index, 4, 4);
  writeU32(fixture.index, 16, 4);
  writeU32(fixture.index, 20, 4);

  std::memcpy(fixture.meta.data(), "CXDS", 4);
  writeU16(fixture.meta, 4, dictionary::contextual::kDefinitionFormatVersion);
  writeU16(fixture.meta, 6, dictionary::contextual::kDefinitionMetaSize);
  for (size_t i = 0; i < 16; ++i) fixture.meta[12 + i] = static_cast<uint8_t>(0xA0U + i);
  std::memcpy(fixture.meta.data() + 28, fixture.canonicalUuid.data(), fixture.canonicalUuid.size());
  std::memcpy(fixture.meta.data() + 44, "de", 2);
  std::memcpy(fixture.meta.data() + 52, "en", 2);
  std::memcpy(fixture.meta.data() + 60, "Fixture", 7);
  writeU32(fixture.meta, 92, 3);
  writeU16(fixture.meta, 96, dictionary::contextual::kDefinitionIndexRecordSize);
  writeU16(fixture.meta, 98, dictionary::contextual::kDefinitionEntryVersion);
  writeU32(fixture.meta, 100, fixture.index.size());
  writeU32(fixture.meta, 104, fixture.entries.size());
  writeU32(fixture.meta, 116, 2);
  refreshDefinitionMeta(fixture);
  return fixture;
}

bool openCanonical(const CanonicalFixture& fixture, dictionary::contextual::CanonicalLexiconReader& reader,
                   RuntimeFormatError& error) {
  return reader.open(sourceFor(fixture.meta), sourceFor(fixture.lexemes), sourceFor(fixture.headwords), error);
}

bool openDefinition(const DefinitionFixture& fixture, dictionary::contextual::DefinitionSourceReader& reader,
                    RuntimeFormatError& error) {
  return reader.open(sourceFor(fixture.meta), sourceFor(fixture.index), sourceFor(fixture.entries),
                     fixture.canonicalUuid.data(), 3, error);
}

}  // namespace

TEST(CanonicalLexiconReader, ReadsDenseIdentityWithoutAllocation) {
  const CanonicalFixture fixture = makeCanonicalFixture();
  dictionary::contextual::CanonicalLexiconReader reader;
  RuntimeFormatError error;
  ASSERT_TRUE(openCanonical(fixture, reader, error)) << dictionary::contextual::runtimeFormatErrorName(error);
  EXPECT_EQ(reader.metadata().lexemeCount, 2U);
  std::array<uint8_t, 31> scratch{};
  ASSERT_TRUE(reader.validateLexemes(scratch.data(), scratch.size(), error));

  dictionary::contextual::CanonicalLexemeRecord lexeme;
  ASSERT_TRUE(reader.readLexeme(1, lexeme, error));
  EXPECT_EQ(lexeme.partOfSpeech, 2);
  EXPECT_EQ(lexeme.flags, 1);
  std::array<char, 8> headword{};
  size_t length = 0;
  ASSERT_TRUE(reader.readHeadword(lexeme, headword.data(), headword.size(), length, error));
  EXPECT_EQ(std::string_view(headword.data(), length), "laden");
}

TEST(CanonicalLexiconReader, RejectsHeaderAndRecordCorruptionBeforeOutOfRangeReads) {
  CanonicalFixture fixture = makeCanonicalFixture();
  dictionary::contextual::CanonicalLexiconReader reader;
  RuntimeFormatError error;

  fixture.meta.resize(dictionary::contextual::kCanonicalMetaSize - 1);
  EXPECT_FALSE(openCanonical(fixture, reader, error));
  EXPECT_EQ(error, RuntimeFormatError::META_TRUNCATED);

  fixture = makeCanonicalFixture();
  fixture.meta[12] ^= 1U;
  EXPECT_FALSE(openCanonical(fixture, reader, error));
  EXPECT_EQ(error, RuntimeFormatError::BAD_META_CRC);

  fixture = makeCanonicalFixture();
  fixture.headwords.push_back('x');
  EXPECT_FALSE(openCanonical(fixture, reader, error));
  EXPECT_EQ(error, RuntimeFormatError::FILE_SIZE_MISMATCH);

  fixture = makeCanonicalFixture();
  ASSERT_TRUE(openCanonical(fixture, reader, error));
  dictionary::contextual::CanonicalLexemeRecord lexeme;
  EXPECT_FALSE(reader.readLexeme(2, lexeme, error));
  EXPECT_EQ(error, RuntimeFormatError::RECORD_ID_OUT_OF_RANGE);

  fixture.lexemes[15] = 0x80U;
  refreshCanonicalMeta(fixture);
  ASSERT_TRUE(openCanonical(fixture, reader, error));
  std::array<uint8_t, 17> scratch{};
  EXPECT_FALSE(reader.validateLexemes(scratch.data(), scratch.size(), error));
  EXPECT_EQ(error, RuntimeFormatError::RECORD_INVALID);
}

TEST(DefinitionSourceReader, ReadsFixedMissingAndPresentIndexRecords) {
  const DefinitionFixture fixture = makeDefinitionFixture();
  dictionary::contextual::DefinitionSourceReader reader;
  RuntimeFormatError error;
  ASSERT_TRUE(openDefinition(fixture, reader, error)) << dictionary::contextual::runtimeFormatErrorName(error);
  EXPECT_EQ(std::string_view(reader.metadata().sourceLabel), "Fixture");

  dictionary::contextual::DefinitionIndexRecord record;
  ASSERT_TRUE(reader.readIndex(0, record, error));
  EXPECT_TRUE(record.present());
  EXPECT_EQ(record.entryLength, 4U);
  ASSERT_TRUE(reader.readIndex(1, record, error));
  EXPECT_FALSE(record.present());
  std::array<uint8_t, 13> scratch{};
  ASSERT_TRUE(reader.validateIndex(scratch.data(), scratch.size(), error));
  EXPECT_FALSE(reader.validateIndex(scratch.data(), 7, error));
  EXPECT_EQ(error, RuntimeFormatError::OUTPUT_BUFFER_TOO_SMALL);
}

TEST(DefinitionSourceReader, RejectsIdentityCountLabelAndSourceSizeMismatch) {
  DefinitionFixture fixture = makeDefinitionFixture();
  dictionary::contextual::DefinitionSourceReader reader;
  RuntimeFormatError error;

  auto wrongUuid = fixture.canonicalUuid;
  wrongUuid[0] ^= 1U;
  EXPECT_FALSE(reader.open(sourceFor(fixture.meta), sourceFor(fixture.index), sourceFor(fixture.entries),
                           wrongUuid.data(), 3, error));
  EXPECT_EQ(error, RuntimeFormatError::CANONICAL_MISMATCH);

  EXPECT_FALSE(reader.open(sourceFor(fixture.meta), sourceFor(fixture.index), sourceFor(fixture.entries),
                           fixture.canonicalUuid.data(), 2, error));
  EXPECT_EQ(error, RuntimeFormatError::CANONICAL_MISMATCH);

  fixture = makeDefinitionFixture();
  fixture.meta[60] = 0xFFU;
  refreshDefinitionMeta(fixture);
  EXPECT_FALSE(openDefinition(fixture, reader, error));
  EXPECT_EQ(error, RuntimeFormatError::INVALID_LABEL);

  fixture = makeDefinitionFixture();
  fixture.entries.push_back(0);
  EXPECT_FALSE(openDefinition(fixture, reader, error));
  EXPECT_EQ(error, RuntimeFormatError::FILE_SIZE_MISMATCH);
}

TEST(DefinitionSourceReader, RejectsMalformedOverlapAndCoverageMismatch) {
  DefinitionFixture fixture = makeDefinitionFixture();
  dictionary::contextual::DefinitionSourceReader reader;
  RuntimeFormatError error;
  ASSERT_TRUE(openDefinition(fixture, reader, error));

  writeU32(fixture.index, 8, 1);
  refreshDefinitionMeta(fixture);
  ASSERT_TRUE(openDefinition(fixture, reader, error));
  dictionary::contextual::DefinitionIndexRecord record;
  EXPECT_FALSE(reader.readIndex(1, record, error));
  EXPECT_EQ(error, RuntimeFormatError::RECORD_INVALID);

  fixture = makeDefinitionFixture();
  writeU32(fixture.index, 16, 2);
  refreshDefinitionMeta(fixture);
  ASSERT_TRUE(openDefinition(fixture, reader, error));
  std::array<uint8_t, 13> scratch{};
  EXPECT_FALSE(reader.validateIndex(scratch.data(), scratch.size(), error));
  EXPECT_EQ(error, RuntimeFormatError::INDEX_ORDER_INVALID);

  fixture = makeDefinitionFixture();
  writeU32(fixture.meta, 116, 1);
  refreshDefinitionMeta(fixture);
  ASSERT_TRUE(openDefinition(fixture, reader, error));
  EXPECT_FALSE(reader.validateIndex(scratch.data(), scratch.size(), error));
  EXPECT_EQ(error, RuntimeFormatError::COVERAGE_MISMATCH);
}

TEST(ContextualRuntimeFormat, RejectsPayloadCrcCorruptionWithBoundedScratch) {
  DefinitionFixture fixture = makeDefinitionFixture();
  dictionary::contextual::DefinitionSourceReader reader;
  RuntimeFormatError error;
  ASSERT_TRUE(openDefinition(fixture, reader, error));

  std::array<uint8_t, 3> scratch{};
  EXPECT_TRUE(reader.validatePayloadCrc(scratch.data(), scratch.size(), error));

  fixture.index[14] ^= 0x80U;
  ASSERT_TRUE(openDefinition(fixture, reader, error));
  EXPECT_FALSE(reader.validatePayloadCrc(scratch.data(), scratch.size(), error));
  EXPECT_EQ(error, RuntimeFormatError::BAD_FILE_CRC);
}

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Crc32.h"
#include "DefinitionPager.h"
#include "DictionaryPackage.h"

namespace {
using dictionary::DictionaryPackage;
using dictionary::PackageError;
using dictionary::RandomAccessSource;

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

bool readEntryVector(void* context, const dictionary::EntrySlice& entry, const uint32_t relativeOffset, void* output,
                     const size_t capacity, size_t& bytesRead, PackageError& error) {
  bytesRead = 0;
  error = PackageError::NONE;
  const auto& data = *static_cast<const std::vector<uint8_t>*>(context);
  if (entry.length == 0 || static_cast<uint64_t>(entry.offset) + entry.length > data.size() ||
      relativeOffset > entry.length) {
    error = PackageError::ENTRY_RANGE_INVALID;
    return false;
  }
  if (capacity == 0 || relativeOffset == entry.length) return true;
  if (!output) {
    error = PackageError::OUTPUT_BUFFER_TOO_SMALL;
    return false;
  }
  bytesRead = std::min(capacity, static_cast<size_t>(entry.length - relativeOffset));
  std::memcpy(output, data.data() + entry.offset + relativeOffset, bytesRead);
  return true;
}

struct Fixture {
  std::vector<uint8_t> meta;
  std::vector<uint8_t> lexemes;
  std::vector<uint8_t> headwords;
  std::vector<uint8_t> entries;
};

void writeLexeme(std::vector<uint8_t>& data, const size_t offset, const uint32_t headwordOffset,
                 const uint16_t headwordLength, const uint32_t entryOffset, const uint32_t entryLength,
                 const uint64_t keyHash, const uint8_t partOfSpeech, const uint8_t flags = 0) {
  writeU32(data, offset, headwordOffset);
  writeU32(data, offset + 4, entryOffset);
  writeU32(data, offset + 8, entryLength);
  writeU64(data, offset + 12, keyHash);
  writeU16(data, offset + 20, headwordLength);
  data[offset + 22] = partOfSpeech;
  data[offset + 23] = flags;
}

void refreshMetaCrc(Fixture& fixture) {
  writeU32(fixture.meta, 64, dictionary::updateCrc32(0, fixture.lexemes.data(), fixture.lexemes.size()));
  writeU32(fixture.meta, 68, dictionary::updateCrc32(0, fixture.headwords.data(), fixture.headwords.size()));
  writeU32(fixture.meta, 72, dictionary::updateCrc32(0, fixture.entries.data(), fixture.entries.size()));
  writeU32(fixture.meta, 76, dictionary::updateCrc32(0, fixture.meta.data(), 76));
}

Fixture makeFixture() {
  Fixture fixture;
  fixture.meta.resize(dictionary::kDictionaryMetaSize, 0);
  fixture.lexemes.resize(2 * dictionary::kLexemeRecordSize, 0);
  fixture.headwords = {'H', 'a', 'u', 's', 'g', 'e', 'h', 'e', 'n'};
  fixture.entries = {
      1,   0,   1,   0,                     // Entry version, flags, one field.
      1,   0,   5,   0,                     // Definition field header.
      'h', 'o', 'u', 's', 'e', 1, 0, 1, 0,  // Entry version, flags, one field.
      1,   0,   5,   0,                     // Definition field header.
      't', 'o', ' ', 'g', 'o',
  };

  writeLexeme(fixture.lexemes, 0, 0, 4, 0, 13, 0x1111111111111111ULL, 1);
  writeLexeme(fixture.lexemes, dictionary::kLexemeRecordSize, 4, 5, 13, 13, 0x2222222222222222ULL, 2, 1);

  std::memcpy(fixture.meta.data(), "CXDM", 4);
  writeU16(fixture.meta, 4, dictionary::kDictionaryPackageVersion);
  writeU16(fixture.meta, 6, dictionary::kDictionaryMetaSize);
  for (size_t i = 0; i < 16; ++i) fixture.meta[12 + i] = static_cast<uint8_t>(i + 1);
  std::memcpy(fixture.meta.data() + 28, "de", 2);
  std::memcpy(fixture.meta.data() + 36, "en", 2);
  writeU32(fixture.meta, 44, 2);
  writeU16(fixture.meta, 48, dictionary::kLexemeRecordSize);
  writeU32(fixture.meta, 52, fixture.lexemes.size());
  writeU32(fixture.meta, 56, fixture.headwords.size());
  writeU32(fixture.meta, 60, fixture.entries.size());
  refreshMetaCrc(fixture);
  return fixture;
}

bool openFixture(const Fixture& fixture, DictionaryPackage& package, PackageError& error) {
  return package.open(sourceFor(fixture.meta), sourceFor(fixture.lexemes), sourceFor(fixture.headwords),
                      sourceFor(fixture.entries), error);
}

void replaceFirstEntry(Fixture& fixture, const std::vector<std::pair<uint8_t, std::string_view>>& fields) {
  fixture.entries.assign({1, 0, static_cast<uint8_t>(fields.size()), 0});
  for (const auto& [type, text] : fields) {
    fixture.entries.push_back(type);
    fixture.entries.push_back(0);
    fixture.entries.push_back(static_cast<uint8_t>(text.size()));
    fixture.entries.push_back(static_cast<uint8_t>(text.size() >> 8U));
    fixture.entries.insert(fixture.entries.end(), text.begin(), text.end());
  }
  writeU32(fixture.lexemes, 8, fixture.entries.size());
  fixture.lexemes.resize(dictionary::kLexemeRecordSize);
  writeU32(fixture.meta, 44, 1);
  writeU32(fixture.meta, 52, fixture.lexemes.size());
  writeU32(fixture.meta, 60, fixture.entries.size());
  refreshMetaCrc(fixture);
}

int measuredBytes(void*, const std::string_view text) { return static_cast<int>(text.size()); }

}  // namespace

TEST(DictionaryPackage, ResolvesDenseLexemeIdToHeadwordAndEntry) {
  const Fixture fixture = makeFixture();
  DictionaryPackage package;
  PackageError error;
  ASSERT_TRUE(openFixture(fixture, package, error)) << dictionary::packageErrorName(error);

  dictionary::LexemeRecord lexeme;
  ASSERT_TRUE(package.readLexeme(1, lexeme, error));
  EXPECT_EQ(lexeme.lexemeKeyHash, 0x2222222222222222ULL);
  EXPECT_EQ(lexeme.partOfSpeech, 2);
  EXPECT_EQ(lexeme.flags, 1);

  std::array<char, 16> headword{};
  size_t headwordLength = 0;
  ASSERT_TRUE(package.readHeadword(lexeme, headword.data(), headword.size(), headwordLength, error));
  EXPECT_EQ(std::string_view(headword.data(), headwordLength), "gehen");

  dictionary::EntrySlice entry;
  ASSERT_TRUE(package.getEntrySlice(lexeme, entry, error));
  dictionary::EntryHeader entryHeader;
  ASSERT_TRUE(package.readEntryHeader(entry, entryHeader, error));
  EXPECT_EQ(entryHeader.fieldCount, 1);

  dictionary::EntryFieldHeader field;
  ASSERT_TRUE(package.readEntryFieldHeader(entry, 4, field, error));
  EXPECT_EQ(field.type, 1);
  std::array<char, 8> chunk{};
  size_t bytesRead = 0;
  ASSERT_TRUE(package.readEntryChunk(entry, field.payloadOffset, chunk.data(), field.length, bytesRead, error));
  EXPECT_EQ(std::string_view(chunk.data(), bytesRead), "to go");
}

TEST(DictionaryPackage, RejectsTruncatedAndCorruptMetadata) {
  Fixture fixture = makeFixture();
  DictionaryPackage package;
  PackageError error;

  fixture.meta.resize(dictionary::kDictionaryMetaSize - 1);
  EXPECT_FALSE(openFixture(fixture, package, error));
  EXPECT_EQ(error, PackageError::META_TRUNCATED);

  fixture = makeFixture();
  fixture.meta[12] ^= 1U;
  EXPECT_FALSE(openFixture(fixture, package, error));
  EXPECT_EQ(error, PackageError::BAD_META_CRC);
}

TEST(DictionaryPackage, RejectsSourceSizeMismatch) {
  Fixture fixture = makeFixture();
  fixture.headwords.push_back('x');
  DictionaryPackage package;
  PackageError error;

  EXPECT_FALSE(openFixture(fixture, package, error));
  EXPECT_EQ(error, PackageError::FILE_SIZE_MISMATCH);
}

TEST(DictionaryPackage, RejectsOutOfRangeLexemeIdsAndRecords) {
  Fixture fixture = makeFixture();
  DictionaryPackage package;
  PackageError error;
  ASSERT_TRUE(openFixture(fixture, package, error));

  dictionary::LexemeRecord lexeme;
  EXPECT_FALSE(package.readLexeme(2, lexeme, error));
  EXPECT_EQ(error, PackageError::LEXEME_ID_OUT_OF_RANGE);

  writeU32(fixture.lexemes, 0, static_cast<uint32_t>(fixture.headwords.size()));
  refreshMetaCrc(fixture);
  ASSERT_TRUE(openFixture(fixture, package, error));
  EXPECT_FALSE(package.readLexeme(0, lexeme, error));
  EXPECT_EQ(error, PackageError::LEXEME_RECORD_INVALID);
}

TEST(DictionaryPackage, RequiresCallerOwnedHeadwordCapacity) {
  const Fixture fixture = makeFixture();
  DictionaryPackage package;
  PackageError error;
  ASSERT_TRUE(openFixture(fixture, package, error));

  dictionary::LexemeRecord lexeme;
  ASSERT_TRUE(package.readLexeme(0, lexeme, error));
  std::array<char, 4> tooSmall{};
  size_t outputLength = 0;
  EXPECT_FALSE(package.readHeadword(lexeme, tooSmall.data(), tooSmall.size(), outputLength, error));
  EXPECT_EQ(error, PackageError::OUTPUT_BUFFER_TOO_SMALL);
}

TEST(DictionaryPackage, RejectsEntryReadsOutsideValidatedSlice) {
  const Fixture fixture = makeFixture();
  DictionaryPackage package;
  PackageError error;
  ASSERT_TRUE(openFixture(fixture, package, error));

  dictionary::EntrySlice entry{13, 13};
  std::array<char, 8> output{};
  size_t bytesRead = 0;
  EXPECT_FALSE(package.readEntryChunk(entry, 14, output.data(), output.size(), bytesRead, error));
  EXPECT_EQ(error, PackageError::ENTRY_RANGE_INVALID);
}

TEST(DictionaryPackage, RejectsMalformedEntryHeadersAndFields) {
  Fixture fixture = makeFixture();
  DictionaryPackage package;
  PackageError error;
  ASSERT_TRUE(openFixture(fixture, package, error));

  dictionary::EntrySlice entry{0, 13};
  dictionary::EntryHeader entryHeader;
  fixture.entries[0] = 2;
  EXPECT_FALSE(package.readEntryHeader(entry, entryHeader, error));
  EXPECT_EQ(error, PackageError::ENTRY_HEADER_INVALID);

  fixture.entries[0] = 1;
  fixture.entries[6] = 20;  // Field payload extends past the 13-byte entry.
  dictionary::EntryFieldHeader field;
  EXPECT_FALSE(package.readEntryFieldHeader(entry, 4, field, error));
  EXPECT_EQ(error, PackageError::ENTRY_FIELD_INVALID);
}

TEST(DictionaryPackage, ValidatesFilesWithBoundedReusableScratch) {
  Fixture fixture = makeFixture();
  std::array<uint8_t, 3> scratch{};
  PackageError error;

  const uint32_t expected = dictionary::updateCrc32(0, fixture.entries.data(), fixture.entries.size());
  EXPECT_TRUE(
      dictionary::validateSourceCrc(sourceFor(fixture.entries), expected, scratch.data(), scratch.size(), error));

  fixture.entries.back() ^= 1U;
  EXPECT_FALSE(
      dictionary::validateSourceCrc(sourceFor(fixture.entries), expected, scratch.data(), scratch.size(), error));
  EXPECT_EQ(error, PackageError::BAD_FILE_CRC);
}

TEST(DefinitionPager, StreamsWrappedPagesAcrossEntryFields) {
  Fixture fixture = makeFixture();
  replaceFirstEntry(fixture, {{2, "verb"}, {1, "one two three four five six"}, {3, "an example"}});
  DictionaryPackage package;
  PackageError packageError;
  ASSERT_TRUE(openFixture(fixture, package, packageError));

  dictionary::LexemeRecord lexeme;
  dictionary::EntrySlice entry;
  ASSERT_TRUE(package.readLexeme(0, lexeme, packageError));
  ASSERT_TRUE(package.getEntrySlice(lexeme, entry, packageError));

  dictionary::definition::Pager pager;
  dictionary::definition::Page page;
  dictionary::definition::PagerError error;
  const dictionary::definition::WidthMeasurer measurer{nullptr, measuredBytes};
  ASSERT_TRUE(pager.load(package, entry, {}, measurer, 7, dictionary::definition::kMaxPageLines, page, error))
      << dictionary::definition::pagerErrorName(error);

  ASSERT_GE(page.lineCount, 6);
  EXPECT_EQ(page.lineText(0), "verb");
  EXPECT_EQ(page.lines[0].fieldType, 2);
  EXPECT_TRUE(page.lines[0].fieldStart);
  EXPECT_EQ(page.lineText(1), "one two");
  EXPECT_EQ(page.lines[1].fieldType, 1);
  EXPECT_TRUE(page.lines[1].fieldStart);
  EXPECT_TRUE(page.lines[1].gapBefore);
  EXPECT_FALSE(page.lines[2].gapBefore);
  EXPECT_EQ(page.lineText(page.lineCount - 1), "example");
  EXPECT_FALSE(page.hasNext);
}

TEST(DefinitionPager, StreamsFromGenericEntryReaderWithoutMaterializingPayload) {
  Fixture fixture = makeFixture();
  replaceFirstEntry(fixture, {{2, "verb"}, {1, "contextual definition"}});
  const dictionary::definition::EntryReader reader{&fixture.entries, readEntryVector};
  const dictionary::EntrySlice entry{0, static_cast<uint32_t>(fixture.entries.size())};
  dictionary::definition::Pager pager;
  dictionary::definition::Page page;
  dictionary::definition::PagerError error;
  const dictionary::definition::WidthMeasurer measurer{nullptr, measuredBytes};

  ASSERT_TRUE(pager.load(reader, entry, {}, measurer, 40, dictionary::definition::kMaxPageLines, page, error));
  ASSERT_EQ(page.lineCount, 2);
  EXPECT_EQ(page.lineText(0), "verb");
  EXPECT_EQ(page.lineText(1), "contextual definition");
  EXPECT_FALSE(page.hasNext);
}

TEST(DefinitionPager, AppendsEntriesWithoutASecondPageBuffer) {
  Fixture fixture = makeFixture();
  replaceFirstEntry(fixture, {{1, "first meaning"}});
  DictionaryPackage package;
  PackageError packageError;
  ASSERT_TRUE(openFixture(fixture, package, packageError));

  dictionary::LexemeRecord lexeme;
  dictionary::EntrySlice entry;
  ASSERT_TRUE(package.readLexeme(0, lexeme, packageError));
  ASSERT_TRUE(package.getEntrySlice(lexeme, entry, packageError));

  dictionary::definition::Pager pager;
  dictionary::definition::Page page;
  dictionary::definition::PagerError error;
  const dictionary::definition::WidthMeasurer measurer{nullptr, measuredBytes};
  ASSERT_TRUE(pager.load(package, entry, {}, measurer, 40, dictionary::definition::kMaxPageLines, page, error));
  const uint8_t firstLineCount = page.lineCount;
  ASSERT_TRUE(pager.append(package, entry, {}, measurer, 40, dictionary::definition::kMaxPageLines, page, error));

  ASSERT_EQ(page.lineCount, firstLineCount * 2);
  EXPECT_TRUE(page.lines[firstLineCount].gapBefore);
  EXPECT_EQ(page.lineText(firstLineCount), "first meaning");
}

TEST(DefinitionPager, ReturnsBoundedContinuationCursorWithoutMaterializingEntry) {
  Fixture fixture = makeFixture();
  std::string text;
  for (int i = 0; i < 900; ++i) text += "word ";
  replaceFirstEntry(fixture, {{1, text}});
  DictionaryPackage package;
  PackageError packageError;
  ASSERT_TRUE(openFixture(fixture, package, packageError));

  dictionary::LexemeRecord lexeme;
  dictionary::EntrySlice entry;
  ASSERT_TRUE(package.readLexeme(0, lexeme, packageError));
  ASSERT_TRUE(package.getEntrySlice(lexeme, entry, packageError));

  dictionary::definition::Pager pager;
  dictionary::definition::Page first;
  dictionary::definition::PagerError error;
  const dictionary::definition::WidthMeasurer measurer{nullptr, measuredBytes};
  ASSERT_TRUE(pager.load(package, entry, {}, measurer, 20, dictionary::definition::kMaxPageLines, first, error));
  ASSERT_TRUE(first.hasNext);
  EXPECT_LE(first.textBytesUsed, dictionary::definition::kPageTextBytes);
  EXPECT_LE(first.lineCount, dictionary::definition::kMaxPageLines);

  dictionary::definition::Page second;
  ASSERT_TRUE(
      pager.load(package, entry, first.next, measurer, 20, dictionary::definition::kMaxPageLines, second, error));
  EXPECT_GT(second.lineCount, 0);
  EXPECT_GT(second.next.fieldByteOffset, first.next.fieldByteOffset);
}

TEST(DefinitionPager, RejectsInvalidCursorAndUtf8) {
  Fixture fixture = makeFixture();
  replaceFirstEntry(fixture, {{1, std::string_view("bad\xC3", 4)}});
  DictionaryPackage package;
  PackageError packageError;
  ASSERT_TRUE(openFixture(fixture, package, packageError));

  dictionary::LexemeRecord lexeme;
  dictionary::EntrySlice entry;
  ASSERT_TRUE(package.readLexeme(0, lexeme, packageError));
  ASSERT_TRUE(package.getEntrySlice(lexeme, entry, packageError));

  dictionary::definition::Pager pager;
  dictionary::definition::Page page;
  dictionary::definition::PagerError error;
  const dictionary::definition::WidthMeasurer measurer{nullptr, measuredBytes};
  dictionary::definition::Cursor invalid;
  invalid.fieldIndex = 2;
  EXPECT_FALSE(pager.load(package, entry, invalid, measurer, 20, dictionary::definition::kMaxPageLines, page, error));
  EXPECT_EQ(error, dictionary::definition::PagerError::CURSOR_INVALID);

  EXPECT_FALSE(pager.load(package, entry, {}, measurer, 20, dictionary::definition::kMaxPageLines, page, error));
  EXPECT_EQ(error, dictionary::definition::PagerError::INVALID_UTF8);
}

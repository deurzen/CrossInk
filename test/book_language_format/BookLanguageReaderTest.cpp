#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <string_view>
#include <utility>
#include <vector>

#include "BookLanguageReader.h"
#include "PageShortlist.h"

namespace {
using dictionary::book_language::BookLanguageReader;
using dictionary::book_language::RandomAccessSource;
using dictionary::book_language::ReaderError;

void writeU16(std::vector<uint8_t>& data, size_t offset, uint16_t value) {
  data[offset] = value;
  data[offset + 1] = value >> 8U;
}
void writeU32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
  data[offset] = value;
  data[offset + 1] = value >> 8U;
  data[offset + 2] = value >> 16U;
  data[offset + 3] = value >> 24U;
}
void writeU64(std::vector<uint8_t>& data, size_t offset, uint64_t value) {
  writeU32(data, offset, value);
  writeU32(data, offset + 4, value >> 32U);
}
void refreshCrc(std::vector<uint8_t>& data) {
  writeU32(data, 100, dictionary::book_language::updateCrc32(0, data.data() + 108, data.size() - 108));
  writeU32(data, 104, dictionary::book_language::updateCrc32(0, data.data(), 104));
}

std::vector<uint8_t> makeArtifact() {
  constexpr size_t spineOffset = 108;
  constexpr size_t shardOffset = 116;
  constexpr size_t blobOffset = 136;
  constexpr size_t lemmaOffset = 168;
  constexpr size_t metadataOffset = 176;
  std::vector<uint8_t> data(180, 0);
  std::memcpy(data.data(), "CXLG", 4);
  writeU16(data, 4, dictionary::book_language::kFormatVersion);
  writeU16(data, 6, 108);
  writeU16(data, 12, 1);
  writeU16(data, 14, 1);
  for (size_t i = 0; i < 16; ++i) data[16 + i] = i + 1;
  std::memcpy(data.data() + 32, "de", 2);
  std::memcpy(data.data() + 40, "und", 3);
  writeU16(data, 48, 1);
  writeU32(data, 52, 1);
  writeU32(data, 56, 1);
  writeU32(data, 60, 2);
  writeU32(data, 68, spineOffset);
  writeU32(data, 72, shardOffset);
  writeU32(data, 76, blobOffset);
  writeU32(data, 80, lemmaOffset);
  writeU32(data, 84, metadataOffset);
  writeU32(data, 96, data.size());

  writeU32(data, spineOffset, 0);
  writeU32(data, spineOffset + 4, 1);
  writeU32(data, shardOffset, 0);
  writeU16(data, shardOffset + 4, 32);
  writeU16(data, shardOffset + 6, 1);
  writeU32(data, shardOffset + 8, 0);
  writeU32(data, shardOffset + 12, 64);

  const std::string_view surface = "liebe";
  writeU64(data, blobOffset, dictionary::book_language::fnv1a64(surface));
  writeU16(data, blobOffset + 8, 32);
  data[blobOffset + 10] = surface.size();
  data[blobOffset + 11] = 2;
  data[blobOffset + 12] = 1;
  data[blobOffset + 13] = 200;
  writeU16(data, blobOffset + 14, 900);
  writeU32(data, blobOffset + 16, 0x0000DA80);  // Finite past indicative third-person singular.
  writeU16(data, blobOffset + 20, 0);
  writeU16(data, blobOffset + 22, 1);
  std::memcpy(data.data() + blobOffset + 24, surface.data(), surface.size());
  writeU32(data, lemmaOffset, 10);
  writeU32(data, lemmaOffset + 4, 20);
  std::memcpy(data.data() + metadataOffset, "meta", 4);
  refreshCrc(data);
  return data;
}

std::vector<uint8_t> makeRepeatedSurfaceArtifact(
    const std::vector<std::pair<uint32_t, uint16_t>>& grammarAndPrimaryIds) {
  constexpr size_t spineOffset = 108;
  constexpr size_t shardOffset = 116;
  constexpr size_t shardRecordSize = 20;
  constexpr size_t candidateRecordSize = 28;
  const size_t blobOffset = shardOffset + grammarAndPrimaryIds.size() * shardRecordSize;
  const size_t lemmaOffset = blobOffset + grammarAndPrimaryIds.size() * candidateRecordSize;
  const size_t metadataOffset = lemmaOffset + 8;
  std::vector<uint8_t> data(metadataOffset + 4, 0);
  std::memcpy(data.data(), "CXLG", 4);
  writeU16(data, 4, dictionary::book_language::kFormatVersion);
  writeU16(data, 6, 108);
  writeU16(data, 12, 1);
  writeU16(data, 14, 1);
  for (size_t index = 0; index < 16; ++index) data[16 + index] = index + 1;
  std::memcpy(data.data() + 32, "de", 2);
  std::memcpy(data.data() + 40, "und", 3);
  writeU16(data, 48, 1);
  writeU32(data, 52, grammarAndPrimaryIds.size());
  writeU32(data, 56, grammarAndPrimaryIds.size());
  writeU32(data, 60, 2);
  writeU32(data, 68, spineOffset);
  writeU32(data, 72, shardOffset);
  writeU32(data, 76, blobOffset);
  writeU32(data, 80, lemmaOffset);
  writeU32(data, 84, metadataOffset);
  writeU32(data, 96, data.size());
  writeU32(data, spineOffset, 0);
  writeU32(data, spineOffset + 4, grammarAndPrimaryIds.size());

  constexpr std::string_view surface = "liebe";
  for (size_t index = 0; index < grammarAndPrimaryIds.size(); ++index) {
    const size_t shard = shardOffset + index * shardRecordSize;
    writeU32(data, shard, index * candidateRecordSize);
    writeU16(data, shard + 4, candidateRecordSize);
    writeU16(data, shard + 6, 1);
    writeU32(data, shard + 8, index * 64);
    writeU32(data, shard + 12, (index + 1) * 64);

    const size_t candidate = blobOffset + index * candidateRecordSize;
    writeU64(data, candidate, dictionary::book_language::fnv1a64(surface));
    writeU16(data, candidate + 8, candidateRecordSize);
    data[candidate + 10] = surface.size();
    data[candidate + 11] = 1;
    data[candidate + 12] = 2;
    writeU16(data, candidate + 14, 900);
    writeU32(data, candidate + 16, grammarAndPrimaryIds[index].first);
    writeU16(data, candidate + 20, grammarAndPrimaryIds[index].second);
    std::memcpy(data.data() + candidate + 22, surface.data(), surface.size());
  }
  writeU32(data, lemmaOffset, 10);
  writeU32(data, lemmaOffset + 4, 20);
  std::memcpy(data.data() + metadataOffset, "meta", 4);
  refreshCrc(data);
  return data;
}

bool readMemory(void* context, uint32_t offset, void* output, size_t length) {
  const auto& data = *static_cast<const std::vector<uint8_t>*>(context);
  if (static_cast<uint64_t>(offset) + length > data.size()) return false;
  std::memcpy(output, data.data() + offset, length);
  return true;
}
RandomAccessSource sourceFor(const std::vector<uint8_t>& data) {
  return {const_cast<std::vector<uint8_t>*>(&data), data.size(), readMemory};
}
struct CountingSource {
  std::vector<uint8_t> data;
  int reads = 0;
};
bool readCounting(void* context, uint32_t offset, void* output, size_t length) {
  auto& source = *static_cast<CountingSource*>(context);
  if (static_cast<uint64_t>(offset) + length > source.data.size()) return false;
  ++source.reads;
  std::memcpy(output, source.data.data() + offset, length);
  return true;
}
}  // namespace

TEST(BookLanguageReader, ReadsInlineCandidateAndLocalLemmas) {
  const auto data = makeArtifact();
  BookLanguageReader reader;
  ReaderError error;
  ASSERT_TRUE(reader.open(sourceFor(data), error));
  dictionary::book_language::ShardDirectoryRecord shard;
  ASSERT_TRUE(reader.readShard(0, shard, error));
  EXPECT_EQ(shard.recordCount, 1);
  const dictionary::book_language::InlineCandidate* candidate = nullptr;
  ASSERT_TRUE(reader.readCandidate(shard, 0, candidate, error));
  ASSERT_NE(candidate, nullptr);
  EXPECT_EQ(std::string_view(candidate->surface, candidate->surfaceLength), "liebe");
  EXPECT_EQ(candidate->analysisCount, 2);
  EXPECT_EQ(candidate->difficulty, 200);
  EXPECT_EQ(candidate->grammarDescriptor, 0x0000DA80U);
  EXPECT_EQ(candidate->localLemmaIds[0], 0);
  EXPECT_EQ(candidate->localLemmaIds[1], 1);
  uint32_t global = 0;
  ASSERT_TRUE(reader.readGlobalLexemeId(1, global, error));
  EXPECT_EQ(global, 20U);
}

TEST(BookLanguageReader, CachesShardBlobAndLemmaReads) {
  CountingSource source{makeArtifact()};
  BookLanguageReader reader;
  ReaderError error;
  ASSERT_TRUE(reader.open({&source, source.data.size(), readCounting}, error));
  source.reads = 0;
  dictionary::book_language::ShardDirectoryRecord shard;
  ASSERT_TRUE(reader.readShard(0, shard, error));
  ASSERT_TRUE(reader.readShard(0, shard, error));
  EXPECT_EQ(source.reads, 1);
  const dictionary::book_language::InlineCandidate* candidate = nullptr;
  source.reads = 0;
  ASSERT_TRUE(reader.readCandidate(shard, 0, candidate, error));
  EXPECT_EQ(source.reads, 1);
  uint32_t global = 0;
  source.reads = 0;
  ASSERT_TRUE(reader.readGlobalLexemeId(0, global, error));
  ASSERT_TRUE(reader.readGlobalLexemeId(1, global, error));
  EXPECT_EQ(source.reads, 1);
}

TEST(BookLanguageReader, RejectsMalformedShardAndCandidateRecords) {
  auto data = makeArtifact();
  writeU32(data, 132, 1);  // Shard reserved field.
  refreshCrc(data);
  BookLanguageReader reader;
  ReaderError error;
  ASSERT_TRUE(reader.open(sourceFor(data), error));
  dictionary::book_language::ShardDirectoryRecord shard;
  EXPECT_FALSE(reader.readShard(0, shard, error));
  EXPECT_EQ(error, ReaderError::SHARD_RECORD_INVALID);

  data = makeArtifact();
  data[148] = 0x80;  // Unknown candidate flag.
  refreshCrc(data);
  ASSERT_TRUE(reader.open(sourceFor(data), error));
  ASSERT_TRUE(reader.readShard(0, shard, error));
  const dictionary::book_language::InlineCandidate* candidate = nullptr;
  EXPECT_FALSE(reader.readCandidate(shard, 0, candidate, error));
  EXPECT_EQ(error, ReaderError::CANDIDATE_RECORD_INVALID);
}

TEST(BookLanguageReader, RejectsGrammarBeforeLocalLemmaIds) {
  constexpr uint32_t invalidDescriptors[] = {
      0x00020000U,  // Reserved bit.
      0x00000005U,  // Reserved case.
      0x00000600U,  // Reserved number.
      0x00010001U,  // Infinitive with case.
      0x00018080U,  // Participle with mood.
      0x00008001U,  // Finite with case.
      0x00000080U,  // Mood without verb form.
      0x00002000U,  // Tense without verb form.
  };
  for (const uint32_t descriptor : invalidDescriptors) {
    auto data = makeArtifact();
    writeU32(data, 152, descriptor);
    writeU16(data, 156, 2);  // Also out of range; grammar must fail first.
    refreshCrc(data);
    BookLanguageReader reader;
    ReaderError error;
    ASSERT_TRUE(reader.open(sourceFor(data), error));
    dictionary::book_language::ShardDirectoryRecord shard;
    ASSERT_TRUE(reader.readShard(0, shard, error));
    const dictionary::book_language::InlineCandidate* candidate = nullptr;
    EXPECT_FALSE(reader.readCandidate(shard, 0, candidate, error));
    EXPECT_EQ(error, ReaderError::GRAMMAR_DESCRIPTOR_INVALID) << "descriptor=0x" << std::hex << descriptor;
  }
}

TEST(BookLanguageReader, AcceptsContextualCandidateFlags) {
  auto contextual = makeArtifact();
  contextual[148] = 0x20;  // Proper-noun contextual classification.
  refreshCrc(contextual);
  BookLanguageReader reader;
  ReaderError error;
  ASSERT_TRUE(reader.open(sourceFor(contextual), error));
  dictionary::book_language::ShardDirectoryRecord shard;
  ASSERT_TRUE(reader.readShard(0, shard, error));
  const dictionary::book_language::InlineCandidate* candidate = nullptr;
  ASSERT_TRUE(reader.readCandidate(shard, 0, candidate, error));
  EXPECT_EQ(candidate->flags, 0x20);
}

TEST(PageShortlist, IntersectsInlineCandidatesAndRetainsAmbiguity) {
  const auto data = makeArtifact();
  BookLanguageReader reader;
  ReaderError readerError;
  ASSERT_TRUE(reader.open(sourceFor(data), readerError));
  dictionary::page_shortlist::Generator generator;
  generator.reset();
  EXPECT_TRUE(generator.addRenderedWord("Die", false));
  EXPECT_TRUE(generator.addRenderedWord("liebe,", false));
  EXPECT_TRUE(generator.addRenderedWord("Welt!", false));
  generator.finishRenderedPage();
  dictionary::page_shortlist::Shortlist shortlist;
  dictionary::page_shortlist::GenerateError error;
  ASSERT_TRUE(generator.generate(reader, 0, 0, shortlist, error));
  ASSERT_EQ(shortlist.count, 1);
  EXPECT_EQ(shortlist.surface(0), "liebe");
  EXPECT_EQ(shortlist.items[0].analysisCount, 2);
  EXPECT_EQ(shortlist.items[0].grammarDescriptor, 0x0000DA80U);
  EXPECT_EQ(shortlist.items[0].localLemmaIds[0], 0);
  EXPECT_EQ(shortlist.items[0].localLemmaIds[1], 1);
}

TEST(PageShortlist, MergesCompatibleGrammarForRepeatedSurfaceAndPrimary) {
  const auto data = makeRepeatedSurfaceArtifact({{0x00008080U, 0}, {0x0000DA00U, 0}});
  BookLanguageReader reader;
  ReaderError readerError;
  ASSERT_TRUE(reader.open(sourceFor(data), readerError));
  dictionary::page_shortlist::Generator generator;
  generator.reset();
  ASSERT_TRUE(generator.addRenderedWord("liebe", false));
  generator.finishRenderedPage();
  dictionary::page_shortlist::Shortlist shortlist;
  dictionary::page_shortlist::GenerateError error;
  ASSERT_TRUE(generator.generate(reader, 0, 1, shortlist, error));
  ASSERT_EQ(shortlist.count, 1);
  EXPECT_EQ(shortlist.items[0].grammarDescriptor, 0x0000DA80U);
}

TEST(PageShortlist, KeepsGrammarConflictStickyAcrossRepeatedSurface) {
  const auto data = makeRepeatedSurfaceArtifact({{0x00000200U, 0}, {0x00000400U, 0}, {0x00000200U, 0}});
  BookLanguageReader reader;
  ReaderError readerError;
  ASSERT_TRUE(reader.open(sourceFor(data), readerError));
  dictionary::page_shortlist::Generator generator;
  generator.reset();
  ASSERT_TRUE(generator.addRenderedWord("liebe", false));
  generator.finishRenderedPage();
  dictionary::page_shortlist::Shortlist shortlist;
  dictionary::page_shortlist::GenerateError error;
  ASSERT_TRUE(generator.generate(reader, 0, 2, shortlist, error));
  ASSERT_EQ(shortlist.count, 1);
  EXPECT_EQ(shortlist.items[0].grammarDescriptor, 0U);
}

TEST(PageShortlist, ClearsGrammarWhenRepeatedSurfacePrimaryChanges) {
  const auto data = makeRepeatedSurfaceArtifact({{0x00000200U, 0}, {0x00000400U, 1}});
  BookLanguageReader reader;
  ReaderError readerError;
  ASSERT_TRUE(reader.open(sourceFor(data), readerError));
  dictionary::page_shortlist::Generator generator;
  generator.reset();
  ASSERT_TRUE(generator.addRenderedWord("liebe", false));
  generator.finishRenderedPage();
  dictionary::page_shortlist::Shortlist shortlist;
  dictionary::page_shortlist::GenerateError error;
  ASSERT_TRUE(generator.generate(reader, 0, 1, shortlist, error));
  ASSERT_EQ(shortlist.count, 1);
  EXPECT_EQ(shortlist.items[0].grammarDescriptor, 0U);
}

TEST(PageShortlist, StaysWithinV5GrowthBudget) {
  EXPECT_EQ(sizeof(dictionary::page_shortlist::Item), 36U);
  EXPECT_EQ(sizeof(dictionary::page_shortlist::Shortlist), 3784U);
  EXPECT_LE(sizeof(dictionary::page_shortlist::Shortlist) - dictionary::page_shortlist::kV4ShortlistSize,
            dictionary::page_shortlist::kMaxV5ShortlistGrowth);
}

TEST(PageShortlist, UsesOnlyPrimaryCanonicalAnalysisForLearningIdentity) {
  dictionary::page_shortlist::Item item;
  item.analysisCount = 3;
  item.localLemmaIds[0] = 4;
  item.localLemmaIds[1] = 7;
  item.localLemmaIds[2] = 9;
  EXPECT_EQ(dictionary::page_shortlist::learningIdentityCount(item), 1);
  item.analysisCount = 0;
  EXPECT_EQ(dictionary::page_shortlist::learningIdentityCount(item), 0);
}

TEST(PageShortlist, SortsHardestFirstWithStablePageOrderTies) {
  dictionary::page_shortlist::Shortlist shortlist;
  shortlist.count = 3;
  shortlist.items[0].difficulty = 40;
  shortlist.items[0].confidence = 1000;
  shortlist.items[0].visibleOrder = 0;
  shortlist.items[1].difficulty = 200;
  shortlist.items[1].confidence = 900;
  shortlist.items[1].visibleOrder = 1;
  shortlist.items[2].difficulty = 200;
  shortlist.items[2].confidence = 900;
  shortlist.items[2].visibleOrder = 2;

  dictionary::page_shortlist::sortForDisplay(shortlist);

  EXPECT_EQ(shortlist.items[0].visibleOrder, 1);
  EXPECT_EQ(shortlist.items[1].visibleOrder, 2);
  EXPECT_EQ(shortlist.items[2].visibleOrder, 0);
}

TEST(PageShortlist, JoinsLayoutInsertedHyphensBeforeHashing) {
  const auto data = makeArtifact();
  BookLanguageReader reader;
  ReaderError readerError;
  ASSERT_TRUE(reader.open(sourceFor(data), readerError));
  dictionary::page_shortlist::Generator generator;
  generator.reset();
  EXPECT_TRUE(generator.addRenderedWord("lie-", true));
  EXPECT_TRUE(generator.addRenderedWord("be", false));
  generator.finishRenderedPage();
  dictionary::page_shortlist::Shortlist shortlist;
  dictionary::page_shortlist::GenerateError error;
  ASSERT_TRUE(generator.generate(reader, 0, 0, shortlist, error));
  ASSERT_EQ(shortlist.count, 1);
  EXPECT_EQ(shortlist.surface(0), "liebe");
}

TEST(PageShortlist, TokenizesGermanPunctuationWithoutAllocating) {
  dictionary::page_shortlist::Generator generator;
  generator.reset();
  ASSERT_TRUE(generator.addRenderedWord("Straße", false));
  ASSERT_TRUE(generator.addRenderedWord("–", false));
  ASSERT_TRUE(generator.addRenderedWord("E-Mail", false));
  ASSERT_TRUE(generator.addRenderedWord("O’Connor", false));
  ASSERT_TRUE(generator.addRenderedWord("foo_bar", false));
  ASSERT_TRUE(generator.addRenderedWord("123", false));
  generator.finishRenderedPage();
  EXPECT_EQ(generator.visibleTokenCount(), 5);
}

TEST(PageShortlist, RejectsInvalidShardRanges) {
  const auto data = makeArtifact();
  BookLanguageReader reader;
  ReaderError readerError;
  ASSERT_TRUE(reader.open(sourceFor(data), readerError));
  dictionary::page_shortlist::Generator generator;
  generator.reset();
  ASSERT_TRUE(generator.addRenderedWord("liebe", false));
  generator.finishRenderedPage();
  dictionary::page_shortlist::Shortlist shortlist;
  dictionary::page_shortlist::GenerateError error;
  EXPECT_FALSE(generator.generate(reader, 0, 1, shortlist, error));
  EXPECT_EQ(error, dictionary::page_shortlist::GenerateError::SHARD_RANGE_INVALID);
}

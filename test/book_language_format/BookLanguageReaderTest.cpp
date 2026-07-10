#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "BookLanguageReader.h"
#include "PageShortlist.h"

namespace {
using dictionary::book_language::BookLanguageReader;
using dictionary::book_language::RandomAccessSource;
using dictionary::book_language::ReaderError;

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

void refreshHeaderCrc(std::vector<uint8_t>& data) {
  writeU32(data, 100,
           dictionary::book_language::updateCrc32(0, data.data() + dictionary::book_language::kHeaderSize,
                                                  data.size() - dictionary::book_language::kHeaderSize));
  writeU32(data, 104, dictionary::book_language::updateCrc32(0, data.data(), 104));
}

std::vector<uint8_t> makeArtifact() {
  std::vector<uint8_t> data(248, 0);
  std::memcpy(data.data(), "CXLG", 4);
  writeU16(data, 4, 1);
  writeU16(data, 6, 108);
  writeU16(data, 12, 1);
  writeU16(data, 14, 1);
  for (size_t index = 0; index < 16; ++index) data[16 + index] = static_cast<uint8_t>(index + 1);
  std::memcpy(data.data() + 32, "de", 2);
  std::memcpy(data.data() + 40, "en", 2);
  writeU16(data, 48, 1);
  writeU32(data, 52, 1);
  writeU32(data, 56, 1);
  writeU32(data, 60, 2);
  writeU32(data, 64, 1);
  writeU32(data, 68, 108);
  writeU32(data, 72, 116);
  writeU32(data, 76, 132);
  writeU32(data, 80, 148);
  writeU32(data, 84, 156);
  writeU32(data, 88, 172);
  writeU32(data, 92, 244);
  writeU32(data, 96, data.size());

  writeU32(data, 108, 0);   // Spine first shard.
  writeU32(data, 112, 1);   // Spine shard count.
  writeU32(data, 116, 0);   // Shard first candidate.
  writeU16(data, 120, 1);   // Candidate count.
  writeU32(data, 124, 0);   // Source token start.
  writeU32(data, 128, 64);  // Source token end.

  const std::string_view surface = "liebe";
  writeU64(data, 132, dictionary::book_language::fnv1a64(surface));
  writeU16(data, 140, 0);  // Surface ID.
  writeU16(data, 142, 0);  // Primary lemma.
  writeU16(data, 144, 1);  // Alternate lemma.
  data[146] = surface.size();
  data[147] = 1;  // Ambiguous.
  writeU32(data, 148, 10);
  writeU32(data, 152, 20);
  writeU32(data, 156, 10);
  writeU16(data, 160, 0);
  writeU32(data, 164, 20);
  writeU16(data, 168, 1);

  std::memcpy(data.data() + 172, "CXSD", 4);
  writeU16(data, 176, 1);
  writeU16(data, 178, 40);
  writeU32(data, 180, 1);   // Surfaces.
  writeU32(data, 184, 2);   // Analyses.
  writeU32(data, 188, 0);   // Components.
  writeU32(data, 192, 40);  // Surface records.
  writeU32(data, 196, 60);  // Analyses.
  writeU32(data, 200, 64);  // Components.
  writeU32(data, 204, 64);  // Strings.
  writeU32(data, 208, 72);  // Section size.
  writeU32(data, 212, 0);   // String offset.
  writeU32(data, 216, 0);   // First analysis.
  writeU32(data, 220, 0);   // First component.
  writeU16(data, 224, surface.size());
  data[226] = 2;
  data[227] = 0;
  writeU16(data, 228, 900);
  writeU16(data, 230, 1);
  writeU16(data, 232, 0);
  writeU16(data, 234, 1);
  std::memcpy(data.data() + 236, surface.data(), surface.size());
  std::memcpy(data.data() + 244, "meta", 4);
  refreshHeaderCrc(data);
  return data;
}

bool readMemory(void* context, const uint32_t offset, void* output, const size_t length) {
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

bool readCountingMemory(void* context, const uint32_t offset, void* output, const size_t length) {
  auto& source = *static_cast<CountingSource*>(context);
  if (static_cast<uint64_t>(offset) + length > source.data.size()) return false;
  ++source.reads;
  std::memcpy(output, source.data.data() + offset, length);
  return true;
}

}  // namespace

TEST(BookLanguageReader, ReadsShardCandidateAndAmbiguousSurface) {
  const auto data = makeArtifact();
  BookLanguageReader reader;
  ReaderError error;
  ASSERT_TRUE(reader.open(sourceFor(data), error)) << dictionary::book_language::readerErrorName(error);

  dictionary::book_language::ShardDirectoryRecord shard;
  ASSERT_TRUE(reader.readShard(0, shard, error));
  EXPECT_EQ(shard.recordCount, 1);

  dictionary::book_language::ShardCandidate candidate;
  ASSERT_TRUE(reader.readCandidate(shard, 0, candidate, error));
  EXPECT_EQ(candidate.primaryLocalLemmaId, 0);
  EXPECT_EQ(candidate.alternateLocalLemmaId, 1);
  EXPECT_EQ(candidate.surfaceHash, dictionary::book_language::fnv1a64("liebe"));

  dictionary::book_language::SurfaceRecord surface;
  ASSERT_TRUE(reader.readSurface(candidate.localSurfaceId, surface, error));
  EXPECT_EQ(surface.analysisCount, 2);
  bool equal = false;
  EXPECT_TRUE(reader.surfaceEquals(surface, "liebe", equal, error));
  EXPECT_TRUE(equal);
  EXPECT_TRUE(reader.surfaceEquals(surface, "Liebe", equal, error));
  EXPECT_FALSE(equal);

  uint16_t localLemmaId = UINT16_MAX;
  ASSERT_TRUE(reader.readSurfaceAnalysis(surface, 0, localLemmaId, error));
  EXPECT_EQ(localLemmaId, 0);
  ASSERT_TRUE(reader.readSurfaceAnalysis(surface, 1, localLemmaId, error));
  EXPECT_EQ(localLemmaId, 1);
  uint32_t globalLexemeId = 0;
  ASSERT_TRUE(reader.readGlobalLexemeId(1, globalLexemeId, error));
  EXPECT_EQ(globalLexemeId, 20U);
}

TEST(BookLanguageReader, CachesSequentialLocalLemmaRecords) {
  CountingSource source{makeArtifact()};
  BookLanguageReader reader;
  ReaderError error;
  ASSERT_TRUE(reader.open({&source, source.data.size(), readCountingMemory}, error));
  source.reads = 0;

  uint32_t globalLexemeId = 0;
  ASSERT_TRUE(reader.readGlobalLexemeId(0, globalLexemeId, error));
  EXPECT_EQ(globalLexemeId, 10U);
  ASSERT_TRUE(reader.readGlobalLexemeId(1, globalLexemeId, error));
  EXPECT_EQ(globalLexemeId, 20U);
  EXPECT_EQ(source.reads, 1);
}

TEST(BookLanguageReader, RejectsMalformedSurfaceHeader) {
  auto data = makeArtifact();
  writeU32(data, 208, 68);  // Declared section size does not reach metadata.
  refreshHeaderCrc(data);
  BookLanguageReader reader;
  ReaderError error;
  EXPECT_FALSE(reader.open(sourceFor(data), error));
  EXPECT_EQ(error, ReaderError::SURFACE_HEADER_INVALID);
}

TEST(BookLanguageReader, RejectsMalformedShardAndCandidateRecords) {
  auto data = makeArtifact();
  BookLanguageReader reader;
  ReaderError error;
  ASSERT_TRUE(reader.open(sourceFor(data), error));

  dictionary::book_language::ShardDirectoryRecord shard;
  data[122] = 1;  // Reserved shard byte.
  EXPECT_FALSE(reader.readShard(0, shard, error));
  EXPECT_EQ(error, ReaderError::SHARD_RECORD_INVALID);

  data = makeArtifact();
  ASSERT_TRUE(reader.open(sourceFor(data), error));
  ASSERT_TRUE(reader.readShard(0, shard, error));
  data[147] = 0x80;  // Unknown candidate flag.
  dictionary::book_language::ShardCandidate candidate;
  EXPECT_FALSE(reader.readCandidate(shard, 0, candidate, error));
  EXPECT_EQ(error, ReaderError::CANDIDATE_RECORD_INVALID);
}

TEST(PageShortlist, IntersectsLexicalTokensAndRetainsAmbiguity) {
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
  ASSERT_TRUE(generator.generate(reader, 0, 0, shortlist, error))
      << dictionary::page_shortlist::generateErrorName(error);
  ASSERT_EQ(shortlist.count, 1);
  EXPECT_EQ(shortlist.surface(0), "liebe");
  EXPECT_EQ(shortlist.items[0].primaryLocalLemmaId, 0);
  EXPECT_EQ(shortlist.items[0].alternateLocalLemmaId, 1);
  EXPECT_EQ(shortlist.items[0].analysisCount, 2);
  EXPECT_EQ(shortlist.items[0].localLemmaIds[0], 0);
  EXPECT_EQ(shortlist.items[0].localLemmaIds[1], 1);
  EXPECT_FALSE(shortlist.truncated);
}

TEST(PageShortlist, OmitsCompoundOnlyMatchesWithoutWholeWordDefinitions) {
  auto data = makeArtifact();
  writeU16(data, 142, UINT16_MAX);  // No primary whole-word analysis.
  writeU16(data, 144, UINT16_MAX);  // No alternate whole-word analysis.
  writeU32(data, 184, 0);           // Surface analysis count.
  writeU32(data, 188, 2);           // Surface component count.
  writeU32(data, 196, 60);          // Empty analyses start.
  writeU32(data, 200, 60);          // Components start.
  data[226] = 0;
  data[227] = 2;
  refreshHeaderCrc(data);

  BookLanguageReader reader;
  ReaderError readerError;
  ASSERT_TRUE(reader.open(sourceFor(data), readerError));
  dictionary::page_shortlist::Generator generator;
  generator.reset();
  ASSERT_TRUE(generator.addRenderedWord("liebe", false));
  generator.finishRenderedPage();

  dictionary::page_shortlist::Shortlist shortlist;
  dictionary::page_shortlist::GenerateError error;
  ASSERT_TRUE(generator.generate(reader, 0, 0, shortlist, error));
  EXPECT_EQ(shortlist.count, 0);
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
  EXPECT_EQ(generator.visibleTokenCount(), 1);

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
  EXPECT_FALSE(generator.visibleTokensTruncated());
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

TEST(BookLanguageReader, RejectsOutOfRangeReads) {
  const auto data = makeArtifact();
  BookLanguageReader reader;
  ReaderError error;
  ASSERT_TRUE(reader.open(sourceFor(data), error));

  dictionary::book_language::ShardDirectoryRecord shard;
  EXPECT_FALSE(reader.readShard(1, shard, error));
  EXPECT_EQ(error, ReaderError::SHARD_ID_OUT_OF_RANGE);

  dictionary::book_language::SurfaceRecord surface;
  EXPECT_FALSE(reader.readSurface(1, surface, error));
  EXPECT_EQ(error, ReaderError::SURFACE_ID_OUT_OF_RANGE);
}

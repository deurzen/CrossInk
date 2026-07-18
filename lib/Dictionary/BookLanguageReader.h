#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "BookLanguageFormat.h"

namespace dictionary::book_language {

struct RandomAccessSource {
  void* context = nullptr;
  uint64_t size = 0;
  bool (*readAt)(void* context, uint32_t offset, void* output, size_t length) = nullptr;
};

struct ShardDirectoryRecord {
  uint32_t blobOffset = 0;
  uint16_t blobLength = 0;
  uint16_t recordCount = 0;
  uint32_t sourceTokenStart = 0;
  uint32_t sourceTokenEnd = 0;
};

struct InlineCandidate {
  uint64_t surfaceHash = 0;
  uint16_t recordSize = 0;
  uint8_t surfaceLength = 0;
  uint8_t analysisCount = 0;
  uint8_t flags = 0;
  uint8_t difficulty = 0;
  uint16_t confidence = 0;
  uint16_t localLemmaIds[kMaxInlineAnalyses]{};
  char surface[256]{};
};

enum class ReaderError : uint8_t {
  NONE = 0,
  SOURCE_UNAVAILABLE,
  HEADER_INVALID,
  READ_FAILED,
  SHARD_ID_OUT_OF_RANGE,
  SHARD_RECORD_INVALID,
  CANDIDATE_INDEX_OUT_OF_RANGE,
  CANDIDATE_RECORD_INVALID,
  LOCAL_LEMMA_ID_OUT_OF_RANGE,
};

// Allocation-free reader for the CRC-validated contextual v4 artifact.
// Shard records are consumed sequentially through one bounded cache.
class BookLanguageReader {
 public:
  bool open(const RandomAccessSource& source, ReaderError& error);
  bool readShard(uint32_t shardId, ShardDirectoryRecord& out, ReaderError& error) const;
  bool readCandidate(const ShardDirectoryRecord& shard, uint16_t index, const InlineCandidate*& out,
                     ReaderError& error) const;
  bool readGlobalLexemeId(uint16_t localLemmaId, uint32_t& globalLexemeId, ReaderError& error) const;

  const Header& header() const { return header_; }
  bool isOpen() const { return open_; }

 private:
  Header header_{};
  RandomAccessSource source_{};
  mutable uint8_t shardDirectoryCache_[256]{};
  mutable uint8_t blobCache_[2048]{};
  mutable uint8_t localLemmaCache_[64 * kLocalLemmaRecordSize]{};
  mutable InlineCandidate candidate_{};
  mutable uint32_t shardDirectoryCacheStart_ = UINT32_MAX;
  mutable uint32_t blobCacheStart_ = UINT32_MAX;
  mutable uint32_t candidateCursor_ = 0;
  mutable uint32_t activeBlobOffset_ = UINT32_MAX;
  mutable uint16_t shardDirectoryCacheLength_ = 0;
  mutable uint16_t blobCacheLength_ = 0;
  mutable uint16_t nextCandidateIndex_ = 0;
  mutable uint16_t localLemmaCacheFirst_ = 0;
  mutable uint8_t localLemmaCacheCount_ = 0;
  bool open_ = false;

  bool readCached(uint32_t offset, size_t length, uint32_t sectionEnd, uint8_t* cache, size_t cacheCapacity,
                  uint32_t& cacheStart, uint16_t& cacheLength, void* output) const;
};

static_assert(sizeof(BookLanguageReader) <= 3072, "Book language reader exceeds its lookup-session memory budget");

uint64_t fnv1a64(std::string_view text);
const char* readerErrorName(ReaderError error);

}  // namespace dictionary::book_language

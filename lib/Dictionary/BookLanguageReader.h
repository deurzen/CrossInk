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
  uint32_t firstRecord = 0;
  uint16_t recordCount = 0;
  uint32_t sourceTokenStart = 0;
  uint32_t sourceTokenEnd = 0;
};

struct ShardCandidate {
  uint64_t surfaceHash = 0;
  uint16_t localSurfaceId = UINT16_MAX;
  uint16_t primaryLocalLemmaId = UINT16_MAX;
  uint16_t alternateLocalLemmaId = UINT16_MAX;
  uint8_t surfaceByteLength = 0;
  uint8_t flags = 0;
};

struct SurfaceRecord {
  uint32_t stringOffset = 0;
  uint32_t firstAnalysis = 0;
  uint32_t firstComponent = 0;
  uint16_t stringLength = 0;
  uint8_t analysisCount = 0;
  uint8_t componentCount = 0;
  uint16_t confidence = 0;
  uint16_t flags = 0;
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
  SURFACE_ID_OUT_OF_RANGE,
  SURFACE_HEADER_INVALID,
  SURFACE_RECORD_INVALID,
};

// Allocation-free random-access reader for a previously CRC-validated
// language.bin. Every table record is range-checked before its data is used.
class BookLanguageReader {
 public:
  bool open(const RandomAccessSource& source, ReaderError& error);
  bool readShard(uint32_t shardId, ShardDirectoryRecord& out, ReaderError& error) const;
  bool readCandidate(const ShardDirectoryRecord& shard, uint16_t index, ShardCandidate& out, ReaderError& error) const;
  bool readGlobalLexemeId(uint16_t localLemmaId, uint32_t& globalLexemeId, ReaderError& error) const;
  bool readSurface(uint16_t localSurfaceId, SurfaceRecord& out, ReaderError& error) const;
  bool surfaceEquals(const SurfaceRecord& surface, std::string_view expected, bool& equal, ReaderError& error) const;
  bool readSurfaceAnalysis(const SurfaceRecord& surface, uint8_t index, uint16_t& localLemmaId,
                           ReaderError& error) const;
  bool readSurfaceComponent(const SurfaceRecord& surface, uint8_t index, uint16_t& localLemmaId,
                            ReaderError& error) const;

  const Header& header() const { return header_; }
  bool isOpen() const { return open_; }

 private:
  Header header_{};
  RandomAccessSource source_{};
  uint32_t surfaceSectionSize_ = 0;
  uint32_t surfaceRecordOffset_ = 0;
  uint32_t analysisOffset_ = 0;
  uint32_t componentOffset_ = 0;
  uint32_t stringPoolOffset_ = 0;
  uint32_t analysisCount_ = 0;
  uint32_t componentCount_ = 0;
  bool open_ = false;
};

uint64_t fnv1a64(std::string_view text);
const char* readerErrorName(ReaderError error);

}  // namespace dictionary::book_language

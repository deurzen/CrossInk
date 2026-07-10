#include "BookLanguageReader.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace dictionary::book_language {
namespace {

constexpr uint8_t kSurfaceMagic[] = {'C', 'X', 'S', 'D'};
constexpr uint16_t kSurfaceVersion = 1;
constexpr uint16_t kSurfaceHeaderSize = 40;
constexpr uint32_t kSurfaceRecordSize = 20;
constexpr uint8_t kKnownCandidateFlags = 0x07;

uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

uint64_t readU64(const uint8_t* data) {
  return static_cast<uint64_t>(readU32(data)) | (static_cast<uint64_t>(readU32(data + 4)) << 32U);
}

bool sourceCanRead(const RandomAccessSource& source) {
  return source.readAt != nullptr && source.size <= std::numeric_limits<uint32_t>::max();
}

bool rangeFits(const uint32_t offset, const uint32_t length, const uint32_t size) {
  return static_cast<uint64_t>(offset) + length <= size;
}

bool tableFits(const uint32_t offset, const uint32_t count, const uint32_t recordSize, const uint32_t nextOffset) {
  return static_cast<uint64_t>(offset) + static_cast<uint64_t>(count) * recordSize <= nextOffset;
}

bool isAligned(const uint32_t value) { return (value & 0x03U) == 0; }

}  // namespace

bool BookLanguageReader::open(const RandomAccessSource& source, ReaderError& error) {
  open_ = false;
  header_ = {};
  source_ = {};
  surfaceSectionSize_ = 0;
  surfaceRecordOffset_ = 0;
  analysisOffset_ = 0;
  componentOffset_ = 0;
  stringPoolOffset_ = 0;
  analysisCount_ = 0;
  componentCount_ = 0;
  shardCacheStart_ = UINT32_MAX;
  candidateCacheStart_ = UINT32_MAX;
  surfaceRecordCacheStart_ = UINT32_MAX;
  surfaceDetailCacheStart_ = UINT32_MAX;
  surfaceStringCacheStart_ = UINT32_MAX;
  shardCacheLength_ = 0;
  candidateCacheLength_ = 0;
  surfaceRecordCacheLength_ = 0;
  surfaceDetailCacheLength_ = 0;
  surfaceStringCacheLength_ = 0;
  localLemmaCacheFirst_ = 0;
  localLemmaCacheCount_ = 0;
  error = ReaderError::NONE;

  if (!sourceCanRead(source)) {
    error = ReaderError::SOURCE_UNAVAILABLE;
    return false;
  }

  uint8_t headerBytes[kHeaderSize]{};
  if (source.size < sizeof(headerBytes) || !source.readAt(source.context, 0, headerBytes, sizeof(headerBytes))) {
    error = ReaderError::READ_FAILED;
    return false;
  }
  FormatError formatError = FormatError::NONE;
  if (!parseHeader(headerBytes, sizeof(headerBytes), source.size, header_, formatError)) {
    (void)formatError;
    header_ = {};
    error = ReaderError::HEADER_INVALID;
    return false;
  }

  const uint32_t surfaceSectionSize = header_.metadataOffset - header_.surfaceDetailOffset;
  uint8_t surfaceHeader[kSurfaceHeaderSize]{};
  if (surfaceSectionSize < sizeof(surfaceHeader) ||
      !source.readAt(source.context, header_.surfaceDetailOffset, surfaceHeader, sizeof(surfaceHeader))) {
    header_ = {};
    error = ReaderError::SURFACE_HEADER_INVALID;
    return false;
  }
  if (std::memcmp(surfaceHeader, kSurfaceMagic, sizeof(kSurfaceMagic)) != 0 ||
      readU16(surfaceHeader + 4) != kSurfaceVersion || readU16(surfaceHeader + 6) != kSurfaceHeaderSize ||
      readU32(surfaceHeader + 8) != header_.localSurfaceCount) {
    header_ = {};
    error = ReaderError::SURFACE_HEADER_INVALID;
    return false;
  }

  analysisCount_ = readU32(surfaceHeader + 12);
  componentCount_ = readU32(surfaceHeader + 16);
  surfaceRecordOffset_ = readU32(surfaceHeader + 20);
  analysisOffset_ = readU32(surfaceHeader + 24);
  componentOffset_ = readU32(surfaceHeader + 28);
  stringPoolOffset_ = readU32(surfaceHeader + 32);
  surfaceSectionSize_ = readU32(surfaceHeader + 36);
  if (surfaceSectionSize_ != surfaceSectionSize || !isAligned(surfaceRecordOffset_) || !isAligned(analysisOffset_) ||
      !isAligned(componentOffset_) || !isAligned(stringPoolOffset_) || surfaceRecordOffset_ < kSurfaceHeaderSize ||
      analysisOffset_ < surfaceRecordOffset_ || componentOffset_ < analysisOffset_ ||
      stringPoolOffset_ < componentOffset_ || surfaceSectionSize_ < stringPoolOffset_ ||
      !tableFits(surfaceRecordOffset_, header_.localSurfaceCount, kSurfaceRecordSize, analysisOffset_) ||
      !tableFits(analysisOffset_, analysisCount_, sizeof(uint16_t), componentOffset_) ||
      !tableFits(componentOffset_, componentCount_, sizeof(uint16_t), stringPoolOffset_)) {
    header_ = {};
    error = ReaderError::SURFACE_HEADER_INVALID;
    return false;
  }

  source_ = source;
  open_ = true;
  return true;
}

bool BookLanguageReader::readCached(const uint32_t offset, const size_t length, const uint32_t sectionEnd,
                                    uint8_t* cache, const size_t cacheCapacity, uint32_t& cacheStart,
                                    uint16_t& cacheLength, void* output) const {
  if (length > cacheCapacity || static_cast<uint64_t>(offset) + length > sectionEnd) return false;
  if (cacheLength == 0 || offset < cacheStart || static_cast<uint64_t>(offset) + length > cacheStart + cacheLength) {
    cacheStart = offset;
    const size_t available = sectionEnd - offset;
    cacheLength = static_cast<uint16_t>(std::min(cacheCapacity, available));
    if (!source_.readAt(source_.context, cacheStart, cache, cacheLength)) {
      cacheLength = 0;
      return false;
    }
  }
  std::memcpy(output, cache + (offset - cacheStart), length);
  return true;
}

bool BookLanguageReader::readShard(const uint32_t shardId, ShardDirectoryRecord& out, ReaderError& error) const {
  out = {};
  error = ReaderError::NONE;
  if (!open_) {
    error = ReaderError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (shardId >= header_.shardCount) {
    error = ReaderError::SHARD_ID_OUT_OF_RANGE;
    return false;
  }

  uint8_t data[kShardDirectoryRecordSize]{};
  const uint32_t offset = header_.shardDirectoryOffset + shardId * kShardDirectoryRecordSize;
  if (!readCached(offset, sizeof(data), header_.shardRecordsOffset, shardCache_, sizeof(shardCache_), shardCacheStart_,
                  shardCacheLength_, data)) {
    error = ReaderError::READ_FAILED;
    return false;
  }
  out.firstRecord = readU32(data);
  out.recordCount = readU16(data + 4);
  const uint16_t reserved = readU16(data + 6);
  out.sourceTokenStart = readU32(data + 8);
  out.sourceTokenEnd = readU32(data + 12);
  if (reserved != 0 || static_cast<uint64_t>(out.firstRecord) + out.recordCount > header_.shardRecordCount ||
      out.sourceTokenStart > out.sourceTokenEnd) {
    out = {};
    error = ReaderError::SHARD_RECORD_INVALID;
    return false;
  }
  return true;
}

bool BookLanguageReader::readCandidate(const ShardDirectoryRecord& shard, const uint16_t index, ShardCandidate& out,
                                       ReaderError& error) const {
  out = {};
  error = ReaderError::NONE;
  if (!open_) {
    error = ReaderError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (index >= shard.recordCount) {
    error = ReaderError::CANDIDATE_INDEX_OUT_OF_RANGE;
    return false;
  }
  const uint32_t recordIndex = shard.firstRecord + index;
  if (recordIndex >= header_.shardRecordCount) {
    error = ReaderError::SHARD_RECORD_INVALID;
    return false;
  }

  uint8_t data[kShardCandidateRecordSize]{};
  const uint32_t offset = header_.shardRecordsOffset + recordIndex * kShardCandidateRecordSize;
  if (!readCached(offset, sizeof(data), header_.localLemmaTableOffset, candidateCache_, sizeof(candidateCache_),
                  candidateCacheStart_, candidateCacheLength_, data)) {
    error = ReaderError::READ_FAILED;
    return false;
  }
  out.surfaceHash = readU64(data);
  out.localSurfaceId = readU16(data + 8);
  out.primaryLocalLemmaId = readU16(data + 10);
  out.alternateLocalLemmaId = readU16(data + 12);
  out.surfaceByteLength = data[14];
  out.flags = data[15];
  const bool primaryValid = out.primaryLocalLemmaId == UINT16_MAX || out.primaryLocalLemmaId < header_.localLemmaCount;
  const bool alternateValid =
      out.alternateLocalLemmaId == UINT16_MAX || out.alternateLocalLemmaId < header_.localLemmaCount;
  if (out.localSurfaceId >= header_.localSurfaceCount || out.surfaceByteLength == 0 ||
      (out.flags & ~kKnownCandidateFlags) != 0 || !primaryValid || !alternateValid ||
      (out.primaryLocalLemmaId == UINT16_MAX && out.alternateLocalLemmaId != UINT16_MAX)) {
    out = {};
    error = ReaderError::CANDIDATE_RECORD_INVALID;
    return false;
  }
  return true;
}

bool BookLanguageReader::readGlobalLexemeId(const uint16_t localLemmaId, uint32_t& globalLexemeId,
                                            ReaderError& error) const {
  globalLexemeId = 0;
  error = ReaderError::NONE;
  if (!open_) {
    error = ReaderError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (localLemmaId >= header_.localLemmaCount) {
    error = ReaderError::LOCAL_LEMMA_ID_OUT_OF_RANGE;
    return false;
  }
  if (localLemmaCacheCount_ == 0 || localLemmaId < localLemmaCacheFirst_ ||
      localLemmaId >= static_cast<uint32_t>(localLemmaCacheFirst_) + localLemmaCacheCount_) {
    localLemmaCacheFirst_ = localLemmaId;
    const uint32_t remaining = header_.localLemmaCount - localLemmaId;
    localLemmaCacheCount_ = static_cast<uint8_t>(remaining < 64U ? remaining : 64U);
    const uint32_t offset = header_.localLemmaTableOffset + localLemmaId * kLocalLemmaRecordSize;
    const size_t bytes = static_cast<size_t>(localLemmaCacheCount_) * kLocalLemmaRecordSize;
    if (!source_.readAt(source_.context, offset, localLemmaCache_, bytes)) {
      localLemmaCacheCount_ = 0;
      error = ReaderError::READ_FAILED;
      return false;
    }
  }
  const size_t cacheOffset = static_cast<size_t>(localLemmaId - localLemmaCacheFirst_) * kLocalLemmaRecordSize;
  globalLexemeId = readU32(localLemmaCache_ + cacheOffset);
  return true;
}

bool BookLanguageReader::readSurface(const uint16_t localSurfaceId, SurfaceRecord& out, ReaderError& error) const {
  out = {};
  error = ReaderError::NONE;
  if (!open_) {
    error = ReaderError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (localSurfaceId >= header_.localSurfaceCount) {
    error = ReaderError::SURFACE_ID_OUT_OF_RANGE;
    return false;
  }
  uint8_t data[kSurfaceRecordSize]{};
  const uint32_t relativeOffset = surfaceRecordOffset_ + localSurfaceId * kSurfaceRecordSize;
  const uint32_t offset = header_.surfaceDetailOffset + relativeOffset;
  const uint32_t sectionEnd = header_.surfaceDetailOffset + surfaceSectionSize_;
  if (!readCached(offset, sizeof(data), sectionEnd, surfaceRecordCache_, sizeof(surfaceRecordCache_),
                  surfaceRecordCacheStart_, surfaceRecordCacheLength_, data)) {
    error = ReaderError::READ_FAILED;
    return false;
  }
  out.stringOffset = readU32(data);
  out.firstAnalysis = readU32(data + 4);
  out.firstComponent = readU32(data + 8);
  out.stringLength = readU16(data + 12);
  out.analysisCount = data[14];
  out.componentCount = data[15];
  out.confidence = readU16(data + 16);
  out.flags = readU16(data + 18);
  const uint32_t stringPoolSize = surfaceSectionSize_ - stringPoolOffset_;
  if (out.stringLength == 0 || !rangeFits(out.stringOffset, out.stringLength, stringPoolSize) ||
      static_cast<uint64_t>(out.firstAnalysis) + out.analysisCount > analysisCount_ ||
      static_cast<uint64_t>(out.firstComponent) + out.componentCount > componentCount_ || out.confidence > 1000 ||
      (out.flags & ~kKnownCandidateFlags) != 0) {
    out = {};
    error = ReaderError::SURFACE_RECORD_INVALID;
    return false;
  }
  return true;
}

bool BookLanguageReader::surfaceEquals(const SurfaceRecord& surface, const std::string_view expected, bool& equal,
                                       ReaderError& error) const {
  equal = false;
  error = ReaderError::NONE;
  if (!open_) {
    error = ReaderError::SOURCE_UNAVAILABLE;
    return false;
  }
  const uint32_t stringPoolSize = surfaceSectionSize_ - stringPoolOffset_;
  if (surface.stringLength == 0 || !rangeFits(surface.stringOffset, surface.stringLength, stringPoolSize)) {
    error = ReaderError::SURFACE_RECORD_INVALID;
    return false;
  }
  if (expected.size() != surface.stringLength) return true;

  uint8_t chunk[32]{};
  size_t compared = 0;
  const uint32_t sectionEnd = header_.surfaceDetailOffset + surfaceSectionSize_;
  while (compared < expected.size()) {
    const size_t length = std::min(sizeof(chunk), expected.size() - compared);
    const uint32_t offset = header_.surfaceDetailOffset + stringPoolOffset_ + surface.stringOffset + compared;
    if (!readCached(offset, length, sectionEnd, surfaceStringCache_, sizeof(surfaceStringCache_),
                    surfaceStringCacheStart_, surfaceStringCacheLength_, chunk)) {
      error = ReaderError::READ_FAILED;
      return false;
    }
    if (std::memcmp(chunk, expected.data() + compared, length) != 0) return true;
    compared += length;
  }
  equal = true;
  return true;
}

bool BookLanguageReader::readSurfaceAnalysis(const SurfaceRecord& surface, const uint8_t index, uint16_t& localLemmaId,
                                             ReaderError& error) const {
  localLemmaId = UINT16_MAX;
  error = ReaderError::NONE;
  if (!open_) {
    error = ReaderError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (index >= surface.analysisCount || static_cast<uint64_t>(surface.firstAnalysis) + index >= analysisCount_) {
    error = ReaderError::SURFACE_RECORD_INVALID;
    return false;
  }
  uint8_t data[sizeof(uint16_t)]{};
  const uint32_t relativeOffset = analysisOffset_ + (surface.firstAnalysis + index) * sizeof(uint16_t);
  const uint32_t offset = header_.surfaceDetailOffset + relativeOffset;
  const uint32_t sectionEnd = header_.surfaceDetailOffset + surfaceSectionSize_;
  if (!readCached(offset, sizeof(data), sectionEnd, surfaceDetailCache_, sizeof(surfaceDetailCache_),
                  surfaceDetailCacheStart_, surfaceDetailCacheLength_, data)) {
    error = ReaderError::READ_FAILED;
    return false;
  }
  localLemmaId = readU16(data);
  if (localLemmaId >= header_.localLemmaCount) {
    localLemmaId = UINT16_MAX;
    error = ReaderError::LOCAL_LEMMA_ID_OUT_OF_RANGE;
    return false;
  }
  return true;
}

bool BookLanguageReader::readSurfaceComponent(const SurfaceRecord& surface, const uint8_t index, uint16_t& localLemmaId,
                                              ReaderError& error) const {
  localLemmaId = UINT16_MAX;
  error = ReaderError::NONE;
  if (!open_) {
    error = ReaderError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (index >= surface.componentCount || static_cast<uint64_t>(surface.firstComponent) + index >= componentCount_) {
    error = ReaderError::SURFACE_RECORD_INVALID;
    return false;
  }
  uint8_t data[sizeof(uint16_t)]{};
  const uint32_t relativeOffset = componentOffset_ + (surface.firstComponent + index) * sizeof(uint16_t);
  const uint32_t offset = header_.surfaceDetailOffset + relativeOffset;
  const uint32_t sectionEnd = header_.surfaceDetailOffset + surfaceSectionSize_;
  if (!readCached(offset, sizeof(data), sectionEnd, surfaceDetailCache_, sizeof(surfaceDetailCache_),
                  surfaceDetailCacheStart_, surfaceDetailCacheLength_, data)) {
    error = ReaderError::READ_FAILED;
    return false;
  }
  localLemmaId = readU16(data);
  if (localLemmaId >= header_.localLemmaCount) {
    localLemmaId = UINT16_MAX;
    error = ReaderError::LOCAL_LEMMA_ID_OUT_OF_RANGE;
    return false;
  }
  return true;
}

uint64_t fnv1a64(const std::string_view text) {
  uint64_t hash = 0xCBF29CE484222325ULL;
  for (const char value : text) {
    hash ^= static_cast<uint8_t>(value);
    hash *= 0x100000001B3ULL;
  }
  return hash;
}

const char* readerErrorName(const ReaderError error) {
  switch (error) {
    case ReaderError::NONE:
      return "none";
    case ReaderError::SOURCE_UNAVAILABLE:
      return "source unavailable";
    case ReaderError::HEADER_INVALID:
      return "header invalid";
    case ReaderError::READ_FAILED:
      return "read failed";
    case ReaderError::SHARD_ID_OUT_OF_RANGE:
      return "shard id out of range";
    case ReaderError::SHARD_RECORD_INVALID:
      return "shard record invalid";
    case ReaderError::CANDIDATE_INDEX_OUT_OF_RANGE:
      return "candidate index out of range";
    case ReaderError::CANDIDATE_RECORD_INVALID:
      return "candidate record invalid";
    case ReaderError::LOCAL_LEMMA_ID_OUT_OF_RANGE:
      return "local lemma id out of range";
    case ReaderError::SURFACE_ID_OUT_OF_RANGE:
      return "surface id out of range";
    case ReaderError::SURFACE_HEADER_INVALID:
      return "surface header invalid";
    case ReaderError::SURFACE_RECORD_INVALID:
      return "surface record invalid";
  }
  return "unknown";
}

}  // namespace dictionary::book_language

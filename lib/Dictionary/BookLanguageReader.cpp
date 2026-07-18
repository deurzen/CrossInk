#include "BookLanguageReader.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace dictionary::book_language {
namespace {
constexpr uint8_t kKnownCandidateFlags = 0x3F;

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
  return source.readAt && source.size <= std::numeric_limits<uint32_t>::max();
}
}  // namespace

bool BookLanguageReader::open(const RandomAccessSource& source, ReaderError& error) {
  open_ = false;
  header_ = {};
  source_ = {};
  shardDirectoryCacheStart_ = UINT32_MAX;
  blobCacheStart_ = UINT32_MAX;
  candidateCursor_ = 0;
  activeBlobOffset_ = UINT32_MAX;
  shardDirectoryCacheLength_ = 0;
  blobCacheLength_ = 0;
  nextCandidateIndex_ = 0;
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
    error = ReaderError::HEADER_INVALID;
    return false;
  }
  source_ = source;
  open_ = true;
  return true;
}

bool BookLanguageReader::readCached(const uint32_t offset, const size_t length, const uint32_t sectionEnd,
                                    uint8_t* cache, const size_t capacity, uint32_t& cacheStart, uint16_t& cacheLength,
                                    void* output) const {
  if (length > capacity || static_cast<uint64_t>(offset) + length > sectionEnd) return false;
  if (cacheLength == 0 || offset < cacheStart || static_cast<uint64_t>(offset) + length > cacheStart + cacheLength) {
    cacheStart = offset;
    cacheLength = static_cast<uint16_t>(std::min<size_t>(capacity, sectionEnd - offset));
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
  if (!readCached(offset, sizeof(data), header_.shardBlobOffset, shardDirectoryCache_, sizeof(shardDirectoryCache_),
                  shardDirectoryCacheStart_, shardDirectoryCacheLength_, data)) {
    error = ReaderError::READ_FAILED;
    return false;
  }
  out.blobOffset = readU32(data);
  out.blobLength = readU16(data + 4);
  out.recordCount = readU16(data + 6);
  out.sourceTokenStart = readU32(data + 8);
  out.sourceTokenEnd = readU32(data + 12);
  const uint32_t reserved = readU32(data + 16);
  const uint32_t blobSectionSize = header_.localLemmaTableOffset - header_.shardBlobOffset;
  if (reserved != 0 || out.blobLength > kMaxShardBlobSize ||
      static_cast<uint64_t>(out.blobOffset) + out.blobLength > blobSectionSize ||
      out.recordCount > out.blobLength / kInlineCandidateHeaderSize || out.sourceTokenStart > out.sourceTokenEnd) {
    out = {};
    error = ReaderError::SHARD_RECORD_INVALID;
    return false;
  }
  return true;
}

bool BookLanguageReader::readCandidate(const ShardDirectoryRecord& shard, const uint16_t index,
                                       const InlineCandidate*& out, ReaderError& error) const {
  out = nullptr;
  error = ReaderError::NONE;
  if (!open_) {
    error = ReaderError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (index >= shard.recordCount) {
    error = ReaderError::CANDIDATE_INDEX_OUT_OF_RANGE;
    return false;
  }
  const uint32_t blobStart = header_.shardBlobOffset + shard.blobOffset;
  const uint32_t blobEnd = blobStart + shard.blobLength;
  if (activeBlobOffset_ != blobStart || index < nextCandidateIndex_) {
    activeBlobOffset_ = blobStart;
    candidateCursor_ = blobStart;
    nextCandidateIndex_ = 0;
    blobCacheLength_ = 0;
  }
  while (nextCandidateIndex_ < index) {
    uint8_t skipped[kInlineCandidateHeaderSize]{};
    if (!readCached(candidateCursor_, sizeof(skipped), blobEnd, blobCache_, sizeof(blobCache_), blobCacheStart_,
                    blobCacheLength_, skipped)) {
      error = ReaderError::READ_FAILED;
      return false;
    }
    const uint16_t size = readU16(skipped + 8);
    if (size < kInlineCandidateHeaderSize || (size & 3U) != 0 || candidateCursor_ + size > blobEnd) {
      error = ReaderError::CANDIDATE_RECORD_INVALID;
      return false;
    }
    candidateCursor_ += size;
    ++nextCandidateIndex_;
  }

  uint8_t fixed[kInlineCandidateHeaderSize]{};
  if (!readCached(candidateCursor_, sizeof(fixed), blobEnd, blobCache_, sizeof(blobCache_), blobCacheStart_,
                  blobCacheLength_, fixed)) {
    error = ReaderError::READ_FAILED;
    return false;
  }
  candidate_ = {};
  candidate_.surfaceHash = readU64(fixed);
  candidate_.recordSize = readU16(fixed + 8);
  candidate_.surfaceLength = fixed[10];
  candidate_.analysisCount = fixed[11];
  candidate_.flags = fixed[12];
  candidate_.difficulty = fixed[13];
  candidate_.confidence = readU16(fixed + 14);
  candidate_.grammarDescriptor = readU32(fixed + 16);
  const uint32_t contentSize =
      kInlineCandidateHeaderSize + candidate_.analysisCount * sizeof(uint16_t) + candidate_.surfaceLength;
  const uint32_t expectedRecordSize = (contentSize + 3U) & ~3U;
  if (candidate_.recordSize != expectedRecordSize || candidateCursor_ + candidate_.recordSize > blobEnd ||
      candidate_.surfaceLength == 0 || candidate_.analysisCount == 0 || candidate_.analysisCount > kMaxInlineAnalyses ||
      (candidate_.flags & ~kKnownCandidateFlags) != 0 || candidate_.confidence > 1000) {
    error = ReaderError::CANDIDATE_RECORD_INVALID;
    return false;
  }
  if (!isGrammarDescriptorValid(candidate_.grammarDescriptor)) {
    error = ReaderError::GRAMMAR_DESCRIPTOR_INVALID;
    return false;
  }
  uint8_t analyses[kMaxInlineAnalyses * sizeof(uint16_t)]{};
  if (!readCached(candidateCursor_ + kInlineCandidateHeaderSize, candidate_.analysisCount * sizeof(uint16_t), blobEnd,
                  blobCache_, sizeof(blobCache_), blobCacheStart_, blobCacheLength_, analyses)) {
    error = ReaderError::READ_FAILED;
    return false;
  }
  for (uint8_t analysis = 0; analysis < candidate_.analysisCount; ++analysis) {
    candidate_.localLemmaIds[analysis] = readU16(analyses + analysis * 2U);
    if (candidate_.localLemmaIds[analysis] >= header_.localLemmaCount) {
      error = ReaderError::LOCAL_LEMMA_ID_OUT_OF_RANGE;
      return false;
    }
  }
  const uint32_t textOffset = candidateCursor_ + kInlineCandidateHeaderSize + candidate_.analysisCount * 2U;
  if (!readCached(textOffset, candidate_.surfaceLength, blobEnd, blobCache_, sizeof(blobCache_), blobCacheStart_,
                  blobCacheLength_, candidate_.surface)) {
    error = ReaderError::READ_FAILED;
    return false;
  }
  candidate_.surface[candidate_.surfaceLength] = '\0';
  if (fnv1a64(std::string_view(candidate_.surface, candidate_.surfaceLength)) != candidate_.surfaceHash) {
    error = ReaderError::CANDIDATE_RECORD_INVALID;
    return false;
  }
  const uint8_t paddingLength = static_cast<uint8_t>(candidate_.recordSize - contentSize);
  uint8_t padding[3]{};
  if (paddingLength > 0 &&
      (!readCached(candidateCursor_ + contentSize, paddingLength, blobEnd, blobCache_, sizeof(blobCache_),
                   blobCacheStart_, blobCacheLength_, padding) ||
       std::any_of(padding, padding + paddingLength, [](const uint8_t value) { return value != 0; }))) {
    error = ReaderError::CANDIDATE_RECORD_INVALID;
    return false;
  }
  candidateCursor_ += candidate_.recordSize;
  ++nextCandidateIndex_;
  if (nextCandidateIndex_ == shard.recordCount && candidateCursor_ != blobEnd) {
    error = ReaderError::CANDIDATE_RECORD_INVALID;
    return false;
  }
  out = &candidate_;
  return true;
}

bool BookLanguageReader::readGlobalLexemeId(const uint16_t localLemmaId, uint32_t& globalLexemeId,
                                            ReaderError& error) const {
  globalLexemeId = 0;
  error = ReaderError::NONE;
  if (!open_ || localLemmaId >= header_.localLemmaCount) {
    error = open_ ? ReaderError::LOCAL_LEMMA_ID_OUT_OF_RANGE : ReaderError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (localLemmaCacheCount_ == 0 || localLemmaId < localLemmaCacheFirst_ ||
      localLemmaId >= static_cast<uint32_t>(localLemmaCacheFirst_) + localLemmaCacheCount_) {
    localLemmaCacheFirst_ = localLemmaId;
    const uint32_t remaining = header_.localLemmaCount - localLemmaId;
    localLemmaCacheCount_ = static_cast<uint8_t>(std::min<uint32_t>(remaining, 64));
    const uint32_t offset = header_.localLemmaTableOffset + localLemmaId * kLocalLemmaRecordSize;
    if (!source_.readAt(source_.context, offset, localLemmaCache_, localLemmaCacheCount_ * kLocalLemmaRecordSize)) {
      localLemmaCacheCount_ = 0;
      error = ReaderError::READ_FAILED;
      return false;
    }
  }
  globalLexemeId = readU32(localLemmaCache_ + (localLemmaId - localLemmaCacheFirst_) * kLocalLemmaRecordSize);
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
    case ReaderError::GRAMMAR_DESCRIPTOR_INVALID:
      return "grammar descriptor invalid";
    case ReaderError::LOCAL_LEMMA_ID_OUT_OF_RANGE:
      return "local lemma id out of range";
  }
  return "unknown";
}

}  // namespace dictionary::book_language

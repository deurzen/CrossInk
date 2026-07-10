#pragma once

#include <cstddef>
#include <cstdint>

#include "Crc32.h"

namespace dictionary::book_language {

using dictionary::updateCrc32;

constexpr uint16_t kFormatVersion = 3;
constexpr size_t kHeaderSize = 108;
constexpr uint32_t kMaxFileSize = 64U * 1024U * 1024U;
constexpr uint16_t kMaxSpineCount = 4096;
constexpr uint32_t kMaxShardCount = 65535;
constexpr uint32_t kMaxShardRecordCount = 1000000;
constexpr uint32_t kMaxLocalLemmaCount = 32768;
constexpr uint32_t kMaxShardBlobSize = 24U * 1024U;
constexpr uint8_t kMaxInlineAnalyses = 8;

constexpr uint32_t kSpineRecordSize = 8;
constexpr uint32_t kShardDirectoryRecordSize = 20;
constexpr uint32_t kInlineCandidateHeaderSize = 16;
constexpr uint32_t kLocalLemmaRecordSize = 4;

struct Header {
  uint16_t formatVersion = 0;
  uint16_t headerSize = 0;
  uint32_t flags = 0;
  uint16_t tokenizerVersion = 0;
  uint16_t analyzerVersion = 0;
  uint8_t dictionaryBundleUuid[16]{};
  char sourceLanguage[8]{};
  char targetLanguage[8]{};
  uint16_t spineCount = 0;
  uint32_t shardCount = 0;
  uint32_t shardRecordCount = 0;
  uint32_t localLemmaCount = 0;
  uint32_t spineDirectoryOffset = 0;
  uint32_t shardDirectoryOffset = 0;
  uint32_t shardBlobOffset = 0;
  uint32_t localLemmaTableOffset = 0;
  uint32_t metadataOffset = 0;
  uint32_t fileSize = 0;
  uint32_t payloadCrc32 = 0;
  uint32_t headerCrc32 = 0;
};

enum class FormatError : uint8_t {
  NONE = 0,
  HEADER_TRUNCATED,
  BAD_MAGIC,
  UNSUPPORTED_VERSION,
  BAD_HEADER_SIZE,
  BAD_HEADER_CRC,
  UNSUPPORTED_FLAGS,
  RESERVED_FIELD_NONZERO,
  INVALID_DICTIONARY_UUID,
  INVALID_LANGUAGE,
  COUNT_OUT_OF_RANGE,
  FILE_SIZE_OUT_OF_RANGE,
  FILE_SIZE_MISMATCH,
  OFFSET_UNALIGNED,
  OFFSET_ORDER_INVALID,
  TABLE_OUT_OF_BOUNDS,
  BAD_PAYLOAD_CRC,
};

// Parses and validates only the fixed header. This function allocates no memory.
// actualFileSize is the size reported by storage and may exceed uint32_t.
bool parseHeader(const uint8_t* data, size_t dataSize, uint64_t actualFileSize, Header& out, FormatError& error);

// Validates the payload when the complete artifact is already available.
// Firmware file readers should compute the same CRC incrementally instead of
// allocating the whole artifact.
bool validatePayload(const uint8_t* fileData, size_t dataSize, const Header& header, FormatError& error);

// Incremental validator used while extracting or scanning an artifact. It owns
// only the fixed 108-byte header and CRC state; payload bytes are never retained.
class StreamValidator {
 public:
  bool write(const uint8_t* data, size_t length);
  bool finish(Header& out, FormatError& error) const;
  uint64_t bytesReceived() const { return bytesReceived_; }

 private:
  uint8_t headerBytes_[kHeaderSize]{};
  size_t headerBytesReceived_ = 0;
  uint64_t bytesReceived_ = 0;
  uint32_t payloadCrc32_ = 0;
  bool sizeExceeded_ = false;
};

const char* formatErrorName(FormatError error);

}  // namespace dictionary::book_language

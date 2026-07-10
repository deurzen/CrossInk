#include "BookLanguageFormat.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace dictionary::book_language {
namespace {

constexpr uint8_t kMagic[] = {'C', 'X', 'L', 'G'};
constexpr uint32_t kKnownFlags = 0;
constexpr size_t kHeaderCrcOffset = 104;

uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

bool hasNonzeroByte(const uint8_t* data, const size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (data[i] != 0) return true;
  }
  return false;
}

bool isAsciiLetter(const uint8_t value) {
  return (value >= static_cast<uint8_t>('A') && value <= static_cast<uint8_t>('Z')) ||
         (value >= static_cast<uint8_t>('a') && value <= static_cast<uint8_t>('z'));
}

bool isLanguageCodeValid(const char (&language)[8]) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(language);
  if (!isAsciiLetter(bytes[0])) return false;

  bool foundTerminator = false;
  for (size_t i = 1; i < sizeof(language); ++i) {
    const uint8_t value = bytes[i];
    if (value == 0) {
      foundTerminator = true;
      for (size_t padding = i + 1; padding < sizeof(language); ++padding) {
        if (bytes[padding] != 0) return false;
      }
      break;
    }
    if (!isAsciiLetter(value) && value != static_cast<uint8_t>('-') &&
        !(value >= static_cast<uint8_t>('0') && value <= static_cast<uint8_t>('9'))) {
      return false;
    }
  }
  return foundTerminator;
}

bool multiplyFits(const uint32_t count, const uint32_t recordSize, uint64_t& bytes) {
  bytes = static_cast<uint64_t>(count) * recordSize;
  return bytes <= std::numeric_limits<uint32_t>::max();
}

bool tableFits(const uint32_t offset, const uint32_t count, const uint32_t recordSize, const uint32_t nextOffset) {
  uint64_t bytes = 0;
  if (!multiplyFits(count, recordSize, bytes)) return false;
  return static_cast<uint64_t>(offset) + bytes <= nextOffset;
}

bool offsetsAreAligned(const Header& header) {
  const uint32_t offsets[] = {header.spineDirectoryOffset,  header.shardDirectoryOffset,     header.shardRecordsOffset,
                              header.localLemmaTableOffset, header.globalToLocalTableOffset, header.surfaceDetailOffset,
                              header.metadataOffset};
  return std::all_of(offsets, offsets + (sizeof(offsets) / sizeof(offsets[0])),
                     [](const uint32_t offset) { return (offset & 0x3U) == 0; });
}

bool offsetsAreOrdered(const Header& header) {
  return header.spineDirectoryOffset >= header.headerSize &&
         header.shardDirectoryOffset >= header.spineDirectoryOffset &&
         header.shardRecordsOffset >= header.shardDirectoryOffset &&
         header.localLemmaTableOffset >= header.shardRecordsOffset &&
         header.globalToLocalTableOffset >= header.localLemmaTableOffset &&
         header.surfaceDetailOffset >= header.globalToLocalTableOffset &&
         header.metadataOffset >= header.surfaceDetailOffset && header.fileSize >= header.metadataOffset;
}

}  // namespace

bool parseHeader(const uint8_t* data, const size_t dataSize, const uint64_t actualFileSize, Header& out,
                 FormatError& error) {
  out = {};
  error = FormatError::NONE;
  if (data == nullptr || dataSize < kHeaderSize) {
    error = FormatError::HEADER_TRUNCATED;
    return false;
  }
  if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
    error = FormatError::BAD_MAGIC;
    return false;
  }

  out.formatVersion = readU16(data + 4);
  out.headerSize = readU16(data + 6);
  out.flags = readU32(data + 8);
  out.tokenizerVersion = readU16(data + 12);
  out.analyzerVersion = readU16(data + 14);
  std::memcpy(out.dictionaryBundleUuid, data + 16, sizeof(out.dictionaryBundleUuid));
  std::memcpy(out.sourceLanguage, data + 32, sizeof(out.sourceLanguage));
  std::memcpy(out.targetLanguage, data + 40, sizeof(out.targetLanguage));
  out.spineCount = readU16(data + 48);
  const uint16_t reserved = readU16(data + 50);
  out.shardCount = readU32(data + 52);
  out.shardRecordCount = readU32(data + 56);
  out.localLemmaCount = readU32(data + 60);
  out.localSurfaceCount = readU32(data + 64);
  out.spineDirectoryOffset = readU32(data + 68);
  out.shardDirectoryOffset = readU32(data + 72);
  out.shardRecordsOffset = readU32(data + 76);
  out.localLemmaTableOffset = readU32(data + 80);
  out.globalToLocalTableOffset = readU32(data + 84);
  out.surfaceDetailOffset = readU32(data + 88);
  out.metadataOffset = readU32(data + 92);
  out.fileSize = readU32(data + 96);
  out.payloadCrc32 = readU32(data + 100);
  out.headerCrc32 = readU32(data + kHeaderCrcOffset);

  if (out.formatVersion != kFormatVersion) {
    error = FormatError::UNSUPPORTED_VERSION;
    return false;
  }
  if (out.headerSize != kHeaderSize) {
    error = FormatError::BAD_HEADER_SIZE;
    return false;
  }
  if (dictionary::updateCrc32(0, data, kHeaderCrcOffset) != out.headerCrc32) {
    error = FormatError::BAD_HEADER_CRC;
    return false;
  }
  if ((out.flags & ~kKnownFlags) != 0) {
    error = FormatError::UNSUPPORTED_FLAGS;
    return false;
  }
  if (reserved != 0) {
    error = FormatError::RESERVED_FIELD_NONZERO;
    return false;
  }
  if (!hasNonzeroByte(out.dictionaryBundleUuid, sizeof(out.dictionaryBundleUuid))) {
    error = FormatError::INVALID_DICTIONARY_UUID;
    return false;
  }
  if (!isLanguageCodeValid(out.sourceLanguage) || !isLanguageCodeValid(out.targetLanguage)) {
    error = FormatError::INVALID_LANGUAGE;
    return false;
  }
  if (out.spineCount == 0 || out.spineCount > kMaxSpineCount || out.shardCount > kMaxShardCount ||
      out.shardRecordCount > kMaxShardRecordCount || out.localLemmaCount > kMaxLocalLemmaCount ||
      out.localSurfaceCount > kMaxLocalSurfaceCount) {
    error = FormatError::COUNT_OUT_OF_RANGE;
    return false;
  }
  if (out.fileSize < kHeaderSize || out.fileSize > kMaxFileSize) {
    error = FormatError::FILE_SIZE_OUT_OF_RANGE;
    return false;
  }
  if (actualFileSize != out.fileSize) {
    error = FormatError::FILE_SIZE_MISMATCH;
    return false;
  }
  if (!offsetsAreAligned(out)) {
    error = FormatError::OFFSET_UNALIGNED;
    return false;
  }
  if (!offsetsAreOrdered(out)) {
    error = FormatError::OFFSET_ORDER_INVALID;
    return false;
  }
  if (!tableFits(out.spineDirectoryOffset, out.spineCount, kSpineRecordSize, out.shardDirectoryOffset) ||
      !tableFits(out.shardDirectoryOffset, out.shardCount, kShardDirectoryRecordSize, out.shardRecordsOffset) ||
      !tableFits(out.shardRecordsOffset, out.shardRecordCount, kShardCandidateRecordSize, out.localLemmaTableOffset) ||
      !tableFits(out.localLemmaTableOffset, out.localLemmaCount, kLocalLemmaRecordSize, out.globalToLocalTableOffset) ||
      !tableFits(out.globalToLocalTableOffset, out.localLemmaCount, kGlobalToLocalRecordSize,
                 out.surfaceDetailOffset)) {
    error = FormatError::TABLE_OUT_OF_BOUNDS;
    return false;
  }
  return true;
}

bool validatePayload(const uint8_t* fileData, const size_t dataSize, const Header& header, FormatError& error) {
  error = FormatError::NONE;
  if (fileData == nullptr || dataSize != header.fileSize || header.headerSize > dataSize) {
    error = FormatError::FILE_SIZE_MISMATCH;
    return false;
  }
  if (dictionary::updateCrc32(0, fileData + header.headerSize, dataSize - header.headerSize) != header.payloadCrc32) {
    error = FormatError::BAD_PAYLOAD_CRC;
    return false;
  }
  return true;
}

const char* formatErrorName(const FormatError error) {
  switch (error) {
    case FormatError::NONE:
      return "none";
    case FormatError::HEADER_TRUNCATED:
      return "header truncated";
    case FormatError::BAD_MAGIC:
      return "bad magic";
    case FormatError::UNSUPPORTED_VERSION:
      return "unsupported version";
    case FormatError::BAD_HEADER_SIZE:
      return "bad header size";
    case FormatError::BAD_HEADER_CRC:
      return "bad header crc";
    case FormatError::UNSUPPORTED_FLAGS:
      return "unsupported flags";
    case FormatError::RESERVED_FIELD_NONZERO:
      return "reserved field nonzero";
    case FormatError::INVALID_DICTIONARY_UUID:
      return "invalid dictionary uuid";
    case FormatError::INVALID_LANGUAGE:
      return "invalid language";
    case FormatError::COUNT_OUT_OF_RANGE:
      return "count out of range";
    case FormatError::FILE_SIZE_OUT_OF_RANGE:
      return "file size out of range";
    case FormatError::FILE_SIZE_MISMATCH:
      return "file size mismatch";
    case FormatError::OFFSET_UNALIGNED:
      return "offset unaligned";
    case FormatError::OFFSET_ORDER_INVALID:
      return "offset order invalid";
    case FormatError::TABLE_OUT_OF_BOUNDS:
      return "table out of bounds";
    case FormatError::BAD_PAYLOAD_CRC:
      return "bad payload crc";
  }
  return "unknown";
}

}  // namespace dictionary::book_language

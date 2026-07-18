#include "ContextualRuntimeFormat.h"

#include <cstring>
#include <limits>

#include "Crc32.h"

namespace dictionary::contextual {
namespace {

constexpr uint8_t kCanonicalMagic[] = {'C', 'X', 'C', 'L'};
constexpr uint8_t kDefinitionMagic[] = {'C', 'X', 'D', 'S'};
constexpr uint8_t kKnownLexemeFlags = 0x03;
constexpr uint8_t kMaxPartOfSpeech = 15;

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

bool hasNonzeroByte(const uint8_t* data, const size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (data[i] != 0) return true;
  }
  return false;
}

bool isAllZero(const uint8_t* data, const size_t length) { return !hasNonzeroByte(data, length); }

bool isAsciiLetter(const uint8_t value) {
  return (value >= static_cast<uint8_t>('A') && value <= static_cast<uint8_t>('Z')) ||
         (value >= static_cast<uint8_t>('a') && value <= static_cast<uint8_t>('z'));
}

bool isLanguageValid(const char (&language)[8]) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(language);
  if (!isAsciiLetter(bytes[0])) return false;
  for (size_t i = 1; i < sizeof(language); ++i) {
    const uint8_t value = bytes[i];
    if (value == 0) {
      for (size_t padding = i + 1; padding < sizeof(language); ++padding) {
        if (bytes[padding] != 0) return false;
      }
      return true;
    }
    if (!isAsciiLetter(value) && value != static_cast<uint8_t>('-') &&
        !(value >= static_cast<uint8_t>('0') && value <= static_cast<uint8_t>('9'))) {
      return false;
    }
  }
  return false;
}

bool isUtf8(const uint8_t* data, const size_t length) {
  size_t offset = 0;
  while (offset < length) {
    const uint8_t first = data[offset++];
    if (first < 0x80U) continue;
    uint8_t continuationCount = 0;
    uint32_t codepoint = 0;
    if ((first & 0xE0U) == 0xC0U) {
      continuationCount = 1;
      codepoint = first & 0x1FU;
      if (codepoint < 2) return false;
    } else if ((first & 0xF0U) == 0xE0U) {
      continuationCount = 2;
      codepoint = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
      continuationCount = 3;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (offset + continuationCount > length) return false;
    for (uint8_t i = 0; i < continuationCount; ++i) {
      const uint8_t value = data[offset++];
      if ((value & 0xC0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (value & 0x3FU);
    }
    if ((continuationCount == 2 && codepoint < 0x800U) || (continuationCount == 3 && codepoint < 0x10000U) ||
        codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
      return false;
    }
  }
  return true;
}

bool isLabelValid(const char (&label)[32]) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(label);
  size_t length = 0;
  while (length < sizeof(label) && bytes[length] != 0) ++length;
  if (length == 0 || length == sizeof(label) || !isUtf8(bytes, length)) return false;
  for (size_t i = length + 1; i < sizeof(label); ++i) {
    if (bytes[i] != 0) return false;
  }
  return true;
}

bool rangeFits(const uint32_t offset, const uint32_t length, const uint32_t fileSize) {
  return static_cast<uint64_t>(offset) + length <= fileSize;
}

bool decodeCanonicalLexeme(const uint8_t* data, const uint32_t headwordsFileSize, CanonicalLexemeRecord& out) {
  out.headwordOffset = readU32(data);
  out.keyHash = readU64(data + 4);
  out.headwordLength = readU16(data + 12);
  out.partOfSpeech = data[14];
  out.flags = data[15];
  if (out.headwordLength == 0 || out.headwordLength > kMaxCanonicalHeadwordBytes ||
      !rangeFits(out.headwordOffset, out.headwordLength, headwordsFileSize) || out.keyHash == 0 ||
      out.partOfSpeech > kMaxPartOfSpeech || (out.flags & ~kKnownLexemeFlags) != 0) {
    out = {};
    return false;
  }
  return true;
}

bool validateRuntimeCrc(const RandomAccessSource& source, const uint32_t expectedCrc, uint8_t* scratch,
                        const size_t scratchSize, RuntimeFormatError& error) {
  PackageError packageError;
  if (validateSourceCrc(source, expectedCrc, scratch, scratchSize, packageError)) return true;
  if (packageError == PackageError::OUTPUT_BUFFER_TOO_SMALL) {
    error = RuntimeFormatError::OUTPUT_BUFFER_TOO_SMALL;
  } else if (packageError == PackageError::BAD_FILE_CRC) {
    error = RuntimeFormatError::BAD_FILE_CRC;
  } else {
    error = RuntimeFormatError::PAYLOAD_READ_FAILED;
  }
  return false;
}

bool decodeDefinitionIndex(const uint8_t* data, const uint32_t entriesFileSize, DefinitionIndexRecord& out) {
  out.entryOffset = readU32(data);
  out.entryLength = readU32(data + 4);
  if ((out.entryLength == 0 && out.entryOffset != 0) || out.entryLength > kMaxDefinitionEntryBytes ||
      !rangeFits(out.entryOffset, out.entryLength, entriesFileSize) || (out.entryLength != 0 && out.entryLength < 4)) {
    out = {};
    return false;
  }
  return true;
}

}  // namespace

bool CanonicalLexiconReader::open(const RandomAccessSource& meta, const RandomAccessSource& lexemes,
                                  const RandomAccessSource& headwords, RuntimeFormatError& error) {
  open_ = false;
  metadata_ = {};
  lexemes_ = {};
  headwords_ = {};
  error = RuntimeFormatError::NONE;
  if (!sourceCanRead(meta) || !sourceCanRead(lexemes) || !sourceCanRead(headwords)) {
    error = RuntimeFormatError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (meta.size < kCanonicalMetaSize) {
    error = RuntimeFormatError::META_TRUNCATED;
    return false;
  }
  if (meta.size != kCanonicalMetaSize) {
    error = RuntimeFormatError::BAD_META_SIZE;
    return false;
  }

  uint8_t data[kCanonicalMetaSize]{};
  if (!meta.readAt(meta.context, 0, data, sizeof(data))) {
    error = RuntimeFormatError::META_TRUNCATED;
    return false;
  }
  if (std::memcmp(data, kCanonicalMagic, sizeof(kCanonicalMagic)) != 0) {
    error = RuntimeFormatError::BAD_MAGIC;
    return false;
  }
  if (readU16(data + 4) != kCanonicalFormatVersion) {
    error = RuntimeFormatError::UNSUPPORTED_VERSION;
    return false;
  }
  if (readU16(data + 6) != kCanonicalMetaSize) {
    error = RuntimeFormatError::BAD_META_SIZE;
    return false;
  }
  if (dictionary::updateCrc32(0, data, 108) != readU32(data + 108)) {
    error = RuntimeFormatError::BAD_META_CRC;
    return false;
  }
  if (readU32(data + 8) != 0) {
    error = RuntimeFormatError::UNSUPPORTED_FLAGS;
    return false;
  }
  if (!isAllZero(data + 92, 16)) {
    error = RuntimeFormatError::RESERVED_FIELD_NONZERO;
    return false;
  }

  std::memcpy(metadata_.canonicalUuid, data + 12, sizeof(metadata_.canonicalUuid));
  std::memcpy(metadata_.sourceLanguage, data + 28, sizeof(metadata_.sourceLanguage));
  metadata_.lexemeCount = readU32(data + 36);
  const uint16_t recordSize = readU16(data + 40);
  const uint16_t posVersion = readU16(data + 42);
  metadata_.lexemesFileSize = readU32(data + 44);
  metadata_.headwordsFileSize = readU32(data + 48);
  metadata_.lexemesCrc32 = readU32(data + 52);
  metadata_.headwordsCrc32 = readU32(data + 56);
  std::memcpy(metadata_.payloadSha256, data + 60, sizeof(metadata_.payloadSha256));

  if (!hasNonzeroByte(metadata_.canonicalUuid, sizeof(metadata_.canonicalUuid)) ||
      !hasNonzeroByte(metadata_.payloadSha256, sizeof(metadata_.payloadSha256))) {
    error = RuntimeFormatError::INVALID_UUID;
    return false;
  }
  if (!isLanguageValid(metadata_.sourceLanguage)) {
    error = RuntimeFormatError::INVALID_LANGUAGE;
    return false;
  }
  if (metadata_.lexemeCount == 0 || metadata_.lexemeCount > kMaxCanonicalLexemes) {
    error = RuntimeFormatError::COUNT_OUT_OF_RANGE;
    return false;
  }
  if (recordSize != kCanonicalLexemeRecordSize || posVersion != kCanonicalPosVersion) {
    error = RuntimeFormatError::RECORD_SIZE_INVALID;
    return false;
  }
  if (metadata_.lexemesFileSize != static_cast<uint64_t>(metadata_.lexemeCount) * recordSize ||
      metadata_.headwordsFileSize == 0 || metadata_.headwordsFileSize > kMaxCanonicalHeadwordsSize) {
    error = RuntimeFormatError::FILE_SIZE_OUT_OF_RANGE;
    return false;
  }
  if (lexemes.size != metadata_.lexemesFileSize || headwords.size != metadata_.headwordsFileSize) {
    error = RuntimeFormatError::FILE_SIZE_MISMATCH;
    return false;
  }

  lexemes_ = lexemes;
  headwords_ = headwords;
  open_ = true;
  return true;
}

bool CanonicalLexiconReader::validatePayloadCrc(uint8_t* scratch, const size_t scratchSize,
                                                RuntimeFormatError& error) const {
  error = RuntimeFormatError::NONE;
  if (!open_) {
    error = RuntimeFormatError::SOURCE_UNAVAILABLE;
    return false;
  }
  return validateRuntimeCrc(lexemes_, metadata_.lexemesCrc32, scratch, scratchSize, error) &&
         validateRuntimeCrc(headwords_, metadata_.headwordsCrc32, scratch, scratchSize, error);
}

bool CanonicalLexiconReader::validateLexemes(uint8_t* scratch, const size_t scratchSize,
                                             RuntimeFormatError& error) const {
  error = RuntimeFormatError::NONE;
  if (!open_) {
    error = RuntimeFormatError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (scratch == nullptr || scratchSize < kCanonicalLexemeRecordSize) {
    error = RuntimeFormatError::OUTPUT_BUFFER_TOO_SMALL;
    return false;
  }

  const size_t recordsPerRead = scratchSize / kCanonicalLexemeRecordSize;
  uint32_t canonicalId = 0;
  while (canonicalId < metadata_.lexemeCount) {
    const uint32_t remaining = metadata_.lexemeCount - canonicalId;
    const size_t recordCount = remaining < recordsPerRead ? remaining : recordsPerRead;
    const size_t byteCount = recordCount * kCanonicalLexemeRecordSize;
    if (!lexemes_.readAt(lexemes_.context, canonicalId * kCanonicalLexemeRecordSize, scratch, byteCount)) {
      error = RuntimeFormatError::RECORD_READ_FAILED;
      return false;
    }
    for (size_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
      CanonicalLexemeRecord record;
      if (!decodeCanonicalLexeme(scratch + recordIndex * kCanonicalLexemeRecordSize, metadata_.headwordsFileSize,
                                 record)) {
        error = RuntimeFormatError::RECORD_INVALID;
        return false;
      }
    }
    canonicalId += recordCount;
  }
  return true;
}

bool CanonicalLexiconReader::readLexeme(const uint32_t canonicalId, CanonicalLexemeRecord& out,
                                        RuntimeFormatError& error) const {
  out = {};
  error = RuntimeFormatError::NONE;
  if (!open_) {
    error = RuntimeFormatError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (canonicalId >= metadata_.lexemeCount) {
    error = RuntimeFormatError::RECORD_ID_OUT_OF_RANGE;
    return false;
  }
  uint8_t data[kCanonicalLexemeRecordSize]{};
  if (!lexemes_.readAt(lexemes_.context, canonicalId * kCanonicalLexemeRecordSize, data, sizeof(data))) {
    error = RuntimeFormatError::RECORD_READ_FAILED;
    return false;
  }
  if (!decodeCanonicalLexeme(data, metadata_.headwordsFileSize, out)) {
    error = RuntimeFormatError::RECORD_INVALID;
    return false;
  }
  return true;
}

bool CanonicalLexiconReader::readHeadword(const CanonicalLexemeRecord& lexeme, char* output, const size_t capacity,
                                          size_t& outputLength, RuntimeFormatError& error) const {
  outputLength = 0;
  error = RuntimeFormatError::NONE;
  if (!open_) {
    error = RuntimeFormatError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (lexeme.headwordLength == 0 || lexeme.headwordLength > kMaxCanonicalHeadwordBytes ||
      !rangeFits(lexeme.headwordOffset, lexeme.headwordLength, metadata_.headwordsFileSize)) {
    error = RuntimeFormatError::RECORD_INVALID;
    return false;
  }
  if (output == nullptr || capacity <= lexeme.headwordLength) {
    error = RuntimeFormatError::OUTPUT_BUFFER_TOO_SMALL;
    return false;
  }
  if (!headwords_.readAt(headwords_.context, lexeme.headwordOffset, output, lexeme.headwordLength)) {
    error = RuntimeFormatError::HEADWORD_READ_FAILED;
    return false;
  }
  output[lexeme.headwordLength] = '\0';
  outputLength = lexeme.headwordLength;
  return true;
}

bool DefinitionSourceReader::open(const RandomAccessSource& meta, const RandomAccessSource& index,
                                  const RandomAccessSource& entries, const uint8_t expectedCanonicalUuid[16],
                                  const uint32_t expectedCanonicalCount, RuntimeFormatError& error) {
  open_ = false;
  metadata_ = {};
  index_ = {};
  entries_ = {};
  error = RuntimeFormatError::NONE;
  if (!sourceCanRead(meta) || !sourceCanRead(index) || !sourceCanRead(entries)) {
    error = RuntimeFormatError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (meta.size < kDefinitionMetaSize) {
    error = RuntimeFormatError::META_TRUNCATED;
    return false;
  }
  if (meta.size != kDefinitionMetaSize) {
    error = RuntimeFormatError::BAD_META_SIZE;
    return false;
  }

  uint8_t data[kDefinitionMetaSize]{};
  if (!meta.readAt(meta.context, 0, data, sizeof(data))) {
    error = RuntimeFormatError::META_TRUNCATED;
    return false;
  }
  if (std::memcmp(data, kDefinitionMagic, sizeof(kDefinitionMagic)) != 0) {
    error = RuntimeFormatError::BAD_MAGIC;
    return false;
  }
  if (readU16(data + 4) != kDefinitionFormatVersion) {
    error = RuntimeFormatError::UNSUPPORTED_VERSION;
    return false;
  }
  if (readU16(data + 6) != kDefinitionMetaSize) {
    error = RuntimeFormatError::BAD_META_SIZE;
    return false;
  }
  if (dictionary::updateCrc32(0, data, 140) != readU32(data + 140)) {
    error = RuntimeFormatError::BAD_META_CRC;
    return false;
  }
  if (readU32(data + 8) != 0) {
    error = RuntimeFormatError::UNSUPPORTED_FLAGS;
    return false;
  }
  if (!isAllZero(data + 120, 20)) {
    error = RuntimeFormatError::RESERVED_FIELD_NONZERO;
    return false;
  }

  std::memcpy(metadata_.sourceUuid, data + 12, sizeof(metadata_.sourceUuid));
  std::memcpy(metadata_.canonicalUuid, data + 28, sizeof(metadata_.canonicalUuid));
  std::memcpy(metadata_.sourceLanguage, data + 44, sizeof(metadata_.sourceLanguage));
  std::memcpy(metadata_.targetLanguage, data + 52, sizeof(metadata_.targetLanguage));
  std::memcpy(metadata_.sourceLabel, data + 60, sizeof(metadata_.sourceLabel));
  metadata_.canonicalLexemeCount = readU32(data + 92);
  const uint16_t indexRecordSize = readU16(data + 96);
  const uint16_t entryVersion = readU16(data + 98);
  metadata_.indexFileSize = readU32(data + 100);
  metadata_.entriesFileSize = readU32(data + 104);
  metadata_.indexCrc32 = readU32(data + 108);
  metadata_.entriesCrc32 = readU32(data + 112);
  metadata_.coverageCount = readU32(data + 116);

  if (!hasNonzeroByte(metadata_.sourceUuid, sizeof(metadata_.sourceUuid)) ||
      !hasNonzeroByte(metadata_.canonicalUuid, sizeof(metadata_.canonicalUuid))) {
    error = RuntimeFormatError::INVALID_UUID;
    return false;
  }
  if (!isLanguageValid(metadata_.sourceLanguage) || !isLanguageValid(metadata_.targetLanguage)) {
    error = RuntimeFormatError::INVALID_LANGUAGE;
    return false;
  }
  if (!isLabelValid(metadata_.sourceLabel)) {
    error = RuntimeFormatError::INVALID_LABEL;
    return false;
  }
  if (metadata_.canonicalLexemeCount == 0 || metadata_.canonicalLexemeCount > kMaxCanonicalLexemes ||
      metadata_.coverageCount > metadata_.canonicalLexemeCount) {
    error = RuntimeFormatError::COUNT_OUT_OF_RANGE;
    return false;
  }
  if (indexRecordSize != kDefinitionIndexRecordSize || entryVersion != kDefinitionEntryVersion) {
    error = RuntimeFormatError::RECORD_SIZE_INVALID;
    return false;
  }
  if (metadata_.indexFileSize != static_cast<uint64_t>(metadata_.canonicalLexemeCount) * kDefinitionIndexRecordSize ||
      metadata_.indexFileSize > kMaxDefinitionIndexSize || metadata_.entriesFileSize > kMaxDefinitionEntriesSize) {
    error = RuntimeFormatError::FILE_SIZE_OUT_OF_RANGE;
    return false;
  }
  if (index.size != metadata_.indexFileSize || entries.size != metadata_.entriesFileSize) {
    error = RuntimeFormatError::FILE_SIZE_MISMATCH;
    return false;
  }
  if (expectedCanonicalUuid == nullptr ||
      std::memcmp(metadata_.canonicalUuid, expectedCanonicalUuid, sizeof(metadata_.canonicalUuid)) != 0 ||
      metadata_.canonicalLexemeCount != expectedCanonicalCount) {
    error = RuntimeFormatError::CANONICAL_MISMATCH;
    return false;
  }

  index_ = index;
  entries_ = entries;
  open_ = true;
  return true;
}

bool DefinitionSourceReader::validatePayloadCrc(uint8_t* scratch, const size_t scratchSize,
                                                RuntimeFormatError& error) const {
  error = RuntimeFormatError::NONE;
  if (!open_) {
    error = RuntimeFormatError::SOURCE_UNAVAILABLE;
    return false;
  }
  return validateRuntimeCrc(index_, metadata_.indexCrc32, scratch, scratchSize, error) &&
         validateRuntimeCrc(entries_, metadata_.entriesCrc32, scratch, scratchSize, error);
}

bool DefinitionSourceReader::readIndex(const uint32_t canonicalId, DefinitionIndexRecord& out,
                                       RuntimeFormatError& error) const {
  out = {};
  error = RuntimeFormatError::NONE;
  if (!open_) {
    error = RuntimeFormatError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (canonicalId >= metadata_.canonicalLexemeCount) {
    error = RuntimeFormatError::RECORD_ID_OUT_OF_RANGE;
    return false;
  }
  uint8_t data[kDefinitionIndexRecordSize]{};
  if (!index_.readAt(index_.context, canonicalId * kDefinitionIndexRecordSize, data, sizeof(data))) {
    error = RuntimeFormatError::RECORD_READ_FAILED;
    return false;
  }
  if (!decodeDefinitionIndex(data, metadata_.entriesFileSize, out)) {
    error = RuntimeFormatError::RECORD_INVALID;
    return false;
  }
  return true;
}

bool DefinitionSourceReader::validateIndex(uint8_t* scratch, const size_t scratchSize,
                                           RuntimeFormatError& error) const {
  error = RuntimeFormatError::NONE;
  if (!open_) {
    error = RuntimeFormatError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (scratch == nullptr || scratchSize < kDefinitionIndexRecordSize) {
    error = RuntimeFormatError::OUTPUT_BUFFER_TOO_SMALL;
    return false;
  }

  const size_t recordsPerRead = scratchSize / kDefinitionIndexRecordSize;
  uint32_t previousEnd = 0;
  uint32_t coverage = 0;
  uint32_t canonicalId = 0;
  while (canonicalId < metadata_.canonicalLexemeCount) {
    const uint32_t remaining = metadata_.canonicalLexemeCount - canonicalId;
    const size_t recordCount = remaining < recordsPerRead ? remaining : recordsPerRead;
    const size_t byteCount = recordCount * kDefinitionIndexRecordSize;
    if (!index_.readAt(index_.context, canonicalId * kDefinitionIndexRecordSize, scratch, byteCount)) {
      error = RuntimeFormatError::RECORD_READ_FAILED;
      return false;
    }
    for (size_t recordIndex = 0; recordIndex < recordCount; ++recordIndex) {
      DefinitionIndexRecord record;
      if (!decodeDefinitionIndex(scratch + recordIndex * kDefinitionIndexRecordSize, metadata_.entriesFileSize,
                                 record)) {
        error = RuntimeFormatError::RECORD_INVALID;
        return false;
      }
      if (!record.present()) continue;
      if (record.entryOffset < previousEnd) {
        error = RuntimeFormatError::INDEX_ORDER_INVALID;
        return false;
      }
      previousEnd = record.entryOffset + record.entryLength;
      ++coverage;
    }
    canonicalId += recordCount;
  }
  if (coverage != metadata_.coverageCount) {
    error = RuntimeFormatError::COVERAGE_MISMATCH;
    return false;
  }
  return true;
}

const char* runtimeFormatErrorName(const RuntimeFormatError error) {
  switch (error) {
    case RuntimeFormatError::NONE:
      return "none";
    case RuntimeFormatError::SOURCE_UNAVAILABLE:
      return "source unavailable";
    case RuntimeFormatError::META_TRUNCATED:
      return "meta truncated";
    case RuntimeFormatError::BAD_MAGIC:
      return "bad magic";
    case RuntimeFormatError::UNSUPPORTED_VERSION:
      return "unsupported version";
    case RuntimeFormatError::BAD_META_SIZE:
      return "bad meta size";
    case RuntimeFormatError::BAD_META_CRC:
      return "bad meta crc";
    case RuntimeFormatError::UNSUPPORTED_FLAGS:
      return "unsupported flags";
    case RuntimeFormatError::RESERVED_FIELD_NONZERO:
      return "reserved field nonzero";
    case RuntimeFormatError::INVALID_UUID:
      return "invalid uuid";
    case RuntimeFormatError::INVALID_LANGUAGE:
      return "invalid language";
    case RuntimeFormatError::INVALID_LABEL:
      return "invalid label";
    case RuntimeFormatError::COUNT_OUT_OF_RANGE:
      return "count out of range";
    case RuntimeFormatError::RECORD_SIZE_INVALID:
      return "record size invalid";
    case RuntimeFormatError::FILE_SIZE_OUT_OF_RANGE:
      return "file size out of range";
    case RuntimeFormatError::FILE_SIZE_MISMATCH:
      return "file size mismatch";
    case RuntimeFormatError::PAYLOAD_READ_FAILED:
      return "payload read failed";
    case RuntimeFormatError::BAD_FILE_CRC:
      return "bad file crc";
    case RuntimeFormatError::CANONICAL_MISMATCH:
      return "canonical mismatch";
    case RuntimeFormatError::RECORD_ID_OUT_OF_RANGE:
      return "record id out of range";
    case RuntimeFormatError::RECORD_READ_FAILED:
      return "record read failed";
    case RuntimeFormatError::RECORD_INVALID:
      return "record invalid";
    case RuntimeFormatError::OUTPUT_BUFFER_TOO_SMALL:
      return "output buffer too small";
    case RuntimeFormatError::HEADWORD_READ_FAILED:
      return "headword read failed";
    case RuntimeFormatError::INDEX_ORDER_INVALID:
      return "index order invalid";
    case RuntimeFormatError::COVERAGE_MISMATCH:
      return "coverage mismatch";
  }
  return "unknown";
}

}  // namespace dictionary::contextual

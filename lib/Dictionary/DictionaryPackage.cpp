#include "DictionaryPackage.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "Crc32.h"

namespace dictionary {
namespace {

constexpr uint8_t kMagic[] = {'C', 'X', 'D', 'M'};
constexpr uint32_t kKnownMetaFlags = 0;
constexpr uint8_t kKnownLexemeFlags = 0x03;
constexpr uint8_t kMaxPartOfSpeech = 15;
constexpr size_t kMetaCrcOffset = 76;

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

bool sourceCanRead(const RandomAccessSource& source) {
  return source.readAt != nullptr && source.size <= std::numeric_limits<uint32_t>::max();
}

bool rangeFits(const uint32_t offset, const uint32_t length, const uint32_t fileSize) {
  return static_cast<uint64_t>(offset) + length <= fileSize;
}

}  // namespace

bool DictionaryPackage::open(const RandomAccessSource& meta, const RandomAccessSource& lexemes,
                             const RandomAccessSource& headwords, const RandomAccessSource& entries,
                             PackageError& error) {
  open_ = false;
  metadata_ = {};
  lexemes_ = {};
  headwords_ = {};
  entries_ = {};
  error = PackageError::NONE;

  if (!sourceCanRead(meta) || !sourceCanRead(lexemes) || !sourceCanRead(headwords) || !sourceCanRead(entries)) {
    error = PackageError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (meta.size < kDictionaryMetaSize) {
    error = PackageError::META_TRUNCATED;
    return false;
  }
  if (meta.size != kDictionaryMetaSize) {
    error = PackageError::BAD_META_SIZE;
    return false;
  }

  uint8_t data[kDictionaryMetaSize]{};
  if (!meta.readAt(meta.context, 0, data, sizeof(data))) {
    error = PackageError::META_TRUNCATED;
    return false;
  }
  if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
    error = PackageError::BAD_MAGIC;
    return false;
  }
  if (readU16(data + 4) != kDictionaryPackageVersion) {
    error = PackageError::UNSUPPORTED_VERSION;
    return false;
  }
  if (readU16(data + 6) != kDictionaryMetaSize) {
    error = PackageError::BAD_META_SIZE;
    return false;
  }
  if (dictionary::updateCrc32(0, data, kMetaCrcOffset) != readU32(data + kMetaCrcOffset)) {
    error = PackageError::BAD_META_CRC;
    return false;
  }
  if ((readU32(data + 8) & ~kKnownMetaFlags) != 0) {
    error = PackageError::UNSUPPORTED_FLAGS;
    return false;
  }
  if (readU16(data + 50) != 0) {
    error = PackageError::RESERVED_FIELD_NONZERO;
    return false;
  }

  std::memcpy(metadata_.dictionaryBundleUuid, data + 12, sizeof(metadata_.dictionaryBundleUuid));
  std::memcpy(metadata_.sourceLanguage, data + 28, sizeof(metadata_.sourceLanguage));
  std::memcpy(metadata_.targetLanguage, data + 36, sizeof(metadata_.targetLanguage));
  metadata_.lexemeCount = readU32(data + 44);
  const uint16_t lexemeRecordSize = readU16(data + 48);
  metadata_.lexemesFileSize = readU32(data + 52);
  metadata_.headwordsFileSize = readU32(data + 56);
  metadata_.entriesFileSize = readU32(data + 60);
  metadata_.lexemesCrc32 = readU32(data + 64);
  metadata_.headwordsCrc32 = readU32(data + 68);
  metadata_.entriesCrc32 = readU32(data + 72);

  if (!hasNonzeroByte(metadata_.dictionaryBundleUuid, sizeof(metadata_.dictionaryBundleUuid))) {
    error = PackageError::INVALID_DICTIONARY_UUID;
    return false;
  }
  if (!isLanguageCodeValid(metadata_.sourceLanguage) || !isLanguageCodeValid(metadata_.targetLanguage)) {
    error = PackageError::INVALID_LANGUAGE;
    return false;
  }
  if (metadata_.lexemeCount == 0 || metadata_.lexemeCount > kMaxLexemeCount) {
    error = PackageError::COUNT_OUT_OF_RANGE;
    return false;
  }
  if (lexemeRecordSize != kLexemeRecordSize ||
      metadata_.lexemesFileSize != static_cast<uint64_t>(metadata_.lexemeCount) * kLexemeRecordSize ||
      metadata_.headwordsFileSize == 0 || metadata_.headwordsFileSize > kMaxHeadwordsFileSize ||
      metadata_.entriesFileSize == 0 || metadata_.entriesFileSize > kMaxEntriesFileSize) {
    error = PackageError::FILE_SIZE_OUT_OF_RANGE;
    return false;
  }
  if (lexemes.size != metadata_.lexemesFileSize || headwords.size != metadata_.headwordsFileSize ||
      entries.size != metadata_.entriesFileSize) {
    error = PackageError::FILE_SIZE_MISMATCH;
    return false;
  }

  lexemes_ = lexemes;
  headwords_ = headwords;
  entries_ = entries;
  open_ = true;
  return true;
}

bool DictionaryPackage::readLexeme(const uint32_t lexemeId, LexemeRecord& out, PackageError& error) const {
  out = {};
  error = PackageError::NONE;
  if (!open_) {
    error = PackageError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (lexemeId >= metadata_.lexemeCount) {
    error = PackageError::LEXEME_ID_OUT_OF_RANGE;
    return false;
  }

  uint8_t data[kLexemeRecordSize]{};
  const uint32_t offset = lexemeId * kLexemeRecordSize;
  if (!lexemes_.readAt(lexemes_.context, offset, data, sizeof(data))) {
    error = PackageError::LEXEME_READ_FAILED;
    return false;
  }

  out.headwordOffset = readU32(data);
  out.entryOffset = readU32(data + 4);
  out.entryLength = readU32(data + 8);
  out.lexemeKeyHash = readU64(data + 12);
  out.headwordLength = readU16(data + 20);
  out.partOfSpeech = data[22];
  out.flags = data[23];

  if (out.headwordLength == 0 || out.headwordLength > kMaxHeadwordBytes ||
      !rangeFits(out.headwordOffset, out.headwordLength, metadata_.headwordsFileSize) || out.entryLength == 0 ||
      out.entryLength > kMaxEntryBytes || !rangeFits(out.entryOffset, out.entryLength, metadata_.entriesFileSize) ||
      out.lexemeKeyHash == 0 || out.partOfSpeech > kMaxPartOfSpeech || (out.flags & ~kKnownLexemeFlags) != 0) {
    out = {};
    error = PackageError::LEXEME_RECORD_INVALID;
    return false;
  }
  return true;
}

bool DictionaryPackage::readHeadword(const LexemeRecord& lexeme, char* output, const size_t capacity,
                                     size_t& outputLength, PackageError& error) const {
  outputLength = 0;
  error = PackageError::NONE;
  if (!open_) {
    error = PackageError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (lexeme.headwordLength == 0 || lexeme.headwordLength > kMaxHeadwordBytes ||
      !rangeFits(lexeme.headwordOffset, lexeme.headwordLength, metadata_.headwordsFileSize)) {
    error = PackageError::LEXEME_RECORD_INVALID;
    return false;
  }
  if (output == nullptr || capacity <= lexeme.headwordLength) {
    error = PackageError::OUTPUT_BUFFER_TOO_SMALL;
    return false;
  }
  if (!headwords_.readAt(headwords_.context, lexeme.headwordOffset, output, lexeme.headwordLength)) {
    error = PackageError::HEADWORD_READ_FAILED;
    return false;
  }
  output[lexeme.headwordLength] = '\0';
  outputLength = lexeme.headwordLength;
  return true;
}

bool DictionaryPackage::getEntrySlice(const LexemeRecord& lexeme, EntrySlice& out, PackageError& error) const {
  out = {};
  error = PackageError::NONE;
  if (!open_) {
    error = PackageError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (lexeme.entryLength == 0 || lexeme.entryLength > kMaxEntryBytes ||
      !rangeFits(lexeme.entryOffset, lexeme.entryLength, metadata_.entriesFileSize)) {
    error = PackageError::LEXEME_RECORD_INVALID;
    return false;
  }
  out.offset = lexeme.entryOffset;
  out.length = lexeme.entryLength;
  return true;
}

bool DictionaryPackage::readEntryHeader(const EntrySlice& entry, EntryHeader& out, PackageError& error) const {
  out = {};
  error = PackageError::NONE;
  if (!open_) {
    error = PackageError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (entry.length < 4 || entry.length > kMaxEntryBytes ||
      !rangeFits(entry.offset, entry.length, metadata_.entriesFileSize)) {
    error = PackageError::ENTRY_RANGE_INVALID;
    return false;
  }

  uint8_t data[4]{};
  if (!entries_.readAt(entries_.context, entry.offset, data, sizeof(data))) {
    error = PackageError::ENTRY_READ_FAILED;
    return false;
  }
  out.version = data[0];
  out.flags = data[1];
  out.fieldCount = readU16(data + 2);
  if (out.version != 1 || out.flags != 0 || out.fieldCount == 0 || out.fieldCount > kMaxEntryFieldCount) {
    out = {};
    error = PackageError::ENTRY_HEADER_INVALID;
    return false;
  }
  return true;
}

bool DictionaryPackage::readEntryFieldHeader(const EntrySlice& entry, const uint32_t relativeOffset,
                                             EntryFieldHeader& out, PackageError& error) const {
  out = {};
  error = PackageError::NONE;
  if (!open_) {
    error = PackageError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (entry.length < 4 || entry.length > kMaxEntryBytes ||
      !rangeFits(entry.offset, entry.length, metadata_.entriesFileSize) || relativeOffset < 4 ||
      static_cast<uint64_t>(relativeOffset) + 4 > entry.length) {
    error = PackageError::ENTRY_RANGE_INVALID;
    return false;
  }

  uint8_t data[4]{};
  if (!entries_.readAt(entries_.context, entry.offset + relativeOffset, data, sizeof(data))) {
    error = PackageError::ENTRY_READ_FAILED;
    return false;
  }
  out.type = data[0];
  out.flags = data[1];
  out.length = readU16(data + 2);
  out.payloadOffset = relativeOffset + 4;
  if (out.type == 0 || out.flags != 0 || static_cast<uint64_t>(out.payloadOffset) + out.length > entry.length) {
    out = {};
    error = PackageError::ENTRY_FIELD_INVALID;
    return false;
  }
  return true;
}

bool DictionaryPackage::readEntryChunk(const EntrySlice& entry, const uint32_t relativeOffset, void* output,
                                       const size_t capacity, size_t& bytesRead, PackageError& error) const {
  bytesRead = 0;
  error = PackageError::NONE;
  if (!open_) {
    error = PackageError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (entry.length == 0 || entry.length > kMaxEntryBytes ||
      !rangeFits(entry.offset, entry.length, metadata_.entriesFileSize) || relativeOffset > entry.length) {
    error = PackageError::ENTRY_RANGE_INVALID;
    return false;
  }
  if (capacity == 0 || relativeOffset == entry.length) return true;
  if (output == nullptr) {
    error = PackageError::OUTPUT_BUFFER_TOO_SMALL;
    return false;
  }

  const size_t remaining = entry.length - relativeOffset;
  bytesRead = std::min(capacity, remaining);
  if (bytesRead > std::numeric_limits<uint32_t>::max() ||
      !entries_.readAt(entries_.context, entry.offset + relativeOffset, output, bytesRead)) {
    bytesRead = 0;
    error = PackageError::ENTRY_READ_FAILED;
    return false;
  }
  return true;
}

bool validateSourceCrc(const RandomAccessSource& source, const uint32_t expectedCrc, uint8_t* scratch,
                       const size_t scratchSize, PackageError& error) {
  error = PackageError::NONE;
  if (!sourceCanRead(source)) {
    error = PackageError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (scratch == nullptr || scratchSize == 0) {
    error = PackageError::OUTPUT_BUFFER_TOO_SMALL;
    return false;
  }

  uint32_t crc = 0;
  uint64_t offset = 0;
  while (offset < source.size) {
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(scratchSize, source.size - offset));
    if (!source.readAt(source.context, static_cast<uint32_t>(offset), scratch, chunk)) {
      error = PackageError::SOURCE_UNAVAILABLE;
      return false;
    }
    crc = dictionary::updateCrc32(crc, scratch, chunk);
    offset += chunk;
  }
  if (crc != expectedCrc) {
    error = PackageError::BAD_FILE_CRC;
    return false;
  }
  return true;
}

const char* packageErrorName(const PackageError error) {
  switch (error) {
    case PackageError::NONE:
      return "none";
    case PackageError::SOURCE_UNAVAILABLE:
      return "source unavailable";
    case PackageError::META_TRUNCATED:
      return "meta truncated";
    case PackageError::BAD_MAGIC:
      return "bad magic";
    case PackageError::UNSUPPORTED_VERSION:
      return "unsupported version";
    case PackageError::BAD_META_SIZE:
      return "bad meta size";
    case PackageError::BAD_META_CRC:
      return "bad meta crc";
    case PackageError::UNSUPPORTED_FLAGS:
      return "unsupported flags";
    case PackageError::RESERVED_FIELD_NONZERO:
      return "reserved field nonzero";
    case PackageError::INVALID_DICTIONARY_UUID:
      return "invalid dictionary uuid";
    case PackageError::INVALID_LANGUAGE:
      return "invalid language";
    case PackageError::COUNT_OUT_OF_RANGE:
      return "count out of range";
    case PackageError::FILE_SIZE_OUT_OF_RANGE:
      return "file size out of range";
    case PackageError::FILE_SIZE_MISMATCH:
      return "file size mismatch";
    case PackageError::LEXEME_ID_OUT_OF_RANGE:
      return "lexeme id out of range";
    case PackageError::LEXEME_READ_FAILED:
      return "lexeme read failed";
    case PackageError::LEXEME_RECORD_INVALID:
      return "lexeme record invalid";
    case PackageError::OUTPUT_BUFFER_TOO_SMALL:
      return "output buffer too small";
    case PackageError::HEADWORD_READ_FAILED:
      return "headword read failed";
    case PackageError::ENTRY_READ_FAILED:
      return "entry read failed";
    case PackageError::ENTRY_RANGE_INVALID:
      return "entry range invalid";
    case PackageError::ENTRY_HEADER_INVALID:
      return "entry header invalid";
    case PackageError::ENTRY_FIELD_INVALID:
      return "entry field invalid";
    case PackageError::BAD_FILE_CRC:
      return "bad file crc";
  }
  return "unknown";
}

}  // namespace dictionary

#pragma once

#include <cstddef>
#include <cstdint>

#include "DictionaryPackage.h"

namespace dictionary::contextual {

constexpr uint16_t kCanonicalFormatVersion = 1;
constexpr size_t kCanonicalMetaSize = 112;
constexpr uint16_t kCanonicalLexemeRecordSize = 16;
constexpr uint16_t kCanonicalPosVersion = 1;
constexpr uint32_t kMaxCanonicalLexemes = 500000;
constexpr uint32_t kMaxCanonicalHeadwordsSize = 64U * 1024U * 1024U;
constexpr uint16_t kMaxCanonicalHeadwordBytes = 96;

constexpr uint16_t kDefinitionFormatVersion = 1;
constexpr size_t kDefinitionMetaSize = 144;
constexpr uint16_t kDefinitionIndexRecordSize = 8;
constexpr uint16_t kDefinitionEntryVersion = 1;
constexpr uint32_t kMaxDefinitionIndexSize = 4000000;
constexpr uint32_t kMaxDefinitionEntriesSize = 1024U * 1024U * 1024U;
constexpr uint32_t kMaxDefinitionEntryBytes = 1024U * 1024U;

struct CanonicalMetadata {
  uint8_t canonicalUuid[16]{};
  char sourceLanguage[8]{};
  uint32_t lexemeCount = 0;
  uint32_t lexemesFileSize = 0;
  uint32_t headwordsFileSize = 0;
  uint32_t lexemesCrc32 = 0;
  uint32_t headwordsCrc32 = 0;
  uint8_t payloadSha256[32]{};
};

struct CanonicalLexemeRecord {
  uint32_t headwordOffset = 0;
  uint64_t keyHash = 0;
  uint16_t headwordLength = 0;
  uint8_t partOfSpeech = 0;
  uint8_t flags = 0;
};

struct DefinitionMetadata {
  uint8_t sourceUuid[16]{};
  uint8_t canonicalUuid[16]{};
  char sourceLanguage[8]{};
  char targetLanguage[8]{};
  char sourceLabel[32]{};
  uint32_t canonicalLexemeCount = 0;
  uint32_t indexFileSize = 0;
  uint32_t entriesFileSize = 0;
  uint32_t indexCrc32 = 0;
  uint32_t entriesCrc32 = 0;
  uint32_t coverageCount = 0;
};

struct DefinitionIndexRecord {
  uint32_t entryOffset = 0;
  uint32_t entryLength = 0;

  bool present() const { return entryLength != 0; }
};

enum class RuntimeFormatError : uint8_t {
  NONE = 0,
  SOURCE_UNAVAILABLE,
  META_TRUNCATED,
  BAD_MAGIC,
  UNSUPPORTED_VERSION,
  BAD_META_SIZE,
  BAD_META_CRC,
  UNSUPPORTED_FLAGS,
  RESERVED_FIELD_NONZERO,
  INVALID_UUID,
  INVALID_LANGUAGE,
  INVALID_LABEL,
  COUNT_OUT_OF_RANGE,
  RECORD_SIZE_INVALID,
  FILE_SIZE_OUT_OF_RANGE,
  FILE_SIZE_MISMATCH,
  PAYLOAD_READ_FAILED,
  BAD_FILE_CRC,
  CANONICAL_MISMATCH,
  RECORD_ID_OUT_OF_RANGE,
  RECORD_READ_FAILED,
  RECORD_INVALID,
  OUTPUT_BUFFER_TOO_SMALL,
  HEADWORD_READ_FAILED,
  INDEX_ORDER_INVALID,
  COVERAGE_MISMATCH,
};

// Reads exactly one fixed 8-byte index record from already validated source
// metadata. This supports retained descriptors without reopening meta.bin.
bool readDefinitionIndexRecord(const RandomAccessSource& index, uint32_t canonicalLexemeCount, uint32_t entriesFileSize,
                               uint32_t canonicalId, DefinitionIndexRecord& out, RuntimeFormatError& error);

class CanonicalLexiconReader {
 public:
  bool open(const RandomAccessSource& meta, const RandomAccessSource& lexemes, const RandomAccessSource& headwords,
            RuntimeFormatError& error);
  bool validatePayloadCrc(uint8_t* scratch, size_t scratchSize, RuntimeFormatError& error) const;
  bool validateLexemes(uint8_t* scratch, size_t scratchSize, RuntimeFormatError& error) const;
  bool readLexeme(uint32_t canonicalId, CanonicalLexemeRecord& out, RuntimeFormatError& error) const;
  bool readHeadword(const CanonicalLexemeRecord& lexeme, char* output, size_t capacity, size_t& outputLength,
                    RuntimeFormatError& error) const;

  const CanonicalMetadata& metadata() const { return metadata_; }
  bool isOpen() const { return open_; }

 private:
  CanonicalMetadata metadata_{};
  RandomAccessSource lexemes_{};
  RandomAccessSource headwords_{};
  bool open_ = false;
};

class DefinitionSourceReader {
 public:
  bool open(const RandomAccessSource& meta, const RandomAccessSource& index, const RandomAccessSource& entries,
            const uint8_t expectedCanonicalUuid[16], uint32_t expectedCanonicalCount, RuntimeFormatError& error);
  bool validatePayloadCrc(uint8_t* scratch, size_t scratchSize, RuntimeFormatError& error) const;
  bool readIndex(uint32_t canonicalId, DefinitionIndexRecord& out, RuntimeFormatError& error) const;
  bool validateIndex(uint8_t* scratch, size_t scratchSize, RuntimeFormatError& error) const;

  const DefinitionMetadata& metadata() const { return metadata_; }
  bool isOpen() const { return open_; }

 private:
  DefinitionMetadata metadata_{};
  RandomAccessSource index_{};
  RandomAccessSource entries_{};
  bool open_ = false;
};

const char* runtimeFormatErrorName(RuntimeFormatError error);

}  // namespace dictionary::contextual

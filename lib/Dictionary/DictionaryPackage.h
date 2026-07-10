#pragma once

#include <cstddef>
#include <cstdint>

namespace dictionary {

constexpr uint16_t kDictionaryPackageVersion = 1;
constexpr size_t kDictionaryMetaSize = 80;
constexpr uint16_t kLexemeRecordSize = 24;
constexpr uint32_t kMaxLexemeCount = 500000;
constexpr uint32_t kMaxHeadwordsFileSize = 64U * 1024U * 1024U;
constexpr uint32_t kMaxEntriesFileSize = 1024U * 1024U * 1024U;
constexpr uint16_t kMaxHeadwordBytes = 96;
constexpr uint32_t kMaxEntryBytes = 1024U * 1024U;
constexpr uint16_t kMaxEntryFieldCount = 1024;

struct RandomAccessSource {
  void* context = nullptr;
  uint64_t size = 0;
  bool (*readAt)(void* context, uint32_t offset, void* output, size_t length) = nullptr;
};

struct PackageMetadata {
  uint8_t dictionaryBundleUuid[16]{};
  char sourceLanguage[8]{};
  char targetLanguage[8]{};
  uint32_t lexemeCount = 0;
  uint32_t lexemesFileSize = 0;
  uint32_t headwordsFileSize = 0;
  uint32_t entriesFileSize = 0;
  uint32_t lexemesCrc32 = 0;
  uint32_t headwordsCrc32 = 0;
  uint32_t entriesCrc32 = 0;
};

struct LexemeRecord {
  uint32_t headwordOffset = 0;
  uint32_t entryOffset = 0;
  uint32_t entryLength = 0;
  uint64_t lexemeKeyHash = 0;
  uint16_t headwordLength = 0;
  uint8_t partOfSpeech = 0;
  uint8_t flags = 0;
};

struct EntrySlice {
  uint32_t offset = 0;
  uint32_t length = 0;
};

struct EntryHeader {
  uint8_t version = 0;
  uint8_t flags = 0;
  uint16_t fieldCount = 0;
};

struct EntryFieldHeader {
  uint8_t type = 0;
  uint8_t flags = 0;
  uint16_t length = 0;
  uint32_t payloadOffset = 0;  // Relative to the start of EntrySlice.
};

enum class PackageError : uint8_t {
  NONE = 0,
  SOURCE_UNAVAILABLE,
  META_TRUNCATED,
  BAD_MAGIC,
  UNSUPPORTED_VERSION,
  BAD_META_SIZE,
  BAD_META_CRC,
  UNSUPPORTED_FLAGS,
  RESERVED_FIELD_NONZERO,
  INVALID_DICTIONARY_UUID,
  INVALID_LANGUAGE,
  COUNT_OUT_OF_RANGE,
  FILE_SIZE_OUT_OF_RANGE,
  FILE_SIZE_MISMATCH,
  LEXEME_ID_OUT_OF_RANGE,
  LEXEME_READ_FAILED,
  LEXEME_RECORD_INVALID,
  OUTPUT_BUFFER_TOO_SMALL,
  HEADWORD_READ_FAILED,
  ENTRY_READ_FAILED,
  ENTRY_RANGE_INVALID,
  ENTRY_HEADER_INVALID,
  ENTRY_FIELD_INVALID,
  BAD_FILE_CRC,
};

// Allocation-free reader for a native runtime dictionary package. Sources are
// non-owning callbacks instead of retained HalFile objects, allowing the HAL
// adapter to open only the one SD file needed by each operation.
class DictionaryPackage {
 public:
  bool open(const RandomAccessSource& meta, const RandomAccessSource& lexemes, const RandomAccessSource& headwords,
            const RandomAccessSource& entries, PackageError& error);

  bool readLexeme(uint32_t lexemeId, LexemeRecord& out, PackageError& error) const;
  bool readHeadword(const LexemeRecord& lexeme, char* output, size_t capacity, size_t& outputLength,
                    PackageError& error) const;
  bool getEntrySlice(const LexemeRecord& lexeme, EntrySlice& out, PackageError& error) const;
  bool readEntryHeader(const EntrySlice& entry, EntryHeader& out, PackageError& error) const;
  bool readEntryFieldHeader(const EntrySlice& entry, uint32_t relativeOffset, EntryFieldHeader& out,
                            PackageError& error) const;
  bool readEntryChunk(const EntrySlice& entry, uint32_t relativeOffset, void* output, size_t capacity,
                      size_t& bytesRead, PackageError& error) const;

  const PackageMetadata& metadata() const { return metadata_; }
  bool isOpen() const { return open_; }

 private:
  PackageMetadata metadata_{};
  RandomAccessSource lexemes_{};
  RandomAccessSource headwords_{};
  RandomAccessSource entries_{};
  bool open_ = false;
};

// Full-file validation for installation and tests. Scratch storage is owned by
// the caller so firmware can reuse one bounded buffer instead of allocating.
bool validateSourceCrc(const RandomAccessSource& source, uint32_t expectedCrc, uint8_t* scratch, size_t scratchSize,
                       PackageError& error);

const char* packageErrorName(PackageError error);

}  // namespace dictionary

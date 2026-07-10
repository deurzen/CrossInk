#pragma once

#include <cstddef>
#include <cstdint>

namespace dictionary::lexeme_state {

constexpr uint32_t kMaxLexemeCount = 500000;
constexpr size_t kMetaSize = 36;
constexpr size_t kStatusHeaderSize = 32;
constexpr size_t kWalSize = 40;
constexpr size_t kMaxStatePath = 144;
constexpr uint32_t kMaxReviewScanLexemes = 4096;

enum class Status : uint8_t {
  Unseen = 0,
  Known = 1,
  Learning = 2,
  Ignored = 3,
  ImplicitlyFamiliar = 4,
};

bool isSuppressed(Status status);

using StatusVisitor = bool (*)(void* context, uint32_t lexemeId, Status status);

struct StorageBackend {
  void* context = nullptr;
  bool (*ensureDirectory)(void* context, const char* path) = nullptr;
  bool (*exists)(void* context, const char* path) = nullptr;
  bool (*remove)(void* context, const char* path) = nullptr;
  bool (*rename)(void* context, const char* oldPath, const char* newPath) = nullptr;
  bool (*readAt)(void* context, const char* path, uint32_t offset, void* output, size_t length) = nullptr;
  bool (*writeAtSynced)(void* context, const char* path, uint32_t offset, const void* data, size_t length) = nullptr;
  bool (*writeFileSynced)(void* context, const char* path, const void* data, size_t length) = nullptr;
  bool (*createFileSynced)(void* context, const char* path, const void* prefix, size_t prefixLength,
                           uint32_t totalSize) = nullptr;
  uint64_t (*fileSize)(void* context, const char* path) = nullptr;
};

enum class StateError : uint8_t {
  NONE = 0,
  INVALID_INPUT,
  STORAGE_UNAVAILABLE,
  DIRECTORY_FAILED,
  METADATA_INVALID,
  STATUS_INVALID,
  WAL_INVALID,
  IO_FAILED,
  LEXEME_ID_OUT_OF_RANGE,
  GENERATION_OVERFLOW,
};

// Global per-bundle status store. Updates are replayable after power loss:
// synced WAL -> synced status nibble -> synced generation -> WAL removal.
class Store {
 public:
  bool open(const StorageBackend& storage, const char* rootPath, const uint8_t (&bundleUuid)[16], uint32_t lexemeCount,
            StateError& error);
  bool get(uint32_t lexemeId, Status& status, StateError& error);
  bool set(uint32_t lexemeId, Status status, StateError& error);
  bool readPackedByte(uint32_t byteIndex, uint8_t& value, StateError& error);
  // Reads one bounded contiguous status window. Returning false from visitor
  // stops successfully after that item; nextLexemeId resumes without repeats.
  bool visitNonUnseen(uint32_t firstLexemeId, uint32_t scanLexemeCount, uint8_t* scratch, size_t scratchCapacity,
                      void* context, StatusVisitor visitor, uint32_t& nextLexemeId, StateError& error);

  uint32_t generation() const { return generation_; }
  uint32_t lexemeCount() const { return lexemeCount_; }
  uint32_t packedByteCount() const { return (lexemeCount_ + 1U) / 2U; }
  const char* directoryPath() const { return directoryPath_; }

 private:
  StorageBackend storage_{};
  uint8_t bundleUuid_[16]{};
  char directoryPath_[kMaxStatePath]{};
  char metaPath_[kMaxStatePath]{};
  char statusPath_[kMaxStatePath]{};
  char walPath_[kMaxStatePath]{};
  uint32_t lexemeCount_ = 0;
  uint32_t generation_ = 0;
  // Suppression rebuilds read monotonically increasing packed indexes. This
  // bounded cache turns up to 256 one-byte file opens into one sequential read.
  uint8_t packedCache_[256]{};
  uint32_t packedCacheFirst_ = 0;
  uint16_t packedCacheCount_ = 0;
  bool open_ = false;

  bool recover(StateError& error);
  bool writeStatusNibble(uint32_t lexemeId, Status status, StateError& error);
};

const char* stateErrorName(StateError error);

}  // namespace dictionary::lexeme_state

#pragma once

#include <cstddef>
#include <cstdint>

namespace dictionary {

constexpr uint32_t kMaxEntryBytes = 1024U * 1024U;
constexpr uint16_t kMaxEntryFieldCount = 1024;

struct RandomAccessSource {
  void* context = nullptr;
  uint64_t size = 0;
  bool (*readAt)(void* context, uint32_t offset, void* output, size_t length) = nullptr;
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
  uint32_t payloadOffset = 0;
};

enum class RuntimeError : uint8_t {
  NONE = 0,
  SOURCE_UNAVAILABLE,
  OUTPUT_BUFFER_TOO_SMALL,
  ENTRY_READ_FAILED,
  ENTRY_RANGE_INVALID,
  BAD_FILE_CRC,
};

// Streams a full-file CRC through caller-owned scratch storage so installation
// never materializes a runtime file or allocates in the validation loop.
bool validateSourceCrc(const RandomAccessSource& source, uint32_t expectedCrc, uint8_t* scratch, size_t scratchSize,
                       RuntimeError& error);

const char* runtimeErrorName(RuntimeError error);

}  // namespace dictionary

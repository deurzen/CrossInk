#include "DictionaryRuntime.h"

#include <algorithm>

#include "Crc32.h"

namespace dictionary {

bool validateSourceCrc(const RandomAccessSource& source, const uint32_t expectedCrc, uint8_t* scratch,
                       const size_t scratchSize, RuntimeError& error) {
  error = RuntimeError::NONE;
  if (source.readAt == nullptr || source.size > UINT32_MAX) {
    error = RuntimeError::SOURCE_UNAVAILABLE;
    return false;
  }
  if (scratch == nullptr || scratchSize == 0) {
    error = RuntimeError::OUTPUT_BUFFER_TOO_SMALL;
    return false;
  }

  uint32_t crc = 0;
  uint64_t offset = 0;
  while (offset < source.size) {
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(scratchSize, source.size - offset));
    if (!source.readAt(source.context, static_cast<uint32_t>(offset), scratch, chunk)) {
      error = RuntimeError::SOURCE_UNAVAILABLE;
      return false;
    }
    crc = updateCrc32(crc, scratch, chunk);
    offset += chunk;
  }
  if (crc != expectedCrc) {
    error = RuntimeError::BAD_FILE_CRC;
    return false;
  }
  return true;
}

const char* runtimeErrorName(const RuntimeError error) {
  switch (error) {
    case RuntimeError::NONE:
      return "none";
    case RuntimeError::SOURCE_UNAVAILABLE:
      return "source unavailable";
    case RuntimeError::OUTPUT_BUFFER_TOO_SMALL:
      return "output buffer too small";
    case RuntimeError::ENTRY_READ_FAILED:
      return "entry read failed";
    case RuntimeError::ENTRY_RANGE_INVALID:
      return "entry range invalid";
    case RuntimeError::BAD_FILE_CRC:
      return "bad file crc";
  }
  return "unknown";
}

}  // namespace dictionary

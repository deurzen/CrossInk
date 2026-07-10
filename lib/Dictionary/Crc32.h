#pragma once

#include <cstddef>
#include <cstdint>

namespace dictionary {

// Standard CRC32 compatible with zlib.crc32(). Pass the previous return value
// to continue across chunks; use zero for the first chunk.
uint32_t updateCrc32(uint32_t crc, const uint8_t* data, size_t length);

}  // namespace dictionary

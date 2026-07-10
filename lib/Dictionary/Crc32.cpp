#include "Crc32.h"

#include <array>
#include <cstdint>

namespace dictionary {
namespace {

constexpr std::array<uint32_t, 256> makeCrcTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t index = 0; index < table.size(); ++index) {
    uint32_t value = index;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (value & 1U);
      value = (value >> 1U) ^ (0xEDB88320U & mask);
    }
    table[index] = value;
  }
  return table;
}

// One kilobyte in flash avoids eight bit-at-a-time rounds for every byte while
// validating dictionary files. No CRC table is copied to DRAM.
static constexpr auto kCrcTable = makeCrcTable();

}  // namespace

uint32_t updateCrc32(uint32_t crc, const uint8_t* data, const size_t length) {
  if (data == nullptr && length != 0) return crc;

  crc ^= UINT32_MAX;
  for (size_t i = 0; i < length; ++i) {
    crc = kCrcTable[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
  }
  return crc ^ UINT32_MAX;
}

}  // namespace dictionary

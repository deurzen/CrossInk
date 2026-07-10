#pragma once

#include <cstddef>
#include <cstdint>

namespace DeviceSync {

class Crc32 {
 public:
  void update(const void* data, size_t length);
  uint32_t value() const { return ~value_; }

 private:
  uint32_t value_ = UINT32_MAX;
};

}  // namespace DeviceSync

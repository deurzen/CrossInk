#pragma once

#include <cstddef>

namespace DeviceSync {

struct CodecInput {
  void* context = nullptr;
  bool (*readExact)(void* context, void* data, size_t length) = nullptr;
  size_t size = 0;
};

struct CodecOutput {
  void* context = nullptr;
  bool (*writeExact)(void* context, const void* data, size_t length) = nullptr;
};

}  // namespace DeviceSync

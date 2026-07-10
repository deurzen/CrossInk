#pragma once

#include <cstddef>
#include <cstdint>

#include "SyncPolicy.h"

namespace DeviceSync::SyncPolicyCodec {

constexpr uint16_t FORMAT_VERSION = 1;
constexpr size_t MIN_ENCODED_SIZE = 32;
constexpr size_t MAX_ENCODED_SIZE = 1004;

struct Input {
  void* context = nullptr;
  bool (*readExact)(void* context, void* data, size_t length) = nullptr;
  size_t size = 0;
};

struct Output {
  void* context = nullptr;
  bool (*writeExact)(void* context, const void* data, size_t length) = nullptr;
};

enum class DecodeResult : uint8_t {
  Ok,
  Invalid,
  Unsupported,
};

size_t encodedSize(const SyncPolicy& policy);
bool encode(const SyncPolicy& policy, const Output& output);
DecodeResult decode(const Input& input, SyncPolicy& policy);

}  // namespace DeviceSync::SyncPolicyCodec

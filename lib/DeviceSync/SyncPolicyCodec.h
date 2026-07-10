#pragma once

#include <cstddef>
#include <cstdint>

#include "CodecIO.h"
#include "SyncPolicy.h"

namespace DeviceSync::SyncPolicyCodec {

constexpr uint16_t FORMAT_VERSION = 1;
constexpr size_t MIN_ENCODED_SIZE = 32;
constexpr size_t MAX_ENCODED_SIZE = 1004;

using Input = CodecInput;
using Output = CodecOutput;

enum class DecodeResult : uint8_t {
  Ok,
  Invalid,
  Unsupported,
};

size_t encodedSize(const SyncPolicy& policy);
bool encode(const SyncPolicy& policy, const Output& output);
DecodeResult validate(const Input& input);
DecodeResult decode(const Input& input, SyncPolicy& policy);

}  // namespace DeviceSync::SyncPolicyCodec

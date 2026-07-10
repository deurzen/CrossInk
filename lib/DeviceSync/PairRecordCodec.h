#pragma once

#include <cstddef>
#include <cstdint>

#include "CodecIO.h"
#include "PairRecord.h"

namespace DeviceSync::PairRecordCodec {

constexpr uint16_t FORMAT_VERSION = 1;
constexpr size_t MIN_ENCODED_SIZE = 147;
constexpr size_t MAX_ENCODED_SIZE = MIN_ENCODED_SIZE + PAIR_DISPLAY_NAME_BYTES - 2;

enum class DecodeResult : uint8_t {
  Ok,
  Invalid,
  Unsupported,
};

size_t encodedSize(const PairRecord& record);
bool encode(const PairRecord& record, const CodecOutput& output);
DecodeResult validate(const CodecInput& input);
DecodeResult decode(const CodecInput& input, PairRecord& record);

}  // namespace DeviceSync::PairRecordCodec

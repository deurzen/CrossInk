#include "PairRecordCodec.h"

#include <cstring>

#include "Crc32.h"

namespace DeviceSync::PairRecordCodec {
namespace {
constexpr uint8_t MAGIC[] = {'D', 'S', 'P', 'R'};
constexpr uint8_t FLAG_HAS_LAST_SESSION = 1U << 0;
constexpr uint8_t KNOWN_FLAGS = FLAG_HAS_LAST_SESSION;

class Writer {
 public:
  explicit Writer(const CodecOutput& output) : output_(output) {}

  bool write(const void* data, const size_t length) {
    if (!ok_ || !output_.writeExact(output_.context, data, length)) {
      ok_ = false;
      return false;
    }
    crc_.update(data, length);
    return true;
  }

  bool writeU8(const uint8_t value) { return write(&value, sizeof(value)); }
  bool writeU16(const uint16_t value) {
    const uint8_t data[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
    return write(data, sizeof(data));
  }
  bool writeU32(const uint32_t value) {
    const uint8_t data[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                            static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
    return write(data, sizeof(data));
  }
  bool writeU64(const uint64_t value) {
    uint8_t data[8];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = static_cast<uint8_t>(value >> (i * 8));
    return write(data, sizeof(data));
  }
  bool writeCrc() {
    const uint32_t value = crc_.value();
    const uint8_t data[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                            static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
    return ok_ && output_.writeExact(output_.context, data, sizeof(data));
  }

 private:
  const CodecOutput& output_;
  Crc32 crc_;
  bool ok_ = true;
};

class Reader {
 public:
  explicit Reader(const CodecInput& input) : input_(input) {}

  bool read(void* data, const size_t length) {
    if (length > input_.size - consumed_ || !input_.readExact(input_.context, data, length)) return false;
    consumed_ += length;
    crc_.update(data, length);
    return true;
  }

  bool readU8(uint8_t& value) { return read(&value, sizeof(value)); }
  bool readU16(uint16_t& value) {
    uint8_t data[2];
    if (!read(data, sizeof(data))) return false;
    value = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
    return true;
  }
  bool readU32(uint32_t& value) {
    uint8_t data[4];
    if (!read(data, sizeof(data))) return false;
    value = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
            (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
    return true;
  }
  bool readU64(uint64_t& value) {
    uint8_t data[8];
    if (!read(data, sizeof(data))) return false;
    value = 0;
    for (size_t i = 0; i < sizeof(data); ++i) value |= static_cast<uint64_t>(data[i]) << (i * 8);
    return true;
  }
  bool checkCrc() {
    uint8_t data[4];
    if (sizeof(data) > input_.size - consumed_ || !input_.readExact(input_.context, data, sizeof(data))) return false;
    consumed_ += sizeof(data);
    const uint32_t expected = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                              (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
    return expected == crc_.value();
  }

  size_t consumed() const { return consumed_; }

 private:
  const CodecInput& input_;
  Crc32 crc_;
  size_t consumed_ = 0;
};

DecodeResult decodeImpl(const CodecInput& input, PairRecord* record) {
  if (record != nullptr) record->reset();
  if (input.readExact == nullptr || input.size < sizeof(MAGIC) + sizeof(uint16_t)) return DecodeResult::Invalid;
  const auto invalid = [record]() {
    if (record != nullptr) record->reset();
    return DecodeResult::Invalid;
  };

  Reader reader(input);
  uint8_t magic[sizeof(MAGIC)];
  uint16_t version = 0;
  if (!reader.read(magic, sizeof(magic)) || std::memcmp(magic, MAGIC, sizeof(MAGIC)) != 0 || !reader.readU16(version)) {
    return invalid();
  }
  if (version > FORMAT_VERSION) return DecodeResult::Unsupported;
  if (version != FORMAT_VERSION || input.size < MIN_ENCODED_SIZE || input.size > MAX_ENCODED_SIZE) return invalid();

  uint32_t declaredLength = 0;
  uint8_t nameLength = 0;
  uint8_t flags = 0;
  uint16_t reserved = 0;
  PairRecord scratch;
  PairRecord& output = record != nullptr ? *record : scratch;
  if (!reader.readU32(declaredLength) || declaredLength != input.size || !reader.readU8(nameLength) ||
      nameLength == 0 || nameLength >= PAIR_DISPLAY_NAME_BYTES || !reader.readU8(flags) ||
      (flags & ~KNOWN_FLAGS) != 0 || !reader.readU16(reserved) || reserved != 0 ||
      !reader.read(output.peerDeviceId.data(), output.peerDeviceId.size()) ||
      !reader.read(output.pairSecret.data(), output.pairSecret.size()) ||
      !reader.readU64(output.firstPairedLocalGeneration) ||
      !reader.read(output.lastSuccessfulSessionId.data(), output.lastSuccessfulSessionId.size()) ||
      !reader.readU64(output.capabilitySnapshot) ||
      !reader.read(output.lastObservedPolicyDigest.data(), output.lastObservedPolicyDigest.size()) ||
      !reader.readU64(output.nextLocalHandshakeCounter) || !reader.readU64(output.lastAcceptedPeerHandshakeCounter) ||
      !reader.read(output.peerDisplayName, nameLength)) {
    return invalid();
  }
  output.peerDisplayName[nameLength] = '\0';
  output.hasLastSuccessfulSession = (flags & FLAG_HAS_LAST_SESSION) != 0;

  if (!output.valid() || reader.consumed() + sizeof(uint32_t) != input.size || !reader.checkCrc() ||
      reader.consumed() != input.size) {
    return invalid();
  }
  return DecodeResult::Ok;
}

}  // namespace

size_t encodedSize(const PairRecord& record) {
  if (!record.valid()) return 0;
  const size_t nameLength = strnlen(record.peerDisplayName, PAIR_DISPLAY_NAME_BYTES);
  return nameLength < PAIR_DISPLAY_NAME_BYTES ? MIN_ENCODED_SIZE + nameLength - 1 : 0;
}

bool encode(const PairRecord& record, const CodecOutput& output) {
  if (output.writeExact == nullptr) return false;
  const size_t size = encodedSize(record);
  if (size == 0 || size > UINT32_MAX) return false;
  const size_t nameLength = std::strlen(record.peerDisplayName);

  Writer writer(output);
  if (!writer.write(MAGIC, sizeof(MAGIC)) || !writer.writeU16(FORMAT_VERSION) ||
      !writer.writeU32(static_cast<uint32_t>(size)) || !writer.writeU8(static_cast<uint8_t>(nameLength)) ||
      !writer.writeU8(record.hasLastSuccessfulSession ? FLAG_HAS_LAST_SESSION : 0) || !writer.writeU16(0) ||
      !writer.write(record.peerDeviceId.data(), record.peerDeviceId.size()) ||
      !writer.write(record.pairSecret.data(), record.pairSecret.size()) ||
      !writer.writeU64(record.firstPairedLocalGeneration) ||
      !writer.write(record.lastSuccessfulSessionId.data(), record.lastSuccessfulSessionId.size()) ||
      !writer.writeU64(record.capabilitySnapshot) ||
      !writer.write(record.lastObservedPolicyDigest.data(), record.lastObservedPolicyDigest.size()) ||
      !writer.writeU64(record.nextLocalHandshakeCounter) || !writer.writeU64(record.lastAcceptedPeerHandshakeCounter) ||
      !writer.write(record.peerDisplayName, nameLength)) {
    return false;
  }
  return writer.writeCrc();
}

DecodeResult validate(const CodecInput& input) { return decodeImpl(input, nullptr); }

DecodeResult decode(const CodecInput& input, PairRecord& record) { return decodeImpl(input, &record); }

}  // namespace DeviceSync::PairRecordCodec

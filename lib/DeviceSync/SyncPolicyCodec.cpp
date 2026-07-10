#include "SyncPolicyCodec.h"

#include <cstring>

namespace DeviceSync::SyncPolicyCodec {
namespace {
constexpr uint8_t MAGIC[] = {'D', 'S', 'P', 'C'};
constexpr uint16_t FLAG_MIRROR_DELETIONS = 1U << 0;
constexpr uint16_t KNOWN_FLAGS = FLAG_MIRROR_DELETIONS;

class Crc32 {
 public:
  void update(const void* data, const size_t length) {
    static constexpr uint32_t TABLE[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
      value_ = (value_ >> 4) ^ TABLE[(value_ ^ bytes[i]) & 0x0F];
      value_ = (value_ >> 4) ^ TABLE[(value_ ^ (bytes[i] >> 4)) & 0x0F];
    }
  }

  uint32_t value() const { return ~value_; }

 private:
  uint32_t value_ = UINT32_MAX;
};

class Writer {
 public:
  explicit Writer(const Output& output) : output_(output) {}

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
  bool writeCrc() {
    const uint32_t value = crc_.value();
    const uint8_t data[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                            static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
    return ok_ && output_.writeExact(output_.context, data, sizeof(data));
  }

 private:
  const Output& output_;
  Crc32 crc_;
  bool ok_ = true;
};

class Reader {
 public:
  explicit Reader(const Input& input) : input_(input) {}

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
  bool readAndCheckCrc() {
    uint8_t data[4];
    if (sizeof(data) > input_.size - consumed_ || !input_.readExact(input_.context, data, sizeof(data))) return false;
    consumed_ += sizeof(data);
    const uint32_t expected = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                              (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
    return expected == crc_.value();
  }

  size_t consumed() const { return consumed_; }

 private:
  const Input& input_;
  Crc32 crc_;
  size_t consumed_ = 0;
};

}  // namespace

size_t encodedSize(const SyncPolicy& policy) {
  size_t size = MIN_ENCODED_SIZE;
  for (size_t i = 0; i < policy.pathRuleCount(); ++i) {
    const PathRule* rule = policy.pathRule(i);
    if (rule == nullptr) return 0;
    const size_t patternLength = strnlen(rule->pattern, MAX_POLICY_PATTERN_BYTES);
    if (patternLength == 0 || patternLength >= MAX_POLICY_PATTERN_BYTES) return 0;
    size += sizeof(uint8_t) * 2 + patternLength;
  }
  return size <= MAX_ENCODED_SIZE ? size : 0;
}

bool encode(const SyncPolicy& policy, const Output& output) {
  if (output.writeExact == nullptr || policy.pathRuleCount() > UINT8_MAX) return false;
  const size_t size = encodedSize(policy);
  if (size == 0 || size > UINT32_MAX) return false;

  Writer writer(output);
  if (!writer.write(MAGIC, sizeof(MAGIC)) || !writer.writeU16(FORMAT_VERSION) ||
      !writer.writeU32(static_cast<uint32_t>(size)) || !writer.writeU8(static_cast<uint8_t>(CATEGORY_COUNT)) ||
      !writer.writeU8(static_cast<uint8_t>(policy.pathRuleCount())) ||
      !writer.writeU16(policy.mirrorDeletions() ? FLAG_MIRROR_DELETIONS : 0)) {
    return false;
  }

  for (size_t i = 0; i < CATEGORY_COUNT; ++i) {
    if (!writer.writeU8(static_cast<uint8_t>(policy.direction(static_cast<Category>(i))))) return false;
  }
  for (size_t i = 0; i < policy.pathRuleCount(); ++i) {
    const PathRule* rule = policy.pathRule(i);
    const size_t patternLength = std::strlen(rule->pattern);
    if (!writer.writeU8(static_cast<uint8_t>(rule->action)) || !writer.writeU8(static_cast<uint8_t>(patternLength)) ||
        !writer.write(rule->pattern, patternLength)) {
      return false;
    }
  }
  return writer.writeCrc();
}

DecodeResult decode(const Input& input, SyncPolicy& policy) {
  policy.reset();
  if (input.readExact == nullptr || input.size < MIN_ENCODED_SIZE || input.size > MAX_ENCODED_SIZE) {
    return DecodeResult::Invalid;
  }
  const auto invalid = [&policy]() {
    policy.reset();
    return DecodeResult::Invalid;
  };

  Reader reader(input);
  uint8_t magic[sizeof(MAGIC)];
  uint16_t version = 0;
  if (!reader.read(magic, sizeof(magic)) || std::memcmp(magic, MAGIC, sizeof(MAGIC)) != 0 || !reader.readU16(version)) {
    return invalid();
  }
  if (version > FORMAT_VERSION) return DecodeResult::Unsupported;
  if (version != FORMAT_VERSION) return invalid();

  uint32_t declaredLength = 0;
  uint8_t categoryCount = 0;
  uint8_t ruleCount = 0;
  uint16_t flags = 0;
  if (!reader.readU32(declaredLength) || declaredLength != input.size || !reader.readU8(categoryCount) ||
      !reader.readU8(ruleCount) || !reader.readU16(flags) || categoryCount != CATEGORY_COUNT ||
      ruleCount > MAX_POLICY_RULES || (flags & ~KNOWN_FLAGS) != 0) {
    return invalid();
  }

  policy.setMirrorDeletions((flags & FLAG_MIRROR_DELETIONS) != 0);
  for (size_t i = 0; i < CATEGORY_COUNT; ++i) {
    uint8_t rawDirection = 0;
    if (!reader.readU8(rawDirection) || rawDirection > static_cast<uint8_t>(Direction::Bidirectional) ||
        !policy.setDirection(static_cast<Category>(i), static_cast<Direction>(rawDirection))) {
      return invalid();
    }
  }

  for (uint8_t i = 0; i < ruleCount; ++i) {
    uint8_t rawAction = 0;
    uint8_t patternLength = 0;
    char pattern[MAX_POLICY_PATTERN_BYTES];
    if (!reader.readU8(rawAction) || rawAction > static_cast<uint8_t>(PathRuleAction::Include) ||
        !reader.readU8(patternLength) || patternLength == 0 || patternLength >= sizeof(pattern) ||
        !reader.read(pattern, patternLength)) {
      return invalid();
    }
    pattern[patternLength] = '\0';
    if (!policy.addPathRule(static_cast<PathRuleAction>(rawAction), pattern)) return invalid();
  }

  if (reader.consumed() + sizeof(uint32_t) != input.size || !reader.readAndCheckCrc() ||
      reader.consumed() != input.size) {
    return invalid();
  }
  return DecodeResult::Ok;
}

}  // namespace DeviceSync::SyncPolicyCodec

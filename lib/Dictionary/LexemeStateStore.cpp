#include "LexemeStateStore.h"

#include <cstdio>
#include <cstring>

#include "Crc32.h"

namespace dictionary::lexeme_state {
namespace {
constexpr char kMetaMagic[] = "CXSM";
constexpr char kStatusMagic[] = "CXST";
constexpr char kWalMagic[] = "CXWL";
constexpr uint16_t kVersion = 1;
constexpr uint32_t kStatusGenerationOffset = 28;
constexpr uint32_t kStatusPayloadOffset = kStatusHeaderSize;

void writeU16(uint8_t* data, const uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
}
void writeU32(uint8_t* data, const uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
  data[2] = static_cast<uint8_t>(value >> 16U);
  data[3] = static_cast<uint8_t>(value >> 24U);
}
uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8U);
}
uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

bool backendValid(const StorageBackend& storage) {
  return storage.ensureDirectory && storage.exists && storage.remove && storage.rename && storage.readAt &&
         storage.writeAtSynced && storage.writeFileSynced && storage.createFileSynced && storage.fileSize;
}

bool appendPath(char* output, const size_t capacity, const char* directory, const char* leaf) {
  const int written = std::snprintf(output, capacity, "%s/%s", directory, leaf);
  return written > 0 && static_cast<size_t>(written) < capacity;
}

void encodeMeta(uint8_t (&data)[kMetaSize], const uint8_t (&uuid)[16], const uint32_t count) {
  std::memset(data, 0, sizeof(data));
  std::memcpy(data, kMetaMagic, 4);
  writeU16(data + 4, kVersion);
  writeU16(data + 6, kMetaSize);
  std::memcpy(data + 8, uuid, 16);
  writeU32(data + 24, count);
  writeU32(data + 32, updateCrc32(0, data, 32));
}

void encodeStatusHeader(uint8_t (&data)[kStatusHeaderSize], const uint8_t (&uuid)[16], const uint32_t count,
                        const uint32_t generation) {
  std::memset(data, 0, sizeof(data));
  std::memcpy(data, kStatusMagic, 4);
  writeU16(data + 4, kVersion);
  writeU16(data + 6, kStatusHeaderSize);
  std::memcpy(data + 8, uuid, 16);
  writeU32(data + 24, count);
  writeU32(data + 28, generation);
}

void encodeWal(uint8_t (&data)[kWalSize], const uint8_t (&uuid)[16], const uint32_t lexemeId,
               const uint32_t targetGeneration, const Status status) {
  std::memset(data, 0, sizeof(data));
  std::memcpy(data, kWalMagic, 4);
  writeU16(data + 4, kVersion);
  writeU16(data + 6, kWalSize);
  std::memcpy(data + 8, uuid, 16);
  writeU32(data + 24, lexemeId);
  writeU32(data + 28, targetGeneration);
  data[32] = static_cast<uint8_t>(status);
  writeU32(data + 36, updateCrc32(0, data, 36));
}

}  // namespace

bool isSuppressed(const Status status) { return status != Status::Unseen; }

bool Store::open(const StorageBackend& storage, const char* rootPath, const uint8_t (&bundleUuid)[16],
                 const uint32_t lexemeCount, StateError& error) {
  open_ = false;
  error = StateError::NONE;
  if (!backendValid(storage) || !rootPath || rootPath[0] == '\0' || lexemeCount == 0 || lexemeCount > kMaxLexemeCount) {
    error = StateError::INVALID_INPUT;
    return false;
  }
  storage_ = storage;
  std::memcpy(bundleUuid_, bundleUuid, sizeof(bundleUuid_));
  lexemeCount_ = lexemeCount;

  if (!storage_.ensureDirectory(storage_.context, rootPath)) {
    error = StateError::DIRECTORY_FAILED;
    return false;
  }
  char uuidHex[33]{};
  for (size_t index = 0; index < sizeof(bundleUuid_); ++index) {
    std::snprintf(uuidHex + index * 2, 3, "%02x", bundleUuid_[index]);
  }
  if (!appendPath(directoryPath_, sizeof(directoryPath_), rootPath, uuidHex) ||
      !appendPath(metaPath_, sizeof(metaPath_), directoryPath_, "state.meta") ||
      !appendPath(statusPath_, sizeof(statusPath_), directoryPath_, "status.bin") ||
      !appendPath(walPath_, sizeof(walPath_), directoryPath_, "status.wal") ||
      !storage_.ensureDirectory(storage_.context, directoryPath_)) {
    error = StateError::DIRECTORY_FAILED;
    return false;
  }

  const bool metaExists = storage_.exists(storage_.context, metaPath_);
  const bool statusExists = storage_.exists(storage_.context, statusPath_);
  if (!metaExists) {
    uint8_t meta[kMetaSize]{};
    encodeMeta(meta, bundleUuid, lexemeCount);
    char temporary[kMaxStatePath]{};
    if (!appendPath(temporary, sizeof(temporary), directoryPath_, "state.meta.tmp") ||
        !storage_.writeFileSynced(storage_.context, temporary, meta, sizeof(meta)) ||
        !storage_.rename(storage_.context, temporary, metaPath_)) {
      error = StateError::IO_FAILED;
      return false;
    }
  }
  if (!statusExists) {
    uint8_t header[kStatusHeaderSize]{};
    encodeStatusHeader(header, bundleUuid, lexemeCount, 0);
    char temporary[kMaxStatePath]{};
    if (!appendPath(temporary, sizeof(temporary), directoryPath_, "status.bin.tmp") ||
        !storage_.createFileSynced(storage_.context, temporary, header, sizeof(header),
                                   kStatusHeaderSize + packedByteCount()) ||
        !storage_.rename(storage_.context, temporary, statusPath_)) {
      error = StateError::IO_FAILED;
      return false;
    }
  }

  uint8_t meta[kMetaSize]{};
  if (storage_.fileSize(storage_.context, metaPath_) != sizeof(meta) ||
      !storage_.readAt(storage_.context, metaPath_, 0, meta, sizeof(meta)) || std::memcmp(meta, kMetaMagic, 4) != 0 ||
      readU16(meta + 4) != kVersion || readU16(meta + 6) != kMetaSize ||
      std::memcmp(meta + 8, bundleUuid_, sizeof(bundleUuid_)) != 0 || readU32(meta + 24) != lexemeCount_ ||
      readU32(meta + 28) != 0 || readU32(meta + 32) != updateCrc32(0, meta, 32)) {
    error = StateError::METADATA_INVALID;
    return false;
  }

  uint8_t statusHeader[kStatusHeaderSize]{};
  if (storage_.fileSize(storage_.context, statusPath_) != kStatusHeaderSize + packedByteCount() ||
      !storage_.readAt(storage_.context, statusPath_, 0, statusHeader, sizeof(statusHeader)) ||
      std::memcmp(statusHeader, kStatusMagic, 4) != 0 || readU16(statusHeader + 4) != kVersion ||
      readU16(statusHeader + 6) != kStatusHeaderSize ||
      std::memcmp(statusHeader + 8, bundleUuid_, sizeof(bundleUuid_)) != 0 ||
      readU32(statusHeader + 24) != lexemeCount_) {
    error = StateError::STATUS_INVALID;
    return false;
  }
  generation_ = readU32(statusHeader + 28);
  open_ = true;
  return recover(error);
}

bool Store::recover(StateError& error) {
  error = StateError::NONE;
  if (!open_) {
    error = StateError::STORAGE_UNAVAILABLE;
    return false;
  }
  if (!storage_.exists(storage_.context, walPath_)) return true;

  uint8_t wal[kWalSize]{};
  if (storage_.fileSize(storage_.context, walPath_) != sizeof(wal) ||
      !storage_.readAt(storage_.context, walPath_, 0, wal, sizeof(wal)) || std::memcmp(wal, kWalMagic, 4) != 0 ||
      readU16(wal + 4) != kVersion || readU16(wal + 6) != kWalSize ||
      std::memcmp(wal + 8, bundleUuid_, sizeof(bundleUuid_)) != 0 || readU32(wal + 24) >= lexemeCount_ ||
      wal[32] > static_cast<uint8_t>(Status::ImplicitlyFamiliar) || wal[33] != 0 || wal[34] != 0 || wal[35] != 0 ||
      readU32(wal + 36) != updateCrc32(0, wal, 36)) {
    error = StateError::WAL_INVALID;
    return false;
  }
  const uint32_t targetGeneration = readU32(wal + 28);
  if (targetGeneration < generation_) return storage_.remove(storage_.context, walPath_);
  if (targetGeneration != generation_ && (generation_ == UINT32_MAX || targetGeneration != generation_ + 1U)) {
    error = StateError::WAL_INVALID;
    return false;
  }
  if (!writeStatusNibble(readU32(wal + 24), static_cast<Status>(wal[32]), error)) return false;
  if (targetGeneration != generation_ &&
      !storage_.writeAtSynced(storage_.context, statusPath_, kStatusGenerationOffset, wal + 28, sizeof(uint32_t))) {
    error = StateError::IO_FAILED;
    return false;
  }
  generation_ = targetGeneration;
  storage_.remove(storage_.context, walPath_);  // A stale valid WAL is harmless and reaped on next open.
  return true;
}

bool Store::writeStatusNibble(const uint32_t lexemeId, const Status status, StateError& error) {
  const uint32_t byteIndex = lexemeId / 2U;
  uint8_t packed = 0;
  if (!storage_.readAt(storage_.context, statusPath_, kStatusPayloadOffset + byteIndex, &packed, sizeof(packed))) {
    error = StateError::IO_FAILED;
    return false;
  }
  if ((lexemeId & 1U) == 0) {
    packed = static_cast<uint8_t>((packed & 0xF0U) | static_cast<uint8_t>(status));
  } else {
    packed = static_cast<uint8_t>((packed & 0x0FU) | (static_cast<uint8_t>(status) << 4U));
  }
  if (!storage_.writeAtSynced(storage_.context, statusPath_, kStatusPayloadOffset + byteIndex, &packed,
                              sizeof(packed))) {
    error = StateError::IO_FAILED;
    return false;
  }
  return true;
}

bool Store::get(const uint32_t lexemeId, Status& status, StateError& error) {
  status = Status::Unseen;
  if (!recover(error)) return false;
  if (lexemeId >= lexemeCount_) {
    error = StateError::LEXEME_ID_OUT_OF_RANGE;
    return false;
  }
  uint8_t packed = 0;
  if (!readPackedByte(lexemeId / 2U, packed, error)) return false;
  const uint8_t value = (lexemeId & 1U) == 0 ? packed & 0x0FU : packed >> 4U;
  if (value > static_cast<uint8_t>(Status::ImplicitlyFamiliar)) {
    error = StateError::STATUS_INVALID;
    return false;
  }
  status = static_cast<Status>(value);
  return true;
}

bool Store::set(const uint32_t lexemeId, const Status status, StateError& error) {
  if (!recover(error)) return false;
  if (lexemeId >= lexemeCount_ || static_cast<uint8_t>(status) > static_cast<uint8_t>(Status::ImplicitlyFamiliar)) {
    error = StateError::LEXEME_ID_OUT_OF_RANGE;
    return false;
  }
  Status current;
  if (!get(lexemeId, current, error)) return false;
  if (current == status) return true;
  if (generation_ == UINT32_MAX) {
    error = StateError::GENERATION_OVERFLOW;
    return false;
  }
  uint8_t wal[kWalSize]{};
  encodeWal(wal, bundleUuid_, lexemeId, generation_ + 1U, status);
  if (!storage_.writeFileSynced(storage_.context, walPath_, wal, sizeof(wal))) {
    error = StateError::IO_FAILED;
    return false;
  }
  return recover(error);
}

bool Store::readPackedByte(const uint32_t byteIndex, uint8_t& value, StateError& error) {
  value = 0;
  if (!open_ || byteIndex >= packedByteCount()) {
    error = StateError::LEXEME_ID_OUT_OF_RANGE;
    return false;
  }
  if (!storage_.readAt(storage_.context, statusPath_, kStatusPayloadOffset + byteIndex, &value, sizeof(value))) {
    error = StateError::IO_FAILED;
    return false;
  }
  error = StateError::NONE;
  return true;
}

bool Store::visitNonUnseen(const uint32_t firstLexemeId, const uint32_t scanLexemeCount, uint8_t* scratch,
                           const size_t scratchCapacity, void* context, const StatusVisitor visitor,
                           uint32_t& nextLexemeId, StateError& error) {
  nextLexemeId = firstLexemeId;
  if (!recover(error)) return false;
  if (firstLexemeId > lexemeCount_ || scanLexemeCount == 0 || scanLexemeCount > kMaxReviewScanLexemes ||
      scratch == nullptr || visitor == nullptr) {
    error = StateError::INVALID_INPUT;
    return false;
  }
  if (firstLexemeId == lexemeCount_) {
    error = StateError::NONE;
    return true;
  }

  const uint32_t remaining = lexemeCount_ - firstLexemeId;
  uint32_t count = scanLexemeCount < remaining ? scanLexemeCount : remaining;
  const uint64_t capacityLexemes = static_cast<uint64_t>(scratchCapacity) * 2U - (firstLexemeId & 1U);
  if (capacityLexemes == 0) {
    error = StateError::INVALID_INPUT;
    return false;
  }
  if (count > capacityLexemes) count = static_cast<uint32_t>(capacityLexemes);
  const uint32_t endLexemeId = firstLexemeId + count;
  const uint32_t firstByte = firstLexemeId / 2U;
  const uint32_t endByte = (endLexemeId + 1U) / 2U;
  const size_t bytes = endByte - firstByte;
  if (!storage_.readAt(storage_.context, statusPath_, kStatusPayloadOffset + firstByte, scratch, bytes)) {
    error = StateError::IO_FAILED;
    return false;
  }

  for (uint32_t lexemeId = firstLexemeId; lexemeId < endLexemeId; ++lexemeId) {
    const uint8_t packed = scratch[lexemeId / 2U - firstByte];
    const uint8_t value = (lexemeId & 1U) == 0 ? packed & 0x0FU : packed >> 4U;
    if (value > static_cast<uint8_t>(Status::ImplicitlyFamiliar)) {
      error = StateError::STATUS_INVALID;
      return false;
    }
    if (value != static_cast<uint8_t>(Status::Unseen) && !visitor(context, lexemeId, static_cast<Status>(value))) {
      nextLexemeId = lexemeId + 1U;
      error = StateError::NONE;
      return true;
    }
  }
  nextLexemeId = endLexemeId;
  error = StateError::NONE;
  return true;
}

const char* stateErrorName(const StateError error) {
  switch (error) {
    case StateError::NONE:
      return "none";
    case StateError::INVALID_INPUT:
      return "invalid input";
    case StateError::STORAGE_UNAVAILABLE:
      return "storage unavailable";
    case StateError::DIRECTORY_FAILED:
      return "directory failed";
    case StateError::METADATA_INVALID:
      return "metadata invalid";
    case StateError::STATUS_INVALID:
      return "status invalid";
    case StateError::WAL_INVALID:
      return "wal invalid";
    case StateError::IO_FAILED:
      return "io failed";
    case StateError::LEXEME_ID_OUT_OF_RANGE:
      return "lexeme id out of range";
    case StateError::GENERATION_OVERFLOW:
      return "generation overflow";
  }
  return "unknown";
}

}  // namespace dictionary::lexeme_state

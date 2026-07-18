#include "ContextualAttachments.h"

#include <cstdio>
#include <cstring>
#include <limits>

#include "Crc32.h"

namespace dictionary::contextual {

static_assert(sizeof(AttachmentStore) <= 256, "attachment store must remain safe for a small task stack");

namespace {

constexpr uint8_t kMagic[] = {'C', 'X', 'A', 'T'};

uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

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

bool nonzeroUuid(const uint8_t* uuid) {
  for (size_t i = 0; i < 16; ++i) {
    if (uuid[i] != 0) return true;
  }
  return false;
}

bool allZero(const uint8_t* data, const size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (data[i] != 0) return false;
  }
  return true;
}

bool backendValid(const AttachmentStorageBackend& storage) {
  return storage.exists != nullptr && storage.readExact != nullptr && storage.writeSynced != nullptr &&
         storage.remove != nullptr && storage.rename != nullptr;
}

bool appendPath(const char* directory, const char* name, char* output, const size_t capacity) {
  const int written = std::snprintf(output, capacity, "%s/%s", directory, name);
  return written >= 0 && static_cast<size_t>(written) < capacity;
}

}  // namespace

bool parseAttachmentRecord(const uint8_t* data, const size_t length, const uint8_t (&expectedCanonicalUuid)[16],
                           AttachmentRecord& output, AttachmentError& error) {
  output = {};
  error = AttachmentError::NONE;
  if (data == nullptr || !nonzeroUuid(expectedCanonicalUuid)) {
    error = AttachmentError::INVALID_INPUT;
    return false;
  }
  if (length != kAttachmentRecordSize) {
    error = AttachmentError::BAD_RECORD_SIZE;
    return false;
  }
  if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
    error = AttachmentError::BAD_MAGIC;
    return false;
  }
  if (readU16(data + 4) != kAttachmentFormatVersion) {
    error = AttachmentError::UNSUPPORTED_VERSION;
    return false;
  }
  if (readU16(data + 6) != kAttachmentRecordSize) {
    error = AttachmentError::BAD_RECORD_SIZE;
    return false;
  }
  if (dictionary::updateCrc32(0, data, 84) != readU32(data + 84)) {
    error = AttachmentError::BAD_CRC;
    return false;
  }
  if (readU32(data + 8) != 0) {
    error = AttachmentError::UNSUPPORTED_FLAGS;
    return false;
  }
  if (!allZero(data + 33, 3)) {
    error = AttachmentError::RESERVED_FIELD_NONZERO;
    return false;
  }
  if (!nonzeroUuid(data + 12) || std::memcmp(data + 12, expectedCanonicalUuid, 16) != 0) {
    error = AttachmentError::CANONICAL_MISMATCH;
    return false;
  }
  const uint8_t sourceCount = data[32];
  if (sourceCount > kMaxAttachedSources) {
    error = AttachmentError::COUNT_OUT_OF_RANGE;
    return false;
  }
  for (size_t source = 0; source < kMaxAttachedSources; ++source) {
    const uint8_t* uuid = data + 36 + source * 16;
    if (source < sourceCount) {
      if (!nonzeroUuid(uuid)) {
        error = AttachmentError::INVALID_SOURCE_UUID;
        return false;
      }
      for (size_t previous = 0; previous < source; ++previous) {
        if (std::memcmp(uuid, data + 36 + previous * 16, 16) == 0) {
          error = AttachmentError::DUPLICATE_SOURCE_UUID;
          return false;
        }
      }
    } else if (!allZero(uuid, 16)) {
      error = AttachmentError::UNUSED_SLOT_NONZERO;
      return false;
    }
  }

  std::memcpy(output.canonicalUuid, data + 12, sizeof(output.canonicalUuid));
  output.generation = readU32(data + 28);
  output.sourceCount = sourceCount;
  std::memcpy(output.sourceUuids, data + 36, sizeof(output.sourceUuids));
  return true;
}

bool encodeAttachmentRecord(const AttachmentRecord& record, uint8_t (&output)[kAttachmentRecordSize],
                            AttachmentError& error) {
  error = AttachmentError::NONE;
  if (!nonzeroUuid(record.canonicalUuid)) {
    error = AttachmentError::INVALID_INPUT;
    return false;
  }
  if (record.sourceCount > kMaxAttachedSources) {
    error = AttachmentError::COUNT_OUT_OF_RANGE;
    return false;
  }
  for (size_t source = 0; source < kMaxAttachedSources; ++source) {
    if (source < record.sourceCount) {
      if (!nonzeroUuid(record.sourceUuids[source])) {
        error = AttachmentError::INVALID_SOURCE_UUID;
        return false;
      }
      for (size_t previous = 0; previous < source; ++previous) {
        if (std::memcmp(record.sourceUuids[source], record.sourceUuids[previous], 16) == 0) {
          error = AttachmentError::DUPLICATE_SOURCE_UUID;
          return false;
        }
      }
    } else if (!allZero(record.sourceUuids[source], 16)) {
      error = AttachmentError::UNUSED_SLOT_NONZERO;
      return false;
    }
  }

  std::memset(output, 0, kAttachmentRecordSize);
  std::memcpy(output, kMagic, sizeof(kMagic));
  writeU16(output + 4, kAttachmentFormatVersion);
  writeU16(output + 6, kAttachmentRecordSize);
  std::memcpy(output + 12, record.canonicalUuid, sizeof(record.canonicalUuid));
  writeU32(output + 28, record.generation);
  output[32] = record.sourceCount;
  std::memcpy(output + 36, record.sourceUuids, sizeof(record.sourceUuids));
  writeU32(output + 84, dictionary::updateCrc32(0, output, 84));
  return true;
}

bool AttachmentStore::open(const AttachmentStorageBackend& storage, const char* canonicalDirectory,
                           const uint8_t (&canonicalUuid)[16], AttachmentError& error) {
  open_ = false;
  storage_ = {};
  error = AttachmentError::NONE;
  if (!backendValid(storage) || canonicalDirectory == nullptr || canonicalDirectory[0] != '/' ||
      !nonzeroUuid(canonicalUuid)) {
    error = AttachmentError::INVALID_INPUT;
    return false;
  }
  if (!appendPath(canonicalDirectory, "attachments.bin", finalPath_, sizeof(finalPath_))) {
    error = AttachmentError::PATH_TOO_LONG;
    return false;
  }
  storage_ = storage;
  std::memcpy(canonicalUuid_, canonicalUuid, sizeof(canonicalUuid_));
  open_ = true;
  return true;
}

bool AttachmentStore::buildSidePath(const char* name) {
  const char* separator = std::strrchr(finalPath_, '/');
  if (separator == nullptr) return false;
  const size_t directoryLength = static_cast<size_t>(separator - finalPath_);
  const int written =
      std::snprintf(sidePath_, sizeof(sidePath_), "%.*s/%s", static_cast<int>(directoryLength), finalPath_, name);
  return written >= 0 && static_cast<size_t>(written) < sizeof(sidePath_);
}

bool AttachmentStore::readPath(const char* path, AttachmentRecord& output, AttachmentError& error) const {
  uint8_t data[kAttachmentRecordSize]{};
  if (!storage_.readExact(storage_.context, path, data, sizeof(data))) {
    error = AttachmentError::READ_FAILED;
    return false;
  }
  const auto& uuid = *reinterpret_cast<const uint8_t (*)[16]>(canonicalUuid_);
  return parseAttachmentRecord(data, sizeof(data), uuid, output, error);
}

bool AttachmentStore::restoreBackup(AttachmentError& error) {
  if (!buildSidePath("attachments.bak")) {
    error = AttachmentError::PATH_TOO_LONG;
    return false;
  }
  if (storage_.exists(storage_.context, finalPath_) && !storage_.remove(storage_.context, finalPath_)) {
    error = AttachmentError::REMOVE_FAILED;
    return false;
  }
  if (!storage_.rename(storage_.context, sidePath_, finalPath_)) {
    error = AttachmentError::RENAME_FAILED;
    return false;
  }
  return true;
}

bool AttachmentStore::recover(AttachmentError& error) {
  error = AttachmentError::NONE;
  if (!open_) {
    error = AttachmentError::STORAGE_UNAVAILABLE;
    return false;
  }
  const bool finalExists = storage_.exists(storage_.context, finalPath_);
  if (!buildSidePath("attachments.bak")) {
    error = AttachmentError::PATH_TOO_LONG;
    return false;
  }
  const bool backupExists = storage_.exists(storage_.context, sidePath_);
  AttachmentRecord ignored;
  AttachmentError finalError = AttachmentError::NONE;
  if (finalExists && readPath(finalPath_, ignored, finalError)) {
    if (backupExists) storage_.remove(storage_.context, sidePath_);
    if (buildSidePath("attachments.tmp") && storage_.exists(storage_.context, sidePath_)) {
      storage_.remove(storage_.context, sidePath_);
    }
    return true;
  }
  if (backupExists) {
    AttachmentError backupError;
    if (!readPath(sidePath_, ignored, backupError)) {
      error = finalExists ? finalError : backupError;
      return false;
    }
    if (!restoreBackup(error)) return false;
    if (buildSidePath("attachments.tmp") && storage_.exists(storage_.context, sidePath_)) {
      storage_.remove(storage_.context, sidePath_);
    }
    return true;
  }
  if (finalExists) {
    error = finalError;
    return false;
  }
  if (buildSidePath("attachments.tmp") && storage_.exists(storage_.context, sidePath_)) {
    storage_.remove(storage_.context, sidePath_);
  }
  return true;
}

bool AttachmentStore::load(AttachmentRecord& output, AttachmentError& error) {
  output = {};
  if (!recover(error)) return false;
  if (!storage_.exists(storage_.context, finalPath_)) {
    std::memcpy(output.canonicalUuid, canonicalUuid_, sizeof(output.canonicalUuid));
    error = AttachmentError::NONE;
    return true;
  }
  return readPath(finalPath_, output, error);
}

bool AttachmentStore::replace(const uint8_t* sourceUuids, const uint8_t sourceCount, const uint32_t expectedGeneration,
                              const SourceCompatibilityCallback compatibility, void* compatibilityContext,
                              AttachmentRecord& output, AttachmentError& error) {
  output = {};
  if (sourceCount > kMaxAttachedSources || (sourceCount != 0 && sourceUuids == nullptr) ||
      (sourceCount != 0 && compatibility == nullptr)) {
    error = AttachmentError::INVALID_INPUT;
    return false;
  }
  if (!load(output, error)) return false;
  const uint32_t currentGeneration = output.generation;
  if (currentGeneration != expectedGeneration) {
    error = AttachmentError::GENERATION_MISMATCH;
    return false;
  }
  if (currentGeneration == std::numeric_limits<uint32_t>::max()) {
    error = AttachmentError::GENERATION_OVERFLOW;
    return false;
  }

  output = {};
  std::memcpy(output.canonicalUuid, canonicalUuid_, sizeof(output.canonicalUuid));
  output.generation = currentGeneration + 1;
  output.sourceCount = sourceCount;
  for (size_t source = 0; source < sourceCount; ++source) {
    std::memcpy(output.sourceUuids[source], sourceUuids + source * 16, 16);
    const auto& sourceUuid = *reinterpret_cast<const uint8_t (*)[16]>(output.sourceUuids[source]);
    const auto& canonicalUuid = *reinterpret_cast<const uint8_t (*)[16]>(canonicalUuid_);
    if (!compatibility(compatibilityContext, sourceUuid, canonicalUuid)) {
      output = {};
      error = AttachmentError::SOURCE_INCOMPATIBLE;
      return false;
    }
  }

  uint8_t encoded[kAttachmentRecordSize]{};
  if (!encodeAttachmentRecord(output, encoded, error)) return false;
  if (!buildSidePath("attachments.tmp")) {
    error = AttachmentError::PATH_TOO_LONG;
    return false;
  }
  if (!storage_.writeSynced(storage_.context, sidePath_, encoded, sizeof(encoded))) {
    error = AttachmentError::WRITE_FAILED;
    return false;
  }

  const bool replacing = storage_.exists(storage_.context, finalPath_);
  if (!buildSidePath("attachments.bak")) {
    error = AttachmentError::PATH_TOO_LONG;
    return false;
  }
  if (storage_.exists(storage_.context, sidePath_)) storage_.remove(storage_.context, sidePath_);
  if (replacing && !storage_.rename(storage_.context, finalPath_, sidePath_)) {
    error = AttachmentError::RENAME_FAILED;
    return false;
  }
  if (!buildSidePath("attachments.tmp") || !storage_.rename(storage_.context, sidePath_, finalPath_)) {
    if (replacing) {
      AttachmentError restoreError;
      restoreBackup(restoreError);
    }
    error = AttachmentError::RENAME_FAILED;
    return false;
  }
  if (replacing && buildSidePath("attachments.bak")) storage_.remove(storage_.context, sidePath_);
  error = AttachmentError::NONE;
  return true;
}

const char* attachmentErrorName(const AttachmentError error) {
  switch (error) {
    case AttachmentError::NONE:
      return "none";
    case AttachmentError::INVALID_INPUT:
      return "invalid input";
    case AttachmentError::STORAGE_UNAVAILABLE:
      return "storage unavailable";
    case AttachmentError::PATH_TOO_LONG:
      return "path too long";
    case AttachmentError::READ_FAILED:
      return "read failed";
    case AttachmentError::BAD_MAGIC:
      return "bad magic";
    case AttachmentError::UNSUPPORTED_VERSION:
      return "unsupported version";
    case AttachmentError::BAD_RECORD_SIZE:
      return "bad record size";
    case AttachmentError::BAD_CRC:
      return "bad crc";
    case AttachmentError::UNSUPPORTED_FLAGS:
      return "unsupported flags";
    case AttachmentError::RESERVED_FIELD_NONZERO:
      return "reserved field nonzero";
    case AttachmentError::CANONICAL_MISMATCH:
      return "canonical mismatch";
    case AttachmentError::COUNT_OUT_OF_RANGE:
      return "count out of range";
    case AttachmentError::INVALID_SOURCE_UUID:
      return "invalid source uuid";
    case AttachmentError::DUPLICATE_SOURCE_UUID:
      return "duplicate source uuid";
    case AttachmentError::UNUSED_SLOT_NONZERO:
      return "unused slot nonzero";
    case AttachmentError::SOURCE_INCOMPATIBLE:
      return "source incompatible";
    case AttachmentError::GENERATION_MISMATCH:
      return "generation mismatch";
    case AttachmentError::GENERATION_OVERFLOW:
      return "generation overflow";
    case AttachmentError::WRITE_FAILED:
      return "write failed";
    case AttachmentError::RENAME_FAILED:
      return "rename failed";
    case AttachmentError::REMOVE_FAILED:
      return "remove failed";
  }
  return "unknown";
}

}  // namespace dictionary::contextual

#pragma once

#include <cstddef>
#include <cstdint>

namespace dictionary::contextual {

constexpr uint16_t kAttachmentFormatVersion = 1;
constexpr size_t kAttachmentRecordSize = 88;
constexpr size_t kMaxAttachedSources = 3;
constexpr size_t kMaxAttachmentPath = 80;

struct AttachmentRecord {
  uint8_t canonicalUuid[16]{};
  uint32_t generation = 0;
  uint8_t sourceCount = 0;
  uint8_t sourceUuids[kMaxAttachedSources][16]{};
};

enum class AttachmentError : uint8_t {
  NONE = 0,
  INVALID_INPUT,
  STORAGE_UNAVAILABLE,
  PATH_TOO_LONG,
  READ_FAILED,
  BAD_MAGIC,
  UNSUPPORTED_VERSION,
  BAD_RECORD_SIZE,
  BAD_CRC,
  UNSUPPORTED_FLAGS,
  RESERVED_FIELD_NONZERO,
  CANONICAL_MISMATCH,
  COUNT_OUT_OF_RANGE,
  INVALID_SOURCE_UUID,
  DUPLICATE_SOURCE_UUID,
  UNUSED_SLOT_NONZERO,
  SOURCE_INCOMPATIBLE,
  GENERATION_MISMATCH,
  GENERATION_OVERFLOW,
  WRITE_FAILED,
  RENAME_FAILED,
  REMOVE_FAILED,
};

struct AttachmentStorageBackend {
  void* context = nullptr;
  bool (*exists)(void* context, const char* path) = nullptr;
  bool (*readExact)(void* context, const char* path, void* output, size_t length) = nullptr;
  bool (*writeSynced)(void* context, const char* path, const void* data, size_t length) = nullptr;
  bool (*remove)(void* context, const char* path) = nullptr;
  bool (*rename)(void* context, const char* oldPath, const char* newPath) = nullptr;
};

using SourceCompatibilityCallback = bool (*)(void* context, const uint8_t (&sourceUuid)[16],
                                             const uint8_t (&canonicalUuid)[16]);

bool parseAttachmentRecord(const uint8_t* data, size_t length, const uint8_t (&expectedCanonicalUuid)[16],
                           AttachmentRecord& output, AttachmentError& error);
bool encodeAttachmentRecord(const AttachmentRecord& record, uint8_t (&output)[kAttachmentRecordSize],
                            AttachmentError& error);

class AttachmentStore {
 public:
  bool open(const AttachmentStorageBackend& storage, const char* canonicalDirectory, const uint8_t (&canonicalUuid)[16],
            AttachmentError& error);
  bool load(AttachmentRecord& output, AttachmentError& error);
  bool replace(const uint8_t* sourceUuids, uint8_t sourceCount, uint32_t expectedGeneration,
               SourceCompatibilityCallback compatibility, void* compatibilityContext, AttachmentRecord& output,
               AttachmentError& error);
  bool recover(AttachmentError& error);

 private:
  AttachmentStorageBackend storage_{};
  uint8_t canonicalUuid_[16]{};
  char finalPath_[kMaxAttachmentPath]{};
  char sidePath_[kMaxAttachmentPath]{};
  bool open_ = false;

  bool buildSidePath(const char* name);
  bool readPath(const char* path, AttachmentRecord& output, AttachmentError& error) const;
  bool restoreBackup(AttachmentError& error);
};

const char* attachmentErrorName(AttachmentError error);

}  // namespace dictionary::contextual

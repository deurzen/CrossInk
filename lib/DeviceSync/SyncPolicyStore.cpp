#include "SyncPolicyStore.h"

#include <AtomicFile.h>
#include <HalStorage.h>
#include <Logging.h>

#include "SyncPolicyCodec.h"

namespace DeviceSync::SyncPolicyStore {
namespace {
constexpr const char* LOG_MODULE = "SYNC_POLICY";
constexpr AtomicFile::Paths POLICY_PATHS = {POLICY_PATH, POLICY_TEMP_PATH, POLICY_BACKUP_PATH};

struct FileInput {
  HalFile& file;
};

bool readExact(void* context, void* data, const size_t length) {
  auto* input = static_cast<FileInput*>(context);
  return input->file.read(data, length) == static_cast<int>(length);
}

bool writeExact(void* context, const void* data, const size_t length) {
  auto* file = static_cast<HalFile*>(context);
  return file->write(data, length) == length;
}

SyncPolicyCodec::DecodeResult readPolicy(const char* path, SyncPolicy* policy) {
  HalFile file;
  if (!Storage.openFileForRead(LOG_MODULE, path, file)) {
    LOG_ERR(LOG_MODULE, "Could not open sync policy: %s", path);
    return SyncPolicyCodec::DecodeResult::Invalid;
  }

  const uint64_t fileSize = file.fileSize64();
  if (fileSize > SyncPolicyCodec::MAX_ENCODED_SIZE) {
    LOG_ERR(LOG_MODULE, "Sync policy is too large: %s", path);
    file.close();
    return SyncPolicyCodec::DecodeResult::Invalid;
  }

  FileInput fileInput{file};
  const SyncPolicyCodec::Input input{&fileInput, readExact, static_cast<size_t>(fileSize)};
  const SyncPolicyCodec::DecodeResult result =
      policy != nullptr ? SyncPolicyCodec::decode(input, *policy) : SyncPolicyCodec::validate(input);
  if (!file.close()) {
    LOG_ERR(LOG_MODULE, "Could not close sync policy: %s", path);
    if (policy != nullptr) policy->reset();
    return SyncPolicyCodec::DecodeResult::Invalid;
  }
  return result;
}

AtomicFile::ValidationResult validatePolicyFile(const char* path, const void*) {
  switch (readPolicy(path, nullptr)) {
    case SyncPolicyCodec::DecodeResult::Ok:
      return AtomicFile::ValidationResult::Valid;
    case SyncPolicyCodec::DecodeResult::Unsupported:
      return AtomicFile::ValidationResult::Unsupported;
    case SyncPolicyCodec::DecodeResult::Invalid:
      return AtomicFile::ValidationResult::Invalid;
  }
  return AtomicFile::ValidationResult::Invalid;
}

bool writePolicy(HalFile& file, const void* context) {
  if (context == nullptr) return false;
  const auto* policy = static_cast<const SyncPolicy*>(context);
  const SyncPolicyCodec::Output output{&file, writeExact};
  return SyncPolicyCodec::encode(*policy, output);
}

bool hasUnsupportedCandidate() {
  static constexpr const char* CANDIDATES[] = {POLICY_PATH, POLICY_TEMP_PATH, POLICY_BACKUP_PATH};
  for (const char* path : CANDIDATES) {
    if (Storage.exists(path) && readPolicy(path, nullptr) == SyncPolicyCodec::DecodeResult::Unsupported) return true;
  }
  return false;
}

}  // namespace

LoadResult load(SyncPolicy& policy) {
  policy.reset();
  const bool hasCandidate =
      Storage.exists(POLICY_PATH) || Storage.exists(POLICY_TEMP_PATH) || Storage.exists(POLICY_BACKUP_PATH);
  if (!hasCandidate) {
    policy.setDefaults();
    return LoadResult::Missing;
  }

  if (!AtomicFile::recover(LOG_MODULE, POLICY_PATHS, validatePolicyFile)) {
    return hasUnsupportedCandidate() ? LoadResult::Unsupported : LoadResult::Invalid;
  }
  if (!Storage.exists(POLICY_PATH)) {
    policy.setDefaults();
    return LoadResult::Missing;
  }

  switch (readPolicy(POLICY_PATH, &policy)) {
    case SyncPolicyCodec::DecodeResult::Ok:
      return LoadResult::Loaded;
    case SyncPolicyCodec::DecodeResult::Unsupported:
      return LoadResult::Unsupported;
    case SyncPolicyCodec::DecodeResult::Invalid:
      return LoadResult::Invalid;
  }
  policy.reset();
  return LoadResult::Invalid;
}

bool save(const SyncPolicy& policy) {
  if (!Storage.ensureDirectoryExists(DIRECTORY_PATH)) {
    LOG_ERR(LOG_MODULE, "Could not create sync policy directory: %s", DIRECTORY_PATH);
    return false;
  }
  return AtomicFile::write(LOG_MODULE, POLICY_PATHS, writePolicy, validatePolicyFile, &policy);
}

}  // namespace DeviceSync::SyncPolicyStore

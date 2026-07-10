#include "AtomicFile.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace AtomicFile {
namespace {

const char* logModule(const char* moduleName) { return moduleName != nullptr ? moduleName : "ATOMIC"; }

bool sameParentDirectory(const char* left, const char* right) {
  const char* leftSlash = std::strrchr(left, '/');
  const char* rightSlash = std::strrchr(right, '/');
  if (leftSlash == nullptr || rightSlash == nullptr) return false;
  const size_t leftLength = static_cast<size_t>(leftSlash - left);
  const size_t rightLength = static_cast<size_t>(rightSlash - right);
  return leftLength == rightLength && std::strncmp(left, right, leftLength) == 0;
}

bool validPaths(const Paths& paths) {
  if (paths.finalPath == nullptr || paths.tempPath == nullptr || paths.backupPath == nullptr ||
      paths.finalPath[0] != '/' || paths.tempPath[0] != '/' || paths.backupPath[0] != '/') {
    return false;
  }
  return std::strcmp(paths.finalPath, paths.tempPath) != 0 && std::strcmp(paths.finalPath, paths.backupPath) != 0 &&
         std::strcmp(paths.tempPath, paths.backupPath) != 0 && sameParentDirectory(paths.finalPath, paths.tempPath) &&
         sameParentDirectory(paths.finalPath, paths.backupPath);
}

ValidationResult validateIfPresent(const char* path, ValidateCallback validator, const void* context) {
  return Storage.exists(path) ? validator(path, context) : ValidationResult::Invalid;
}

bool removeIfPresent(const char* moduleName, const char* path) {
  if (!Storage.exists(path)) return true;
  if (Storage.remove(path)) return true;
  LOG_ERR(logModule(moduleName), "Could not remove atomic file: %s", path);
  return false;
}

bool promoteCandidate(const char* moduleName, const char* candidatePath, const char* finalPath,
                      ValidateCallback validator, const void* context) {
  if (!Storage.rename(candidatePath, finalPath)) {
    LOG_ERR(logModule(moduleName), "Could not restore atomic file %s from %s", finalPath, candidatePath);
    return false;
  }
  if (validator(finalPath, context) == ValidationResult::Valid) return true;
  LOG_ERR(logModule(moduleName), "Restored atomic file failed validation: %s", finalPath);
  return false;
}

}  // namespace

bool recover(const char* moduleName, const Paths& paths, ValidateCallback validator, const void* context) {
  if (validator == nullptr || !validPaths(paths)) {
    LOG_ERR(logModule(moduleName), "Invalid atomic file validator or paths");
    return false;
  }

  const bool finalExists = Storage.exists(paths.finalPath);
  const ValidationResult finalResult = validateIfPresent(paths.finalPath, validator, context);
  const ValidationResult backupResult = validateIfPresent(paths.backupPath, validator, context);
  const ValidationResult tempResult = validateIfPresent(paths.tempPath, validator, context);
  if (finalResult == ValidationResult::Unsupported || backupResult == ValidationResult::Unsupported ||
      tempResult == ValidationResult::Unsupported) {
    LOG_ERR(logModule(moduleName), "Unsupported atomic file version preserved: %s", paths.finalPath);
    return false;
  }

  if (finalResult == ValidationResult::Valid) {
    // A valid final is authoritative. A temp can only be an interrupted newer
    // attempt; choosing the old final is the conservative old-or-new recovery.
    return removeIfPresent(moduleName, paths.tempPath);
  }

  if (backupResult == ValidationResult::Valid) {
    if (finalExists && !removeIfPresent(moduleName, paths.finalPath)) return false;
    if (!promoteCandidate(moduleName, paths.backupPath, paths.finalPath, validator, context)) return false;
    if (!removeIfPresent(moduleName, paths.tempPath)) return false;
    LOG_DBG(logModule(moduleName), "Recovered atomic file from backup: %s", paths.finalPath);
    return true;
  }

  if (tempResult == ValidationResult::Valid) {
    if (finalExists && !removeIfPresent(moduleName, paths.finalPath)) return false;
    if (!promoteCandidate(moduleName, paths.tempPath, paths.finalPath, validator, context)) return false;
    LOG_DBG(logModule(moduleName), "Recovered atomic file from temp: %s", paths.finalPath);
    return true;
  }

  if (finalExists) {
    LOG_ERR(logModule(moduleName), "Atomic file and recovery candidates are invalid: %s", paths.finalPath);
    return false;
  }

  // No authoritative file is a valid first-run state, but invalid remnants
  // must not be mistaken for a successful recovery.
  const bool hadRemnant = Storage.exists(paths.tempPath) || Storage.exists(paths.backupPath);
  if (hadRemnant) {
    LOG_ERR(logModule(moduleName), "No valid atomic file candidate for: %s", paths.finalPath);
    return false;
  }
  return true;
}

bool write(const char* moduleName, const Paths& paths, WriteCallback writer, ValidateCallback validator,
           const void* context) {
  if (writer == nullptr || validator == nullptr || !validPaths(paths)) {
    LOG_ERR(logModule(moduleName), "Invalid atomic file write arguments");
    return false;
  }
  if (!recover(moduleName, paths, validator, context)) return false;
  if (!removeIfPresent(moduleName, paths.tempPath)) return false;

  HalFile temp;
  if (!Storage.openFileForWrite(logModule(moduleName), paths.tempPath, temp)) {
    LOG_ERR(logModule(moduleName), "Could not open atomic temp file: %s", paths.tempPath);
    return false;
  }

  if (!writer(temp, context)) {
    LOG_ERR(logModule(moduleName), "Could not write atomic temp file: %s", paths.tempPath);
    temp.close();
    removeIfPresent(moduleName, paths.tempPath);
    return false;
  }
  if (!temp.sync()) {
    LOG_ERR(logModule(moduleName), "Could not sync atomic temp file: %s", paths.tempPath);
    temp.close();
    removeIfPresent(moduleName, paths.tempPath);
    return false;
  }
  if (!temp.close()) {
    LOG_ERR(logModule(moduleName), "Could not close atomic temp file: %s", paths.tempPath);
    removeIfPresent(moduleName, paths.tempPath);
    return false;
  }
  const ValidationResult tempResult = validator(paths.tempPath, context);
  if (tempResult != ValidationResult::Valid) {
    LOG_ERR(logModule(moduleName), "Atomic temp file failed validation: %s", paths.tempPath);
    if (tempResult == ValidationResult::Invalid) removeIfPresent(moduleName, paths.tempPath);
    return false;
  }

  if (!removeIfPresent(moduleName, paths.backupPath)) return false;

  const bool hadFinal = Storage.exists(paths.finalPath);
  if (hadFinal && !Storage.rename(paths.finalPath, paths.backupPath)) {
    LOG_ERR(logModule(moduleName), "Could not rotate atomic backup: %s", paths.finalPath);
    removeIfPresent(moduleName, paths.tempPath);
    return false;
  }

  if (!Storage.rename(paths.tempPath, paths.finalPath)) {
    LOG_ERR(logModule(moduleName), "Could not promote atomic temp file: %s", paths.finalPath);
    if (hadFinal && Storage.exists(paths.backupPath) && !Storage.exists(paths.finalPath) &&
        !Storage.rename(paths.backupPath, paths.finalPath)) {
      LOG_ERR(logModule(moduleName), "Could not roll back atomic file: %s", paths.finalPath);
    }
    return false;
  }

  const ValidationResult promotedResult = validator(paths.finalPath, context);
  if (promotedResult == ValidationResult::Valid) return true;

  LOG_ERR(logModule(moduleName), "Promoted atomic file failed validation: %s", paths.finalPath);
  if (promotedResult == ValidationResult::Unsupported) return false;
  if (hadFinal && validateIfPresent(paths.backupPath, validator, context) == ValidationResult::Valid) {
    if (!removeIfPresent(moduleName, paths.finalPath) || !Storage.rename(paths.backupPath, paths.finalPath)) {
      LOG_ERR(logModule(moduleName), "Could not restore atomic backup after validation failure: %s", paths.finalPath);
    }
  }
  return false;
}

bool remove(const char* moduleName, const Paths& paths) {
  if (!validPaths(paths)) {
    LOG_ERR(logModule(moduleName), "Invalid atomic file removal paths");
    return false;
  }
  // Sidecars go first. If power is lost before the final is removed, the
  // authoritative file remains and no recovery candidate can resurrect it.
  return removeIfPresent(moduleName, paths.tempPath) && removeIfPresent(moduleName, paths.backupPath) &&
         removeIfPresent(moduleName, paths.finalPath);
}

bool canonicalName(const char* entryName, const char* requiredSuffix, char* output, const size_t outputSize,
                   bool& isSidecar) {
  if (entryName == nullptr || output == nullptr || outputSize == 0) return false;

  size_t length = std::strlen(entryName);
  isSidecar = false;
  static constexpr const char* sidecarSuffixes[] = {".tmp", ".bak"};
  for (const char* suffix : sidecarSuffixes) {
    const size_t suffixLength = std::strlen(suffix);
    if (length >= suffixLength && std::strcmp(entryName + length - suffixLength, suffix) == 0) {
      length -= suffixLength;
      isSidecar = true;
      break;
    }
  }

  const size_t requiredSuffixLength = requiredSuffix != nullptr ? std::strlen(requiredSuffix) : 0;
  if (length >= outputSize || length < requiredSuffixLength ||
      (requiredSuffixLength != 0 &&
       std::strncmp(entryName + length - requiredSuffixLength, requiredSuffix, requiredSuffixLength) != 0)) {
    return false;
  }

  std::memcpy(output, entryName, length);
  output[length] = '\0';
  return true;
}

}  // namespace AtomicFile

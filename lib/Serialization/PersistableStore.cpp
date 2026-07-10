#include "PersistableStore.h"

#include <AtomicFile.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstdio>

namespace {
constexpr size_t JSON_STORE_PATH_MAX = 96;

class HalFileJsonReader {
 public:
  explicit HalFileJsonReader(HalFile& file) : file(file) {}

  int read() { return file.read(); }
  size_t readBytes(char* buffer, const size_t length) {
    const int count = file.read(buffer, length);
    return count > 0 ? static_cast<size_t>(count) : 0;
  }

 private:
  HalFile& file;
};

struct JsonWriteContext {
  const JsonDocument* document;
};

bool makeAtomicPaths(const char* path, char (&tempPath)[JSON_STORE_PATH_MAX], char (&backupPath)[JSON_STORE_PATH_MAX]) {
  const int tempLength = snprintf(tempPath, sizeof(tempPath), "%s.tmp", path ? path : "");
  const int backupLength = snprintf(backupPath, sizeof(backupPath), "%s.bak", path ? path : "");
  if (path == nullptr || tempLength < 0 || static_cast<size_t>(tempLength) >= sizeof(tempPath) || backupLength < 0 ||
      static_cast<size_t>(backupLength) >= sizeof(backupPath)) {
    LOG_ERR("PERSIST", "JSON store path is invalid or too long");
    return false;
  }
  return true;
}

DeserializationError parseJsonFile(const char* path, JsonDocument& document) {
  HalFile file;
  if (!Storage.openFileForRead("PERSIST", path, file)) return DeserializationError::InvalidInput;
  HalFileJsonReader reader(file);
  const DeserializationError error = deserializeJson(document, reader);
  file.close();
  return error;
}

AtomicFile::ValidationResult validateJsonFile(const char* path, const void*) {
  JsonDocument document;
  return parseJsonFile(path, document) ? AtomicFile::ValidationResult::Invalid : AtomicFile::ValidationResult::Valid;
}

bool writeJsonFile(HalFile& file, const void* context) {
  const auto* writeContext = static_cast<const JsonWriteContext*>(context);
  const size_t expected = measureJson(*writeContext->document);
  const size_t written = serializeJson(*writeContext->document, file);
  if (written == expected) return true;
  LOG_ERR("PERSIST", "Short JSON write: %u/%u bytes", static_cast<unsigned>(written), static_cast<unsigned>(expected));
  return false;
}
}  // namespace

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  Storage.mkdir("/.crosspoint");
  char tempPath[JSON_STORE_PATH_MAX];
  char backupPath[JSON_STORE_PATH_MAX];
  if (!makeAtomicPaths(path, tempPath, backupPath)) return false;

  const AtomicFile::Paths paths{path, tempPath, backupPath};
  const JsonWriteContext context{&doc};
  if (!AtomicFile::write("PERSIST", paths, writeJsonFile, validateJsonFile, &context)) {
    LOG_ERR("PERSIST", "Failed to write %s", path);
    return false;
  }
  return true;
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  char tempPath[JSON_STORE_PATH_MAX];
  char backupPath[JSON_STORE_PATH_MAX];
  if (!makeAtomicPaths(path, tempPath, backupPath)) return false;

  const AtomicFile::Paths paths{path, tempPath, backupPath};
  if (!AtomicFile::recover("PERSIST", paths, validateJsonFile)) {
    LOG_ERR("PERSIST", "Failed to recover %s", path);
    return false;
  }
  if (!Storage.exists(path)) return false;  // Expected on first boot — not an error.

  const DeserializationError error = parseJsonFile(path, doc);
  if (error) {
    LOG_ERR("PERSIST", "JSON parse error in %s: %s", path, error.c_str());
    return false;
  }
  return true;
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave) {
  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &status);
  if (status == obfuscation::DecodeStatus::LEGACY && !pass.empty()) {
    needsResave = true;
  }
  if (status == obfuscation::DecodeStatus::INVALID || status == obfuscation::DecodeStatus::EMPTY || pass.empty()) {
    // Deobfuscation failed or no obfuscated password was stored; fall back to legacy plaintext.
    pass = doc["password"] | "";
    if (!pass.empty()) needsResave = true;
  }
  return pass;
}

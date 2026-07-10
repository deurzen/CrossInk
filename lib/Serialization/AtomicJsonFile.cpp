#include "AtomicJsonFile.h"

#include <AtomicFile.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

namespace AtomicJsonFile {
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

struct JsonContext {
  const char* moduleName;
  const JsonDocument* document;
};

const char* logModule(const char* moduleName) { return moduleName ? moduleName : "JSON"; }

bool makeAtomicPaths(const char* moduleName, const char* path, char (&tempPath)[JSON_STORE_PATH_MAX],
                     char (&backupPath)[JSON_STORE_PATH_MAX]) {
  const int tempLength = snprintf(tempPath, sizeof(tempPath), "%s.tmp", path ? path : "");
  const int backupLength = snprintf(backupPath, sizeof(backupPath), "%s.bak", path ? path : "");
  if (path == nullptr || tempLength < 0 || static_cast<size_t>(tempLength) >= sizeof(tempPath) || backupLength < 0 ||
      static_cast<size_t>(backupLength) >= sizeof(backupPath)) {
    LOG_ERR(logModule(moduleName), "JSON store path is invalid or too long");
    return false;
  }
  return true;
}

DeserializationError parseJsonFile(const char* moduleName, const char* path, JsonDocument& document) {
  HalFile file;
  if (!Storage.openFileForRead(logModule(moduleName), path, file)) return DeserializationError::InvalidInput;
  HalFileJsonReader reader(file);
  const DeserializationError error = deserializeJson(document, reader);
  file.close();
  return error;
}

AtomicFile::ValidationResult validateJsonFile(const char* path, const void* context) {
  const auto* jsonContext = static_cast<const JsonContext*>(context);
  JsonDocument document;
  return parseJsonFile(jsonContext->moduleName, path, document) ? AtomicFile::ValidationResult::Invalid
                                                                : AtomicFile::ValidationResult::Valid;
}

bool writeJsonFile(HalFile& file, const void* context) {
  const auto* jsonContext = static_cast<const JsonContext*>(context);
  const size_t expected = measureJson(*jsonContext->document);
  const size_t written = serializeJson(*jsonContext->document, file);
  if (written == expected) return true;
  LOG_ERR(logModule(jsonContext->moduleName), "Short JSON write: %u/%u bytes", static_cast<unsigned>(written),
          static_cast<unsigned>(expected));
  return false;
}
}  // namespace

bool write(const char* moduleName, const char* path, const JsonDocument& document) {
  char tempPath[JSON_STORE_PATH_MAX];
  char backupPath[JSON_STORE_PATH_MAX];
  if (!makeAtomicPaths(moduleName, path, tempPath, backupPath)) return false;

  const AtomicFile::Paths paths{path, tempPath, backupPath};
  const JsonContext context{moduleName, &document};
  if (!AtomicFile::write(logModule(moduleName), paths, writeJsonFile, validateJsonFile, &context)) {
    LOG_ERR(logModule(moduleName), "Failed to write %s", path);
    return false;
  }
  return true;
}

bool read(const char* moduleName, const char* path, JsonDocument& document) {
  char tempPath[JSON_STORE_PATH_MAX];
  char backupPath[JSON_STORE_PATH_MAX];
  if (!makeAtomicPaths(moduleName, path, tempPath, backupPath)) return false;

  const AtomicFile::Paths paths{path, tempPath, backupPath};
  const JsonContext context{moduleName, nullptr};
  if (!AtomicFile::recover(logModule(moduleName), paths, validateJsonFile, &context)) {
    LOG_ERR(logModule(moduleName), "Failed to recover %s", path);
    return false;
  }
  if (!Storage.exists(path)) return false;

  const DeserializationError error = parseJsonFile(moduleName, path, document);
  if (error) {
    LOG_ERR(logModule(moduleName), "JSON parse error in %s: %s", path, error.c_str());
    return false;
  }
  return true;
}

}  // namespace AtomicJsonFile

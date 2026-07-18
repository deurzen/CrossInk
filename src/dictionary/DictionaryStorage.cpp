#include "DictionaryStorage.h"

#include <HalStorage.h>
#include <esp_task_wdt.h>

#include <cstring>

#include "Crc32.h"

namespace dictionary::storage {
namespace {

bool ensureDirectory(void*, const char* path) { return Storage.ensureDirectoryExists(path); }
bool exists(void*, const char* path) { return Storage.exists(path); }
bool removeTree(void*, const char* path) { return Storage.removeDir(path); }
bool rename(void*, const char* oldPath, const char* newPath) { return Storage.rename(oldPath, newPath); }

uint64_t fileSize(void*, const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("DIN", path, file)) return UINT64_MAX;
  return file.fileSize64();
}

bool readAt(void*, const char* path, const uint32_t offset, void* output, const size_t length) {
  HalFile file;
  return Storage.openFileForRead("DIN", path, file) && file.seek(offset) &&
         file.read(output, length) == static_cast<int>(length);
}

bool validateCrc(void*, const char* path, const uint32_t expectedCrc, uint8_t* scratch, const size_t scratchSize) {
  if (scratch == nullptr || scratchSize == 0) return false;
  HalFile file;
  if (!Storage.openFileForRead("DIN", path, file)) return false;
  uint32_t crc = 0;
  uint64_t remaining = file.fileSize64();
  while (remaining > 0) {
    esp_task_wdt_reset();
    const size_t chunk = remaining < scratchSize ? static_cast<size_t>(remaining) : scratchSize;
    const int read = file.read(scratch, chunk);
    if (read != static_cast<int>(chunk)) return false;
    crc = dictionary::updateCrc32(crc, scratch, chunk);
    remaining -= chunk;
  }
  return crc == expectedCrc;
}

bool parseDirectoryName(const char* name, uint8_t (&uuid)[16]) {
  if (name == nullptr) return false;
  if (std::strlen(name) == 32) return parseUuid(name, uuid);
  static constexpr char BACKUP_PREFIX[] = ".backup-";
  static constexpr char REMOVAL_PREFIX[] = ".removing-";
  if (std::strncmp(name, BACKUP_PREFIX, sizeof(BACKUP_PREFIX) - 1) == 0 &&
      std::strlen(name + sizeof(BACKUP_PREFIX) - 1) == 32) {
    return parseUuid(name + sizeof(BACKUP_PREFIX) - 1, uuid);
  }
  if (std::strncmp(name, REMOVAL_PREFIX, sizeof(REMOVAL_PREFIX) - 1) == 0 &&
      std::strlen(name + sizeof(REMOVAL_PREFIX) - 1) == 32) {
    return parseUuid(name + sizeof(REMOVAL_PREFIX) - 1, uuid);
  }
  return false;
}

}  // namespace

installer::StorageBackend backend() {
  return {nullptr, ensureDirectory, exists, removeTree, rename, fileSize, readAt, validateCrc};
}

bool parseUuid(const char* text, uint8_t (&uuid)[16]) { return installer::parseBundleUuid(text, uuid); }

void formatUuid(const uint8_t (&uuid)[16], char (&output)[37]) { installer::formatBundleUuid(uuid, output); }

bool collectPackageUuids(const char* rootPath, uint8_t* output, const size_t capacity, size_t& count) {
  count = 0;
  if (rootPath == nullptr || output == nullptr || capacity == 0 || !Storage.ensureDirectoryExists(rootPath)) {
    return false;
  }
  HalFile root = Storage.open(rootPath);
  if (!root || !root.isDirectory()) return false;

  char name[64]{};
  for (HalFile entry = root.openNextFile(); entry && count < capacity; entry = root.openNextFile()) {
    if (!entry.isDirectory()) continue;
    entry.getName(name, sizeof(name));
    uint8_t uuid[16]{};
    if (!parseDirectoryName(name, uuid)) continue;
    bool duplicate = false;
    for (size_t i = 0; i < count; ++i) {
      if (std::memcmp(output + i * 16, uuid, 16) == 0) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      std::memcpy(output + count * 16, uuid, 16);
      ++count;
    }
  }
  return true;
}

bool collectBundleUuids(uint8_t* output, const size_t capacity, size_t& count) {
  return collectPackageUuids(ROOT_PATH, output, capacity, count);
}

}  // namespace dictionary::storage

#include "LanguageStateStorage.h"

#include <HalStorage.h>

#include <algorithm>
#include <cstring>

namespace dictionary::language_state_storage {
namespace {

bool ensureDirectory(void*, const char* path) { return Storage.ensureDirectoryExists(path); }
bool exists(void*, const char* path) { return Storage.exists(path); }
bool remove(void*, const char* path) { return Storage.remove(path); }
bool rename(void*, const char* oldPath, const char* newPath) { return Storage.rename(oldPath, newPath); }
uint64_t fileSize(void*, const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("LST", path, file)) return UINT64_MAX;
  return file.fileSize64();
}
bool readAt(void*, const char* path, const uint32_t offset, void* output, const size_t length) {
  HalFile file;
  return Storage.openFileForRead("LST", path, file) && file.seek(offset) &&
         file.read(output, length) == static_cast<int>(length);
}
bool writeAtSynced(void*, const char* path, const uint32_t offset, const void* data, const size_t length) {
  HalFile file = Storage.open(path, O_RDWR);
  return file && file.seek(offset) && file.write(data, length) == length && file.sync();
}
bool writeFileSynced(void*, const char* path, const void* data, const size_t length) {
  HalFile file;
  return Storage.openFileForWrite("LST", path, file) && file.write(data, length) == length && file.sync();
}
bool createFileSynced(void*, const char* path, const void* prefix, const size_t prefixLength,
                      const uint32_t totalSize) {
  if (prefixLength > totalSize) return false;
  HalFile file;
  if (!Storage.openFileForWrite("LST", path, file) || file.write(prefix, prefixLength) != prefixLength) return false;
  static constexpr uint8_t zeros[64]{};
  uint32_t remaining = totalSize - prefixLength;
  while (remaining > 0) {
    const size_t chunk = std::min<size_t>(sizeof(zeros), remaining);
    if (file.write(zeros, chunk) != chunk) return false;
    remaining -= chunk;
  }
  return file.sync();
}

}  // namespace

lexeme_state::StorageBackend backend() {
  return {nullptr, ensureDirectory, exists,          remove,           rename,
          readAt,  writeAtSynced,   writeFileSynced, createFileSynced, fileSize};
}

}  // namespace dictionary::language_state_storage

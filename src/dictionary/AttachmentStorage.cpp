#include "AttachmentStorage.h"

#include <HalStorage.h>

namespace dictionary::attachment_storage {
namespace {

bool exists(void*, const char* path) { return Storage.exists(path); }

bool readExact(void*, const char* path, void* output, const size_t length) {
  HalFile file;
  if (!Storage.openFileForRead("DAT", path, file) || file.fileSize64() != length ||
      file.read(output, length) != static_cast<int>(length)) {
    file.close();
    return false;
  }
  return file.close();
}

bool writeSynced(void*, const char* path, const void* data, const size_t length) {
  HalFile file;
  if (!Storage.openFileForWrite("DAT", path, file) || file.write(data, length) != length || !file.sync()) {
    file.close();
    return false;
  }
  return file.close();
}

bool remove(void*, const char* path) { return !Storage.exists(path) || Storage.remove(path); }

bool rename(void*, const char* oldPath, const char* newPath) { return Storage.rename(oldPath, newPath); }

}  // namespace

contextual::AttachmentStorageBackend backend() { return {nullptr, exists, readExact, writeSynced, remove, rename}; }

}  // namespace dictionary::attachment_storage

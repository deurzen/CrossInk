#include "LanguageStateStorage.h"

#include <HalStorage.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cstring>

namespace dictionary::language_state_storage {
namespace {

io_metrics::Counters* metrics(void* context) { return static_cast<io_metrics::Counters*>(context); }

uint32_t pathToken(const char* path) {
  uint32_t value = 2166136261U;
  if (!path) return value;
  while (*path != '\0') {
    value ^= static_cast<uint8_t>(*path++);
    value *= 16777619U;
  }
  return value == 0 ? 1 : value;
}

void noteSource(io_metrics::Counters* counters, const char* path) {
  if (counters) counters->noteSource(pathToken(path));
}

bool ensureDirectory(void* context, const char* path) {
  if (auto* counters = metrics(context)) ++counters->metadataCalls;
  return Storage.ensureDirectoryExists(path);
}

bool exists(void* context, const char* path) {
  if (auto* counters = metrics(context)) ++counters->metadataCalls;
  return Storage.exists(path);
}

bool remove(void* context, const char* path) {
  if (auto* counters = metrics(context)) ++counters->metadataCalls;
  return Storage.remove(path);
}

bool rename(void* context, const char* oldPath, const char* newPath) {
  if (auto* counters = metrics(context)) ++counters->metadataCalls;
  return Storage.rename(oldPath, newPath);
}

uint64_t fileSize(void* context, const char* path) {
  auto* counters = metrics(context);
  noteSource(counters, path);
  if (counters) ++counters->openAttempts;
  HalFile file;
  if (!Storage.openFileForRead("LST", path, file)) return UINT64_MAX;
  return file.fileSize64();
}

bool readAt(void* context, const char* path, const uint32_t offset, void* output, const size_t length) {
  auto* counters = metrics(context);
  noteSource(counters, path);
  if (counters) ++counters->openAttempts;
  HalFile file;
  if (!Storage.openFileForRead("LST", path, file)) return false;
  if (counters) ++counters->seekAttempts;
  if (!file.seek(offset)) return false;
  if (counters) ++counters->readCalls;
  const int bytesRead = file.read(output, length);
  if (bytesRead > 0 && counters) counters->bytesRead += static_cast<uint32_t>(bytesRead);
  return bytesRead == static_cast<int>(length);
}

bool writeAtSynced(void* context, const char* path, const uint32_t offset, const void* data, const size_t length) {
  auto* counters = metrics(context);
  noteSource(counters, path);
  if (counters) ++counters->openAttempts;
  HalFile file = Storage.open(path, O_RDWR);
  if (!file) return false;
  if (counters) ++counters->seekAttempts;
  if (!file.seek(offset)) return false;
  if (counters) ++counters->writeCalls;
  const size_t bytesWritten = file.write(data, length);
  if (counters) counters->bytesWritten += bytesWritten;
  if (bytesWritten != length) return false;
  if (counters) ++counters->syncCalls;
  return file.sync();
}

bool writeFileSynced(void* context, const char* path, const void* data, const size_t length) {
  auto* counters = metrics(context);
  noteSource(counters, path);
  if (counters) ++counters->openAttempts;
  HalFile file;
  if (!Storage.openFileForWrite("LST", path, file)) return false;
  if (counters) ++counters->writeCalls;
  const size_t bytesWritten = file.write(data, length);
  if (counters) counters->bytesWritten += bytesWritten;
  if (bytesWritten != length) return false;
  if (counters) ++counters->syncCalls;
  return file.sync();
}

bool createFileSynced(void* context, const char* path, const void* prefix, const size_t prefixLength,
                      const uint32_t totalSize) {
  if (prefixLength > totalSize) return false;
  auto* counters = metrics(context);
  noteSource(counters, path);
  if (counters) ++counters->openAttempts;
  HalFile file;
  if (!Storage.openFileForWrite("LST", path, file)) return false;
  if (counters) ++counters->writeCalls;
  const size_t prefixWritten = file.write(prefix, prefixLength);
  if (counters) counters->bytesWritten += prefixWritten;
  if (prefixWritten != prefixLength) return false;

  // Flash-resident zero block keeps initialization sequential without adding
  // stack/heap pressure or issuing thousands of tiny SD writes.
  static constexpr uint8_t zeros[2048]{};
  uint32_t remaining = totalSize - prefixLength;
  while (remaining > 0) {
    const size_t chunk = std::min<size_t>(sizeof(zeros), remaining);
    if (counters) ++counters->writeCalls;
    const size_t bytesWritten = file.write(zeros, chunk);
    if (counters) counters->bytesWritten += bytesWritten;
    if (bytesWritten != chunk) return false;
    remaining -= chunk;
    esp_task_wdt_reset();
  }
  if (counters) ++counters->syncCalls;
  return file.sync();
}

}  // namespace

lexeme_state::StorageBackend backend(io_metrics::Counters* metrics) {
  return {metrics, ensureDirectory, exists,          remove,           rename,
          readAt,  writeAtSynced,   writeFileSynced, createFileSynced, fileSize};
}

}  // namespace dictionary::language_state_storage

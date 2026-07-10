#include <HalStorage.h>

#include <algorithm>
#include <cstring>

void HalStorage::reset() {
  files.clear();
  mutations = 0;
  powerCutMutation = 0;
  shortWrite = false;
  syncFailure = false;
  closeFailure = false;
  openFailure = false;
  removeFailure = false;
  renameCalls = 0;
  failedRenameCall = 0;
}

const std::string* HalStorage::getFile(const std::string& path) const {
  const auto it = files.find(path);
  return it == files.end() ? nullptr : &it->second;
}

void HalStorage::cutPowerAfterMutation(const size_t mutation) { powerCutMutation = mutation; }

void HalStorage::mutated() {
  ++mutations;
  if (powerCutMutation != 0 && mutations == powerCutMutation) throw FakePowerLoss();
}

bool HalStorage::remove(const char* path) {
  const auto it = files.find(path);
  if (it == files.end() || removeFailure) return false;
  files.erase(it);
  mutated();
  return true;
}

bool HalStorage::rename(const char* oldPath, const char* newPath) {
  ++renameCalls;
  const auto it = files.find(oldPath);
  if (it == files.end() || files.find(newPath) != files.end() || renameCalls == failedRenameCall) return false;
  files.emplace(newPath, std::move(it->second));
  files.erase(it);
  mutated();
  return true;
}

bool HalStorage::openFileForWrite(const char*, const char* path, HalFile& file) {
  if (openFailure) return false;
  files[path].clear();
  file = HalFile(this, path);
  mutated();
  return true;
}

size_t HalFile::write(const void* data, const size_t count) {
  if (!open || storage == nullptr) return 0;
  const size_t written = storage->shortWrite && count > 0 ? count - 1 : count;
  storage->files[path].append(static_cast<const char*>(data), written);
  storage->mutated();
  return written;
}

bool HalFile::sync() {
  if (!open || storage == nullptr) return false;
  storage->mutated();
  return !storage->syncFailure;
}

bool HalFile::close() {
  if (!open || storage == nullptr) return true;
  open = false;
  storage->mutated();
  return !storage->closeFailure;
}

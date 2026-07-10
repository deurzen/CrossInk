#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

class FakePowerLoss : public std::runtime_error {
 public:
  FakePowerLoss() : std::runtime_error("simulated power loss") {}
};

class HalStorage;

class HalFile {
  friend class HalStorage;

 public:
  HalFile() = default;
  ~HalFile() = default;
  HalFile(HalFile&& other) noexcept { *this = std::move(other); }
  HalFile& operator=(HalFile&& other) noexcept {
    if (this == &other) return *this;
    storage = other.storage;
    path = std::move(other.path);
    open = other.open;
    other.storage = nullptr;
    other.open = false;
    return *this;
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  size_t write(const void* data, size_t count);
  bool sync();
  bool close();
  bool isOpen() const { return open; }

 private:
  HalFile(HalStorage* storage, std::string path) : storage(storage), path(std::move(path)), open(true) {}

  HalStorage* storage = nullptr;
  std::string path;
  bool open = false;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  void reset();
  void setFile(const std::string& path, const std::string& data) { files[path] = data; }
  const std::string* getFile(const std::string& path) const;
  void cutPowerAfterMutation(size_t mutation);
  void disablePowerCut() { powerCutMutation = 0; }
  size_t mutationCount() const { return mutations; }
  void setShortWrite(bool enabled) { shortWrite = enabled; }
  void setSyncFailure(bool enabled) { syncFailure = enabled; }
  void setCloseFailure(bool enabled) { closeFailure = enabled; }
  void setOpenFailure(bool enabled) { openFailure = enabled; }
  void setRemoveFailure(bool enabled) { removeFailure = enabled; }
  void failRenameCall(size_t call) { failedRenameCall = call; }

  bool exists(const char* path) const { return files.find(path) != files.end(); }
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool openFileForWrite(const char*, const char* path, HalFile& file);

 private:
  friend class HalFile;

  void mutated();

  std::unordered_map<std::string, std::string> files;
  size_t mutations = 0;
  size_t powerCutMutation = 0;
  bool shortWrite = false;
  bool syncFailure = false;
  bool closeFailure = false;
  bool openFailure = false;
  bool removeFailure = false;
  size_t renameCalls = 0;
  size_t failedRenameCall = 0;
};

#define Storage HalStorage::getInstance()

#pragma once

#include <cstddef>
#include <cstdint>

#include "PairRecord.h"

namespace DeviceSync {

// Holds the 212-byte transaction path workspace. Keep this as an activity or
// session member rather than constructing it on a task stack for every write.
class PairRecordStore {
 public:
  enum class LoadResult : uint8_t {
    Loaded,
    Missing,
    Invalid,
    Unsupported,
  };

  PairRecordStore() = default;
  PairRecordStore(const PairRecordStore&) = delete;
  PairRecordStore& operator=(const PairRecordStore&) = delete;

  LoadResult load(const DeviceId& peerDeviceId, PairRecord& record);
  bool save(const PairRecord& record);
  bool remove(const DeviceId& peerDeviceId);

  // Valid after load/save/remove has prepared a valid peer ID.
  const char* finalPath() const { return finalPath_; }

 private:
  static constexpr size_t FINAL_PATH_BYTES = 68;
  static constexpr size_t SIDECAR_PATH_BYTES = 72;

  bool preparePaths(const DeviceId& peerDeviceId);

  char finalPath_[FINAL_PATH_BYTES] = {};
  char tempPath_[SIDECAR_PATH_BYTES] = {};
  char backupPath_[SIDECAR_PATH_BYTES] = {};
};

}  // namespace DeviceSync

#pragma once

#include <cstdint>

#include "SyncPolicy.h"

namespace DeviceSync::SyncPolicyStore {

constexpr const char* DIRECTORY_PATH = "/.crosspoint/device-sync";
constexpr const char* POLICY_PATH = "/.crosspoint/device-sync/config.bin";
constexpr const char* POLICY_TEMP_PATH = "/.crosspoint/device-sync/config.bin.tmp";
constexpr const char* POLICY_BACKUP_PATH = "/.crosspoint/device-sync/config.bin.bak";

enum class LoadResult : uint8_t {
  Loaded,
  Missing,
  Invalid,
  Unsupported,
};

// Missing configuration loads safe defaults without writing to the SD card.
// Invalid or unsupported configuration leaves policy disabled.
LoadResult load(SyncPolicy& policy);
bool save(const SyncPolicy& policy);

}  // namespace DeviceSync::SyncPolicyStore

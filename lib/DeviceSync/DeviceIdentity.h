#pragma once

#include <cstddef>
#include <cstdint>

#include "DeviceSyncTypes.h"

namespace DeviceSync::DeviceIdentity {

constexpr size_t FACTORY_MAC_SIZE = 6;
constexpr size_t SALT_SIZE = 32;
constexpr size_t SHA256_SIZE = 32;

constexpr const char* DIRECTORY_PATH = "/.crosspoint/device-sync";
constexpr const char* IDENTITY_PATH = "/.crosspoint/device-sync/identity.bin";
constexpr const char* IDENTITY_TEMP_PATH = "/.crosspoint/device-sync/identity.bin.tmp";
constexpr const char* IDENTITY_BACKUP_PATH = "/.crosspoint/device-sync/identity.bin.bak";

struct Platform {
  void* context = nullptr;
  bool (*readFactoryMac)(void* context, uint8_t output[FACTORY_MAC_SIZE]) = nullptr;
  bool (*fillRandom)(void* context, void* output, size_t length) = nullptr;
  bool (*sha256)(void* context, const void* data, size_t length, uint8_t output[SHA256_SIZE]) = nullptr;
};

// Hardware uses the factory eFuse MAC, ESP RNG, and mbedTLS SHA-256. The RNG
// callback is intended for this non-secret identity salt, not long-term keys.
Platform systemPlatform();

enum class LoadResult : uint8_t {
  Loaded,
  Created,
  Invalid,
  Unsupported,
  PlatformError,
  StorageError,
};

// Loads or creates the device-local salt, then derives a DeviceId from the
// factory MAC without persisting or exposing the raw MAC.
LoadResult loadOrCreate(DeviceId& deviceId, const Platform& platform);

}  // namespace DeviceSync::DeviceIdentity

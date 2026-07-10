#include "DeviceIdentity.h"

#include <AtomicFile.h>
#include <HalStorage.h>
#include <Logging.h>

#include <array>
#include <cstring>

#include "Crc32.h"

namespace DeviceSync::DeviceIdentity {
namespace {
constexpr const char* LOG_MODULE = "DEVICE_ID";
constexpr uint16_t FORMAT_VERSION = 1;
constexpr size_t ENCODED_SIZE = 46;
constexpr uint8_t MAGIC[] = {'D', 'S', 'I', 'D'};
constexpr char DOMAIN_SEPARATOR[] = "CrossInk Device Sync DeviceId v1";
constexpr AtomicFile::Paths IDENTITY_PATHS = {IDENTITY_PATH, IDENTITY_TEMP_PATH, IDENTITY_BACKUP_PATH};

bool readExact(HalFile& file, void* data, const size_t length, Crc32& crc) {
  if (file.read(data, length) != static_cast<int>(length)) return false;
  crc.update(data, length);
  return true;
}

bool readU16(HalFile& file, uint16_t& value, Crc32& crc) {
  uint8_t data[2];
  if (!readExact(file, data, sizeof(data), crc)) return false;
  value = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
  return true;
}

bool readU32(HalFile& file, uint32_t& value, Crc32& crc) {
  uint8_t data[4];
  if (!readExact(file, data, sizeof(data), crc)) return false;
  value = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
          (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return true;
}

AtomicFile::ValidationResult readIdentity(const char* path, uint8_t* salt) {
  HalFile file;
  if (!Storage.openFileForRead(LOG_MODULE, path, file)) {
    LOG_ERR(LOG_MODULE, "Could not open device identity: %s", path);
    return AtomicFile::ValidationResult::Invalid;
  }
  const uint64_t fileSize = file.fileSize64();
  Crc32 crc;
  uint8_t magic[sizeof(MAGIC)];
  uint16_t version = 0;
  uint32_t declaredLength = 0;
  std::array<uint8_t, SALT_SIZE> scratchSalt{};
  uint8_t* saltOutput = salt != nullptr ? salt : scratchSalt.data();
  bool valid = fileSize >= sizeof(MAGIC) + sizeof(version) && readExact(file, magic, sizeof(magic), crc) &&
               std::memcmp(magic, MAGIC, sizeof(MAGIC)) == 0 && readU16(file, version, crc);
  if (valid && version > FORMAT_VERSION) {
    file.close();
    return AtomicFile::ValidationResult::Unsupported;
  }
  valid = valid && version == FORMAT_VERSION && fileSize == ENCODED_SIZE && readU32(file, declaredLength, crc) &&
          declaredLength == ENCODED_SIZE && readExact(file, saltOutput, SALT_SIZE, crc);

  uint8_t encodedCrc[4];
  if (valid && file.read(encodedCrc, sizeof(encodedCrc)) == static_cast<int>(sizeof(encodedCrc))) {
    const uint32_t expectedCrc = static_cast<uint32_t>(encodedCrc[0]) | (static_cast<uint32_t>(encodedCrc[1]) << 8) |
                                 (static_cast<uint32_t>(encodedCrc[2]) << 16) |
                                 (static_cast<uint32_t>(encodedCrc[3]) << 24);
    valid = expectedCrc == crc.value();
  } else {
    valid = false;
  }
  if (!file.close()) valid = false;
  return valid ? AtomicFile::ValidationResult::Valid : AtomicFile::ValidationResult::Invalid;
}

bool writeChecked(HalFile& file, const void* data, const size_t length, Crc32& crc) {
  if (file.write(data, length) != length) return false;
  crc.update(data, length);
  return true;
}

bool writeIdentity(HalFile& file, const void* context) {
  if (context == nullptr) return false;
  const auto* salt = static_cast<const uint8_t*>(context);
  const uint8_t version[] = {static_cast<uint8_t>(FORMAT_VERSION), static_cast<uint8_t>(FORMAT_VERSION >> 8)};
  const uint8_t length[] = {static_cast<uint8_t>(ENCODED_SIZE), static_cast<uint8_t>(ENCODED_SIZE >> 8),
                            static_cast<uint8_t>(ENCODED_SIZE >> 16), static_cast<uint8_t>(ENCODED_SIZE >> 24)};
  Crc32 crc;
  if (!writeChecked(file, MAGIC, sizeof(MAGIC), crc) || !writeChecked(file, version, sizeof(version), crc) ||
      !writeChecked(file, length, sizeof(length), crc) || !writeChecked(file, salt, SALT_SIZE, crc)) {
    return false;
  }
  const uint32_t value = crc.value();
  const uint8_t encodedCrc[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                                static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  return file.write(encodedCrc, sizeof(encodedCrc)) == sizeof(encodedCrc);
}

AtomicFile::ValidationResult validateIdentity(const char* path, const void*) { return readIdentity(path, nullptr); }

bool hasUnsupportedCandidate() {
  static constexpr const char* CANDIDATES[] = {IDENTITY_PATH, IDENTITY_TEMP_PATH, IDENTITY_BACKUP_PATH};
  for (const char* path : CANDIDATES) {
    if (Storage.exists(path) && readIdentity(path, nullptr) == AtomicFile::ValidationResult::Unsupported) return true;
  }
  return false;
}

bool isAllZero(const uint8_t* data, const size_t length) {
  uint8_t combined = 0;
  for (size_t i = 0; i < length; ++i) combined |= data[i];
  return combined == 0;
}

__attribute__((noinline)) bool deriveDeviceId(DeviceId& deviceId, const uint8_t salt[SALT_SIZE],
                                              const Platform& platform) {
  uint8_t factoryMac[FACTORY_MAC_SIZE];
  if (!platform.readFactoryMac(platform.context, factoryMac) || isAllZero(factoryMac, sizeof(factoryMac))) return false;

  uint8_t material[sizeof(DOMAIN_SEPARATOR) - 1 + FACTORY_MAC_SIZE + SALT_SIZE];
  size_t offset = 0;
  std::memcpy(material + offset, DOMAIN_SEPARATOR, sizeof(DOMAIN_SEPARATOR) - 1);
  offset += sizeof(DOMAIN_SEPARATOR) - 1;
  std::memcpy(material + offset, factoryMac, sizeof(factoryMac));
  offset += sizeof(factoryMac);
  std::memcpy(material + offset, salt, SALT_SIZE);

  uint8_t digest[SHA256_SIZE];
  if (!platform.sha256(platform.context, material, sizeof(material), digest) || isAllZero(digest, sizeof(digest))) {
    return false;
  }
  std::memcpy(deviceId.data(), digest, deviceId.size());
  return true;
}

}  // namespace

LoadResult loadOrCreate(DeviceId& deviceId, const Platform& platform) {
  deviceId.fill(0);
  if (platform.readFactoryMac == nullptr || platform.fillRandom == nullptr || platform.sha256 == nullptr) {
    return LoadResult::PlatformError;
  }

  bool hasCandidate =
      Storage.exists(IDENTITY_PATH) || Storage.exists(IDENTITY_TEMP_PATH) || Storage.exists(IDENTITY_BACKUP_PATH);
  if (!AtomicFile::recover(LOG_MODULE, IDENTITY_PATHS, validateIdentity)) {
    if (hasUnsupportedCandidate()) return LoadResult::Unsupported;
    // An interrupted first-ever write can leave only an invalid temp. No
    // identity was committed, so regenerating is safe; backups are never discarded.
    if (!Storage.exists(IDENTITY_PATH) && Storage.exists(IDENTITY_TEMP_PATH) && !Storage.exists(IDENTITY_BACKUP_PATH) &&
        readIdentity(IDENTITY_TEMP_PATH, nullptr) == AtomicFile::ValidationResult::Invalid) {
      if (!Storage.remove(IDENTITY_TEMP_PATH)) return LoadResult::StorageError;
      hasCandidate = false;
    } else {
      return LoadResult::Invalid;
    }
  }

  std::array<uint8_t, SALT_SIZE> salt{};
  if (Storage.exists(IDENTITY_PATH)) {
    const AtomicFile::ValidationResult result = readIdentity(IDENTITY_PATH, salt.data());
    if (result == AtomicFile::ValidationResult::Unsupported) return LoadResult::Unsupported;
    if (result != AtomicFile::ValidationResult::Valid) return LoadResult::Invalid;
    if (!deriveDeviceId(deviceId, salt.data(), platform)) {
      deviceId.fill(0);
      return LoadResult::PlatformError;
    }
    return LoadResult::Loaded;
  }

  if (hasCandidate) return LoadResult::Invalid;
  if (!platform.fillRandom(platform.context, salt.data(), salt.size()) || isAllZero(salt.data(), salt.size()) ||
      !deriveDeviceId(deviceId, salt.data(), platform)) {
    deviceId.fill(0);
    return LoadResult::PlatformError;
  }
  if (!Storage.ensureDirectoryExists(DIRECTORY_PATH) ||
      !AtomicFile::write(LOG_MODULE, IDENTITY_PATHS, writeIdentity, validateIdentity, salt.data())) {
    deviceId.fill(0);
    return LoadResult::StorageError;
  }
  return LoadResult::Created;
}

}  // namespace DeviceSync::DeviceIdentity

#include "DeviceIdentity.h"

#ifdef SIMULATOR

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <climits>
#include <cstring>

#else

#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>

#endif

namespace DeviceSync::DeviceIdentity {
namespace {

#ifdef SIMULATOR

bool readSystemFactoryMac(void*, uint8_t output[FACTORY_MAC_SIZE]) {
  static constexpr uint8_t SIMULATOR_MAC[FACTORY_MAC_SIZE] = {0x02, 0x43, 0x49, 0x4E, 0x4B, 0x01};
  std::memcpy(output, SIMULATOR_MAC, sizeof(SIMULATOR_MAC));
  return true;
}

bool fillSystemRandom(void*, void* output, const size_t length) {
  return length <= static_cast<size_t>(INT_MAX) &&
         RAND_bytes(static_cast<unsigned char*>(output), static_cast<int>(length)) == 1;
}

bool systemSha256(void*, const void* data, const size_t length, uint8_t output[SHA256_SIZE]) {
  return SHA256(static_cast<const unsigned char*>(data), length, output) != nullptr;
}

#else

bool readSystemFactoryMac(void*, uint8_t output[FACTORY_MAC_SIZE]) {
  return esp_read_mac(output, ESP_MAC_EFUSE_FACTORY) == ESP_OK;
}

bool fillSystemRandom(void*, void* output, const size_t length) {
  esp_fill_random(output, length);
  return true;
}

bool systemSha256(void*, const void* data, const size_t length, uint8_t output[SHA256_SIZE]) {
  return mbedtls_sha256(static_cast<const unsigned char*>(data), length, output, 0) == 0;
}

#endif

}  // namespace

Platform systemPlatform() { return Platform{nullptr, readSystemFactoryMac, fillSystemRandom, systemSha256}; }

}  // namespace DeviceSync::DeviceIdentity

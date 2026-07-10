#include "PairHandshake.h"

#ifdef SIMULATOR

#include <openssl/rand.h>

#include <climits>

#else

#include <WiFi.h>
#include <esp_random.h>

#endif

namespace DeviceSync::PairHandshake {
namespace {

#ifdef SIMULATOR

bool fillSystemStrongRandom(void*, void* output, const size_t length) {
  return output != nullptr && length != 0 && length <= static_cast<size_t>(INT_MAX) &&
         RAND_priv_bytes(static_cast<unsigned char*>(output), static_cast<int>(length)) == 1;
}

#else

bool fillSystemStrongRandom(void*, void* output, const size_t length) {
  if (output == nullptr || length == 0) return false;
  // WiFi.getMode() returns WIFI_MODE_NULL when the low-level driver is stopped.
  // Active STA/AP mode continuously feeds RF entropy into the ESP hardware RNG.
  if (WiFi.getMode() == WIFI_MODE_NULL) return false;
  esp_fill_random(output, length);
  return true;
}

#endif

}  // namespace

StrongRandomSource systemStrongRandomSource() { return StrongRandomSource{nullptr, fillSystemStrongRandom}; }

}  // namespace DeviceSync::PairHandshake

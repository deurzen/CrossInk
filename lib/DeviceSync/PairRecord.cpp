#include "PairRecord.h"

#include <cstring>

namespace DeviceSync {
namespace {

bool allZero(const uint8_t* data, const size_t length) {
  uint8_t combined = 0;
  for (size_t i = 0; i < length; ++i) combined |= data[i];
  return combined == 0;
}

bool validUtf8DisplayName(const char* name) {
  if (name == nullptr) return false;
  const size_t length = strnlen(name, PAIR_DISPLAY_NAME_BYTES);
  if (length == 0 || length >= PAIR_DISPLAY_NAME_BYTES) return false;

  size_t i = 0;
  while (i < length) {
    const uint8_t first = static_cast<uint8_t>(name[i]);
    if (first < 0x20 || first == 0x7F) return false;
    if (first < 0x80) {
      ++i;
      continue;
    }

    size_t continuationCount = 0;
    uint32_t codePoint = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      continuationCount = 1;
      codePoint = first & 0x1F;
    } else if (first >= 0xE0 && first <= 0xEF) {
      continuationCount = 2;
      codePoint = first & 0x0F;
    } else if (first >= 0xF0 && first <= 0xF4) {
      continuationCount = 3;
      codePoint = first & 0x07;
    } else {
      return false;
    }
    if (continuationCount > length - i - 1) return false;
    for (size_t j = 1; j <= continuationCount; ++j) {
      const uint8_t continuation = static_cast<uint8_t>(name[i + j]);
      if ((continuation & 0xC0) != 0x80) return false;
      codePoint = (codePoint << 6) | (continuation & 0x3F);
    }
    if ((continuationCount == 2 && codePoint < 0x800) || (continuationCount == 3 && codePoint < 0x10000) ||
        (codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF) {
      return false;
    }
    i += continuationCount + 1;
  }
  return true;
}

}  // namespace

void PairRecord::reset() {
  peerDeviceId.fill(0);
  peerDisplayName[0] = '\0';
  pairSecret.fill(0);
  firstPairedLocalGeneration = 0;
  lastSuccessfulSessionId.fill(0);
  hasLastSuccessfulSession = false;
  capabilitySnapshot = 0;
  lastObservedPolicyDigest.fill(0);
  nextLocalHandshakeCounter = 1;
  lastAcceptedPeerHandshakeCounter = 0;
}

bool PairRecord::setPeerDisplayName(const char* name) {
  if (!validUtf8DisplayName(name)) return false;
  const size_t length = std::strlen(name);
  std::memcpy(peerDisplayName, name, length + 1);
  return true;
}

bool PairRecord::valid() const {
  return !allZero(peerDeviceId.data(), peerDeviceId.size()) && validUtf8DisplayName(peerDisplayName) &&
         !allZero(pairSecret.data(), pairSecret.size()) && firstPairedLocalGeneration != 0 &&
         nextLocalHandshakeCounter != 0 &&
         (hasLastSuccessfulSession != allZero(lastSuccessfulSessionId.data(), lastSuccessfulSessionId.size()));
}

}  // namespace DeviceSync

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "DeviceSyncTypes.h"

namespace DeviceSync {

constexpr size_t PAIR_DISPLAY_NAME_BYTES = 29;
constexpr size_t PAIR_SECRET_SIZE = 32;
constexpr size_t POLICY_DIGEST_SIZE = 32;

struct PairRecord {
  DeviceId peerDeviceId{};
  std::array<uint8_t, PAIR_SECRET_SIZE> pairSecret{};
  SessionId lastSuccessfulSessionId{};
  std::array<uint8_t, POLICY_DIGEST_SIZE> lastObservedPolicyDigest{};
  uint64_t firstPairedLocalGeneration = 0;
  uint64_t capabilitySnapshot = 0;
  uint64_t nextLocalHandshakeCounter = 1;
  uint64_t lastAcceptedPeerHandshakeCounter = 0;
  char peerDisplayName[PAIR_DISPLAY_NAME_BYTES] = {};
  bool hasLastSuccessfulSession = false;

  ~PairRecord();
  void reset();
  bool setPeerDisplayName(const char* name);
  bool valid() const;
};

}  // namespace DeviceSync

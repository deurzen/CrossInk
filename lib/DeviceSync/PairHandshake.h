#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "DeviceSyncTypes.h"
#include "PairRecord.h"
#include "PairRecordStore.h"

namespace DeviceSync::PairHandshake {

constexpr size_t NONCE_SIZE = 32;
using Nonce = std::array<uint8_t, NONCE_SIZE>;

// This callback must fail unless backed by an active entropy source. It is a
// distinct type from DeviceIdentity::Platform so boot-time salt RNG cannot be
// accidentally reused for key material.
struct StrongRandomSource {
  void* context = nullptr;
  bool (*fill)(void* context, void* output, size_t length) = nullptr;
};

StrongRandomSource systemStrongRandomSource();
bool generateNonce(Nonce& nonce, const StrongRandomSource& source);

enum class TransportRole : uint8_t {
  Invalid,
  Coordinator,
  Joiner,
};

TransportRole roleFor(const DeviceId& localDeviceId, const DeviceId& peerDeviceId);

enum class LocalCounterResult : uint8_t {
  Reserved,
  InvalidRecord,
  Exhausted,
  StorageError,
};

// Persists the increment before returning the reserved counter. A crash can
// skip a value but cannot return a value that was not durably reserved.
LocalCounterResult reserveLocalCounter(PairRecord& record, PairRecordStore& store, uint64_t& reservedCounter);

enum class PeerCounterResult : uint8_t {
  Accepted,
  Duplicate,
  Replay,
  InvalidRecord,
  StorageError,
};

// Persists a newly accepted counter before reporting success. Duplicate lets a
// caller resend an idempotent response without applying the handshake twice.
PeerCounterResult acceptPeerCounter(PairRecord& record, PairRecordStore& store, uint64_t receivedCounter);

}  // namespace DeviceSync::PairHandshake

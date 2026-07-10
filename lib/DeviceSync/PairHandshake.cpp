#include "PairHandshake.h"

#include <limits>

namespace DeviceSync::PairHandshake {
namespace {

bool allZero(const uint8_t* data, const size_t length) {
  uint8_t combined = 0;
  for (size_t i = 0; i < length; ++i) combined |= data[i];
  return combined == 0;
}

int compareIds(const DeviceId& left, const DeviceId& right) {
  for (size_t i = 0; i < left.size(); ++i) {
    if (left[i] < right[i]) return -1;
    if (left[i] > right[i]) return 1;
  }
  return 0;
}

}  // namespace

bool generateNonce(Nonce& nonce, const StrongRandomSource& source) {
  nonce.fill(0);
  if (source.fill == nullptr || !source.fill(source.context, nonce.data(), nonce.size()) ||
      allZero(nonce.data(), nonce.size())) {
    nonce.fill(0);
    return false;
  }
  return true;
}

TransportRole roleFor(const DeviceId& localDeviceId, const DeviceId& peerDeviceId) {
  const int comparison = compareIds(localDeviceId, peerDeviceId);
  if (comparison == 0 || allZero(localDeviceId.data(), localDeviceId.size()) ||
      allZero(peerDeviceId.data(), peerDeviceId.size())) {
    return TransportRole::Invalid;
  }
  return comparison < 0 ? TransportRole::Coordinator : TransportRole::Joiner;
}

LocalCounterResult reserveLocalCounter(PairRecord& record, PairRecordStore& store, uint64_t& reservedCounter) {
  reservedCounter = 0;
  if (!record.valid()) return LocalCounterResult::InvalidRecord;
  if (record.nextLocalHandshakeCounter == std::numeric_limits<uint64_t>::max()) {
    return LocalCounterResult::Exhausted;
  }

  const uint64_t counter = record.nextLocalHandshakeCounter;
  record.nextLocalHandshakeCounter = counter + 1;
  if (!store.save(record)) {
    record.nextLocalHandshakeCounter = counter;
    return LocalCounterResult::StorageError;
  }
  reservedCounter = counter;
  return LocalCounterResult::Reserved;
}

PeerCounterResult acceptPeerCounter(PairRecord& record, PairRecordStore& store, const uint64_t receivedCounter) {
  if (!record.valid() || receivedCounter == 0) return PeerCounterResult::InvalidRecord;
  if (receivedCounter == record.lastAcceptedPeerHandshakeCounter) return PeerCounterResult::Duplicate;
  if (receivedCounter < record.lastAcceptedPeerHandshakeCounter) return PeerCounterResult::Replay;

  const uint64_t previousCounter = record.lastAcceptedPeerHandshakeCounter;
  record.lastAcceptedPeerHandshakeCounter = receivedCounter;
  if (!store.save(record)) {
    record.lastAcceptedPeerHandshakeCounter = previousCounter;
    return PeerCounterResult::StorageError;
  }
  return PeerCounterResult::Accepted;
}

}  // namespace DeviceSync::PairHandshake

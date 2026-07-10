#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "DeviceSyncTypes.h"
#include "PairHandshake.h"
#include "PairRecord.h"

namespace DeviceSync::PairingCrypto {

constexpr size_t X25519_KEY_SIZE = 32;
constexpr size_t TRANSCRIPT_HASH_SIZE = 32;
constexpr size_t CONFIRMATION_KEY_SIZE = 32;
constexpr size_t SESSION_KEY_SIZE = 32;

using TranscriptHash = std::array<uint8_t, TRANSCRIPT_HASH_SIZE>;

struct EphemeralKeyPair {
  std::array<uint8_t, X25519_KEY_SIZE> privateKey{};
  std::array<uint8_t, X25519_KEY_SIZE> publicKey{};

  EphemeralKeyPair() = default;
  EphemeralKeyPair(const EphemeralKeyPair&) = delete;
  EphemeralKeyPair& operator=(const EphemeralKeyPair&) = delete;
  ~EphemeralKeyPair();
  void clear();
};

struct SharedSecret {
  std::array<uint8_t, X25519_KEY_SIZE> bytes{};

  SharedSecret() = default;
  SharedSecret(const SharedSecret&) = delete;
  SharedSecret& operator=(const SharedSecret&) = delete;
  ~SharedSecret();
  void clear();
};

struct DerivedSecrets {
  std::array<uint8_t, PAIR_SECRET_SIZE> pairSecret{};
  std::array<uint8_t, CONFIRMATION_KEY_SIZE> confirmationKey{};
  std::array<uint8_t, SESSION_KEY_SIZE> coordinatorToJoinerKey{};
  std::array<uint8_t, SESSION_KEY_SIZE> joinerToCoordinatorKey{};
  uint32_t shortAuthenticationValue = 0;

  DerivedSecrets() = default;
  DerivedSecrets(const DerivedSecrets&) = delete;
  DerivedSecrets& operator=(const DerivedSecrets&) = delete;
  ~DerivedSecrets();
  void clear();
};

bool generateEphemeralKeyPair(EphemeralKeyPair& keyPair, const PairHandshake::StrongRandomSource& randomSource);
bool deriveSharedSecret(const EphemeralKeyPair& localKeyPair, const std::array<uint8_t, X25519_KEY_SIZE>& peerPublicKey,
                        const PairHandshake::StrongRandomSource& randomSource, SharedSecret& sharedSecret);
bool derivePairingSecrets(const SharedSecret& sharedSecret, const TranscriptHash& transcriptHash,
                          DerivedSecrets& secrets);

}  // namespace DeviceSync::PairingCrypto

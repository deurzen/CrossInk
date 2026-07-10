#include "PairingCrypto.h"

#include <cstring>

#include "PairingCryptoBackend.h"

namespace DeviceSync::PairingCrypto {
namespace {
constexpr char HKDF_INFO[] = "CrossInk Device Sync Pairing v1";
constexpr size_t DERIVED_BYTES = PAIR_SECRET_SIZE + CONFIRMATION_KEY_SIZE + SESSION_KEY_SIZE * 2 + sizeof(uint32_t);

void secureZero(void* data, const size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) bytes[i] = 0;
}

bool allZero(const uint8_t* data, const size_t length) {
  uint8_t combined = 0;
  for (size_t i = 0; i < length; ++i) combined |= data[i];
  return combined == 0;
}

}  // namespace

EphemeralKeyPair::~EphemeralKeyPair() { clear(); }

void EphemeralKeyPair::clear() {
  secureZero(privateKey.data(), privateKey.size());
  publicKey.fill(0);
}

SharedSecret::~SharedSecret() { clear(); }

void SharedSecret::clear() { secureZero(bytes.data(), bytes.size()); }

DerivedSecrets::~DerivedSecrets() { clear(); }

void DerivedSecrets::clear() {
  secureZero(pairSecret.data(), pairSecret.size());
  secureZero(confirmationKey.data(), confirmationKey.size());
  secureZero(coordinatorToJoinerKey.data(), coordinatorToJoinerKey.size());
  secureZero(joinerToCoordinatorKey.data(), joinerToCoordinatorKey.size());
  shortAuthenticationValue = 0;
}

bool generateEphemeralKeyPair(EphemeralKeyPair& keyPair, const PairHandshake::StrongRandomSource& randomSource) {
  keyPair.clear();
  if (randomSource.fill == nullptr ||
      !randomSource.fill(randomSource.context, keyPair.privateKey.data(), keyPair.privateKey.size()) ||
      allZero(keyPair.privateKey.data(), keyPair.privateKey.size())) {
    keyPair.clear();
    return false;
  }

  // RFC 7748 X25519 scalar clamping.
  keyPair.privateKey[0] &= 248;
  keyPair.privateKey[31] &= 127;
  keyPair.privateKey[31] |= 64;
  if (!Backend::x25519Public(keyPair.privateKey.data(), keyPair.publicKey.data(), randomSource) ||
      allZero(keyPair.publicKey.data(), keyPair.publicKey.size())) {
    keyPair.clear();
    return false;
  }
  return true;
}

bool deriveSharedSecret(const EphemeralKeyPair& localKeyPair, const std::array<uint8_t, X25519_KEY_SIZE>& peerPublicKey,
                        const PairHandshake::StrongRandomSource& randomSource, SharedSecret& sharedSecret) {
  sharedSecret.clear();
  uint8_t entropyProbe = 0;
  if (randomSource.fill == nullptr || !randomSource.fill(randomSource.context, &entropyProbe, sizeof(entropyProbe)) ||
      allZero(localKeyPair.privateKey.data(), localKeyPair.privateKey.size()) ||
      allZero(localKeyPair.publicKey.data(), localKeyPair.publicKey.size()) ||
      allZero(peerPublicKey.data(), peerPublicKey.size()) || (peerPublicKey.back() & 0x80) != 0 ||
      !Backend::x25519Shared(localKeyPair.privateKey.data(), peerPublicKey.data(), sharedSecret.bytes.data(),
                             randomSource) ||
      allZero(sharedSecret.bytes.data(), sharedSecret.bytes.size())) {
    entropyProbe = 0;
    sharedSecret.clear();
    return false;
  }
  entropyProbe = 0;
  return true;
}

bool derivePairingSecrets(const SharedSecret& sharedSecret, const TranscriptHash& transcriptHash,
                          DerivedSecrets& secrets) {
  secrets.clear();
  if (allZero(sharedSecret.bytes.data(), sharedSecret.bytes.size()) ||
      allZero(transcriptHash.data(), transcriptHash.size())) {
    return false;
  }

  uint8_t derived[DERIVED_BYTES];
  if (!Backend::hkdfSha256(transcriptHash.data(), transcriptHash.size(), sharedSecret.bytes.data(),
                           sharedSecret.bytes.size(), reinterpret_cast<const uint8_t*>(HKDF_INFO),
                           sizeof(HKDF_INFO) - 1, derived, sizeof(derived))) {
    secureZero(derived, sizeof(derived));
    return false;
  }

  size_t offset = 0;
  std::memcpy(secrets.pairSecret.data(), derived + offset, secrets.pairSecret.size());
  offset += secrets.pairSecret.size();
  std::memcpy(secrets.confirmationKey.data(), derived + offset, secrets.confirmationKey.size());
  offset += secrets.confirmationKey.size();
  std::memcpy(secrets.coordinatorToJoinerKey.data(), derived + offset, secrets.coordinatorToJoinerKey.size());
  offset += secrets.coordinatorToJoinerKey.size();
  std::memcpy(secrets.joinerToCoordinatorKey.data(), derived + offset, secrets.joinerToCoordinatorKey.size());
  offset += secrets.joinerToCoordinatorKey.size();
  const uint32_t sasSeed = static_cast<uint32_t>(derived[offset]) | (static_cast<uint32_t>(derived[offset + 1]) << 8) |
                           (static_cast<uint32_t>(derived[offset + 2]) << 16) |
                           (static_cast<uint32_t>(derived[offset + 3]) << 24);
  secrets.shortAuthenticationValue = sasSeed % 1000000U;
  secureZero(derived, sizeof(derived));
  return true;
}

}  // namespace DeviceSync::PairingCrypto

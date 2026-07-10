#pragma once

#include <cstddef>
#include <cstdint>

#include "PairHandshake.h"

namespace DeviceSync::PairingCrypto::Backend {

bool x25519Public(const uint8_t privateKey[32], uint8_t publicKey[32],
                  const PairHandshake::StrongRandomSource& randomSource);
bool x25519Shared(const uint8_t privateKey[32], const uint8_t peerPublicKey[32], uint8_t sharedSecret[32],
                  const PairHandshake::StrongRandomSource& randomSource);
bool hkdfSha256(const uint8_t* salt, size_t saltLength, const uint8_t* inputKey, size_t inputKeyLength,
                const uint8_t* info, size_t infoLength, uint8_t* output, size_t outputLength);

}  // namespace DeviceSync::PairingCrypto::Backend

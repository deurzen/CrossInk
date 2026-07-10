#include "PairingCryptoBackend.h"

#ifdef SIMULATOR

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <climits>
#include <cstring>

#else

#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

#if !defined(MBEDTLS_ECDH_C) || !defined(MBEDTLS_ECP_DP_CURVE25519_ENABLED) || !defined(MBEDTLS_HKDF_C) || \
    !defined(MBEDTLS_SHA256_C)
#error "Device Sync pairing requires mbedTLS X25519, ECDH, HKDF, and SHA-256"
#endif

#endif

namespace DeviceSync::PairingCrypto::Backend {
namespace {

#ifndef SIMULATOR

int randomAdapter(void* context, unsigned char* output, const size_t length) {
  const auto* source = static_cast<const PairHandshake::StrongRandomSource*>(context);
  return source != nullptr && source->fill != nullptr && source->fill(source->context, output, length) ? 0 : -1;
}

#endif

}  // namespace

#ifdef SIMULATOR

bool x25519Public(const uint8_t privateKey[32], uint8_t publicKey[32], const PairHandshake::StrongRandomSource&) {
  EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, privateKey, 32);
  if (key == nullptr) return false;
  size_t outputLength = 32;
  const bool ok = EVP_PKEY_get_raw_public_key(key, publicKey, &outputLength) == 1 && outputLength == 32;
  EVP_PKEY_free(key);
  return ok;
}

bool x25519Shared(const uint8_t privateKey[32], const uint8_t peerPublicKey[32], uint8_t sharedSecret[32],
                  const PairHandshake::StrongRandomSource&) {
  EVP_PKEY* local = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, privateKey, 32);
  EVP_PKEY* peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peerPublicKey, 32);
  EVP_PKEY_CTX* context = local != nullptr ? EVP_PKEY_CTX_new(local, nullptr) : nullptr;
  size_t outputLength = 32;
  const bool ok = local != nullptr && peer != nullptr && context != nullptr && EVP_PKEY_derive_init(context) == 1 &&
                  EVP_PKEY_derive_set_peer(context, peer) == 1 &&
                  EVP_PKEY_derive(context, sharedSecret, &outputLength) == 1 && outputLength == 32;
  EVP_PKEY_CTX_free(context);
  EVP_PKEY_free(peer);
  EVP_PKEY_free(local);
  return ok;
}

bool hkdfSha256(const uint8_t* salt, const size_t saltLength, const uint8_t* inputKey, const size_t inputKeyLength,
                const uint8_t* info, const size_t infoLength, uint8_t* output, const size_t outputLength) {
  if (saltLength > INT_MAX || inputKeyLength > INT_MAX || infoLength > 32 || outputLength > 255 * 32) return false;

  uint8_t pseudoRandomKey[32] = {};
  unsigned int hashLength = 0;
  if (HMAC(EVP_sha256(), salt, static_cast<int>(saltLength), inputKey, inputKeyLength, pseudoRandomKey, &hashLength) ==
          nullptr ||
      hashLength != sizeof(pseudoRandomKey)) {
    OPENSSL_cleanse(pseudoRandomKey, sizeof(pseudoRandomKey));
    return false;
  }

  uint8_t previous[32] = {};
  size_t previousLength = 0;
  size_t written = 0;
  uint8_t counter = 1;
  while (written < outputLength) {
    uint8_t blockInput[32 + 32 + 1];
    std::memcpy(blockInput, previous, previousLength);
    std::memcpy(blockInput + previousLength, info, infoLength);
    blockInput[previousLength + infoLength] = counter;
    if (HMAC(EVP_sha256(), pseudoRandomKey, sizeof(pseudoRandomKey), blockInput, previousLength + infoLength + 1,
             previous, &hashLength) == nullptr ||
        hashLength != sizeof(previous)) {
      OPENSSL_cleanse(blockInput, sizeof(blockInput));
      OPENSSL_cleanse(previous, sizeof(previous));
      OPENSSL_cleanse(pseudoRandomKey, sizeof(pseudoRandomKey));
      return false;
    }
    OPENSSL_cleanse(blockInput, sizeof(blockInput));
    const size_t copyLength = std::min(sizeof(previous), outputLength - written);
    std::memcpy(output + written, previous, copyLength);
    written += copyLength;
    previousLength = sizeof(previous);
    ++counter;
  }

  OPENSSL_cleanse(pseudoRandomKey, sizeof(pseudoRandomKey));
  OPENSSL_cleanse(previous, sizeof(previous));
  return true;
}

#else

bool x25519Public(const uint8_t privateKey[32], uint8_t publicKey[32],
                  const PairHandshake::StrongRandomSource& randomSource) {
  mbedtls_ecp_group group;
  mbedtls_mpi privateScalar;
  mbedtls_ecp_point publicPoint;
  mbedtls_ecp_group_init(&group);
  mbedtls_mpi_init(&privateScalar);
  mbedtls_ecp_point_init(&publicPoint);

  const bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
                  mbedtls_mpi_read_binary_le(&privateScalar, privateKey, 32) == 0 &&
                  mbedtls_ecp_mul(&group, &publicPoint, &privateScalar, &group.G, randomAdapter,
                                  const_cast<PairHandshake::StrongRandomSource*>(&randomSource)) == 0 &&
                  mbedtls_mpi_write_binary_le(&publicPoint.MBEDTLS_PRIVATE(X), publicKey, 32) == 0;

  mbedtls_ecp_point_free(&publicPoint);
  mbedtls_mpi_free(&privateScalar);
  mbedtls_ecp_group_free(&group);
  return ok;
}

bool x25519Shared(const uint8_t privateKey[32], const uint8_t peerPublicKey[32], uint8_t sharedSecret[32],
                  const PairHandshake::StrongRandomSource& randomSource) {
  mbedtls_ecp_group group;
  mbedtls_mpi privateScalar;
  mbedtls_mpi shared;
  mbedtls_ecp_point peerPoint;
  mbedtls_ecp_group_init(&group);
  mbedtls_mpi_init(&privateScalar);
  mbedtls_mpi_init(&shared);
  mbedtls_ecp_point_init(&peerPoint);

  const bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
                  mbedtls_mpi_read_binary_le(&privateScalar, privateKey, 32) == 0 &&
                  mbedtls_mpi_read_binary_le(&peerPoint.MBEDTLS_PRIVATE(X), peerPublicKey, 32) == 0 &&
                  mbedtls_mpi_lset(&peerPoint.MBEDTLS_PRIVATE(Z), 1) == 0 &&
                  mbedtls_ecp_check_pubkey(&group, &peerPoint) == 0 &&
                  mbedtls_ecdh_compute_shared(&group, &shared, &peerPoint, &privateScalar, randomAdapter,
                                              const_cast<PairHandshake::StrongRandomSource*>(&randomSource)) == 0 &&
                  mbedtls_mpi_write_binary_le(&shared, sharedSecret, 32) == 0;

  mbedtls_ecp_point_free(&peerPoint);
  mbedtls_mpi_free(&shared);
  mbedtls_mpi_free(&privateScalar);
  mbedtls_ecp_group_free(&group);
  return ok;
}

bool hkdfSha256(const uint8_t* salt, const size_t saltLength, const uint8_t* inputKey, const size_t inputKeyLength,
                const uint8_t* info, const size_t infoLength, uint8_t* output, const size_t outputLength) {
  const mbedtls_md_info_t* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return sha256 != nullptr &&
         mbedtls_hkdf(sha256, salt, saltLength, inputKey, inputKeyLength, info, infoLength, output, outputLength) == 0;
}

#endif

}  // namespace DeviceSync::PairingCrypto::Backend

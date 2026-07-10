#include <PairingCrypto.h>
#include <PairingCryptoBackend.h>
#include <gtest/gtest.h>

#include <array>
#include <cstring>

namespace {
using namespace DeviceSync;

struct FixedRandom {
  std::array<uint8_t, PairingCrypto::X25519_KEY_SIZE> bytes{};
  bool fail = false;
};

bool fillFixed(void* context, void* output, const size_t length) {
  auto* random = static_cast<FixedRandom*>(context);
  if (random->fail) return false;
  auto* bytes = static_cast<uint8_t*>(output);
  for (size_t i = 0; i < length; ++i) bytes[i] = random->bytes[i % random->bytes.size()];
  return true;
}

uint8_t hexNibble(const char value) {
  if (value >= '0' && value <= '9') return static_cast<uint8_t>(value - '0');
  return static_cast<uint8_t>(value - 'a' + 10);
}

template <size_t N>
std::array<uint8_t, N> fromHex(const char* hex) {
  std::array<uint8_t, N> output{};
  for (size_t i = 0; i < N; ++i)
    output[i] = static_cast<uint8_t>((hexNibble(hex[i * 2]) << 4) | hexNibble(hex[i * 2 + 1]));
  return output;
}

PairHandshake::StrongRandomSource source(FixedRandom& random) {
  return PairHandshake::StrongRandomSource{&random, fillFixed};
}

TEST(PairingCryptoTest, MatchesRfc7748X25519Vector) {
  FixedRandom aliceRandom{fromHex<32>("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")};
  FixedRandom bobRandom{fromHex<32>("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")};
  const auto expectedAlicePublic = fromHex<32>("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
  const auto expectedBobPublic = fromHex<32>("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
  const auto expectedShared = fromHex<32>("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");

  PairingCrypto::EphemeralKeyPair alice;
  PairingCrypto::EphemeralKeyPair bob;
  ASSERT_TRUE(PairingCrypto::generateEphemeralKeyPair(alice, source(aliceRandom)));
  ASSERT_TRUE(PairingCrypto::generateEphemeralKeyPair(bob, source(bobRandom)));
  EXPECT_EQ(alice.publicKey, expectedAlicePublic);
  EXPECT_EQ(bob.publicKey, expectedBobPublic);

  PairingCrypto::SharedSecret aliceShared;
  PairingCrypto::SharedSecret bobShared;
  ASSERT_TRUE(PairingCrypto::deriveSharedSecret(alice, bob.publicKey, source(aliceRandom), aliceShared));
  ASSERT_TRUE(PairingCrypto::deriveSharedSecret(bob, alice.publicKey, source(bobRandom), bobShared));
  EXPECT_EQ(aliceShared.bytes, expectedShared);
  EXPECT_EQ(bobShared.bytes, expectedShared);
}

TEST(PairingCryptoTest, MatchesRfc5869HkdfSha256Vector) {
  std::array<uint8_t, 22> inputKey{};
  inputKey.fill(0x0B);
  const auto salt = fromHex<13>("000102030405060708090a0b0c");
  const auto info = fromHex<10>("f0f1f2f3f4f5f6f7f8f9");
  const auto expected = fromHex<42>(
      "3cb25f25faacd57a90434f64d0362f2a"
      "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
      "34007208d5b887185865");
  std::array<uint8_t, 42> output{};

  ASSERT_TRUE(PairingCrypto::Backend::hkdfSha256(salt.data(), salt.size(), inputKey.data(), inputKey.size(),
                                                 info.data(), info.size(), output.data(), output.size()));
  EXPECT_EQ(output, expected);
}

TEST(PairingCryptoTest, TranscriptBoundHkdfDerivesSameSecretsAndSixDigitSas) {
  PairingCrypto::SharedSecret firstShared;
  PairingCrypto::SharedSecret secondShared;
  for (size_t i = 0; i < firstShared.bytes.size(); ++i) {
    firstShared.bytes[i] = static_cast<uint8_t>(i + 1);
    secondShared.bytes[i] = static_cast<uint8_t>(i + 1);
  }
  PairingCrypto::TranscriptHash transcript{};
  for (size_t i = 0; i < transcript.size(); ++i) transcript[i] = static_cast<uint8_t>(0xA0 + i);

  PairingCrypto::DerivedSecrets first;
  PairingCrypto::DerivedSecrets second;
  ASSERT_TRUE(PairingCrypto::derivePairingSecrets(firstShared, transcript, first));
  ASSERT_TRUE(PairingCrypto::derivePairingSecrets(secondShared, transcript, second));
  EXPECT_EQ(first.pairSecret, second.pairSecret);
  EXPECT_EQ(first.confirmationKey, second.confirmationKey);
  EXPECT_EQ(first.coordinatorToJoinerKey, second.coordinatorToJoinerKey);
  EXPECT_EQ(first.joinerToCoordinatorKey, second.joinerToCoordinatorKey);
  EXPECT_LT(first.shortAuthenticationValue, 1000000u);

  transcript[0] ^= 0x01;
  PairingCrypto::DerivedSecrets changed;
  ASSERT_TRUE(PairingCrypto::derivePairingSecrets(firstShared, transcript, changed));
  EXPECT_NE(first.pairSecret, changed.pairSecret);
  EXPECT_NE(first.shortAuthenticationValue, changed.shortAuthenticationValue);
}

TEST(PairingCryptoTest, RejectsMissingEntropyAndInvalidPeerKeys) {
  FixedRandom random{};
  random.fail = true;
  PairingCrypto::EphemeralKeyPair keyPair;
  const std::array<uint8_t, 32> zero{};
  EXPECT_FALSE(PairingCrypto::generateEphemeralKeyPair(keyPair, source(random)));
  EXPECT_EQ(keyPair.privateKey, zero);
  EXPECT_EQ(keyPair.publicKey, zero);

  random.fail = false;
  random.bytes.fill(0x55);
  ASSERT_TRUE(PairingCrypto::generateEphemeralKeyPair(keyPair, source(random)));
  PairingCrypto::SharedSecret shared;
  std::array<uint8_t, 32> invalidPeer{};
  EXPECT_FALSE(PairingCrypto::deriveSharedSecret(keyPair, invalidPeer, source(random), shared));
  EXPECT_EQ(shared.bytes, zero);
  invalidPeer.fill(0x01);
  invalidPeer.back() |= 0x80;
  EXPECT_FALSE(PairingCrypto::deriveSharedSecret(keyPair, invalidPeer, source(random), shared));
}

TEST(PairingCryptoTest, RejectsUnsetSharedSecretOrTranscript) {
  PairingCrypto::SharedSecret shared;
  PairingCrypto::TranscriptHash transcript{};
  PairingCrypto::DerivedSecrets secrets;
  const std::array<uint8_t, 32> zero{};
  EXPECT_FALSE(PairingCrypto::derivePairingSecrets(shared, transcript, secrets));
  EXPECT_EQ(secrets.pairSecret, zero);

  shared.bytes.fill(0x42);
  EXPECT_FALSE(PairingCrypto::derivePairingSecrets(shared, transcript, secrets));
}

}  // namespace

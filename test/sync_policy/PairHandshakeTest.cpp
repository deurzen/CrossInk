#include <HalStorage.h>
#include <PairHandshake.h>
#include <gtest/gtest.h>

#include <cstring>
#include <limits>

namespace {
using namespace DeviceSync;

struct FakeEntropy {
  bool fail = false;
  bool zero = false;
  uint8_t seed = 0x40;
};

bool fillEntropy(void* context, void* output, const size_t length) {
  auto* entropy = static_cast<FakeEntropy*>(context);
  if (entropy->fail) return false;
  auto* bytes = static_cast<uint8_t*>(output);
  for (size_t i = 0; i < length; ++i) bytes[i] = entropy->zero ? 0 : static_cast<uint8_t>(entropy->seed + i);
  return true;
}

PairRecord handshakeRecord() {
  PairRecord record;
  for (size_t i = 0; i < record.peerDeviceId.size(); ++i) record.peerDeviceId[i] = static_cast<uint8_t>(0x20 + i);
  EXPECT_TRUE(record.setPeerDisplayName("Peer"));
  for (size_t i = 0; i < record.pairSecret.size(); ++i) record.pairSecret[i] = static_cast<uint8_t>(0x80 + i);
  record.firstPairedLocalGeneration = 3;
  record.nextLocalHandshakeCounter = 10;
  record.lastAcceptedPeerHandshakeCounter = 5;
  return record;
}

class PairHandshakeTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(PairHandshakeTest, GeneratesNonceOnlyFromSuccessfulNonzeroEntropy) {
  FakeEntropy entropy;
  PairHandshake::Nonce nonce{};
  ASSERT_TRUE(PairHandshake::generateNonce(nonce, PairHandshake::StrongRandomSource{&entropy, fillEntropy}));
  EXPECT_EQ(nonce.front(), 0x40);

  entropy.zero = true;
  nonce.fill(0xFF);
  EXPECT_FALSE(PairHandshake::generateNonce(nonce, PairHandshake::StrongRandomSource{&entropy, fillEntropy}));
  for (const uint8_t byte : nonce) EXPECT_EQ(byte, 0);

  entropy.zero = false;
  entropy.fail = true;
  nonce.fill(0xFF);
  EXPECT_FALSE(PairHandshake::generateNonce(nonce, PairHandshake::StrongRandomSource{&entropy, fillEntropy}));
  for (const uint8_t byte : nonce) EXPECT_EQ(byte, 0);
}

TEST_F(PairHandshakeTest, SimulatorStrongRandomSourceProducesNonce) {
  PairHandshake::Nonce nonce{};
  EXPECT_TRUE(PairHandshake::generateNonce(nonce, PairHandshake::systemStrongRandomSource()));
}

TEST_F(PairHandshakeTest, LowerDeviceIdDeterministicallyCoordinates) {
  DeviceId lower{};
  DeviceId higher{};
  lower[15] = 1;
  higher[15] = 2;

  EXPECT_EQ(PairHandshake::roleFor(lower, higher), PairHandshake::TransportRole::Coordinator);
  EXPECT_EQ(PairHandshake::roleFor(higher, lower), PairHandshake::TransportRole::Joiner);
  EXPECT_EQ(PairHandshake::roleFor(lower, lower), PairHandshake::TransportRole::Invalid);
  EXPECT_EQ(PairHandshake::roleFor(DeviceId{}, higher), PairHandshake::TransportRole::Invalid);
}

TEST_F(PairHandshakeTest, ReservesAndPersistsLocalCounterBeforeReturningIt) {
  PairRecordStore store;
  PairRecord record = handshakeRecord();
  uint64_t reserved = 0;

  ASSERT_EQ(PairHandshake::reserveLocalCounter(record, store, reserved), PairHandshake::LocalCounterResult::Reserved);
  EXPECT_EQ(reserved, 10u);
  EXPECT_EQ(record.nextLocalHandshakeCounter, 11u);

  PairRecord loaded;
  ASSERT_EQ(store.load(record.peerDeviceId, loaded), PairRecordStore::LoadResult::Loaded);
  EXPECT_EQ(loaded.nextLocalHandshakeCounter, 11u);
}

TEST_F(PairHandshakeTest, LocalReservationRollsBackOnFailureAndRejectsExhaustion) {
  PairRecordStore store;
  PairRecord record = handshakeRecord();
  Storage.setDirectoryFailure(true);
  uint64_t reserved = 99;

  EXPECT_EQ(PairHandshake::reserveLocalCounter(record, store, reserved),
            PairHandshake::LocalCounterResult::StorageError);
  EXPECT_EQ(reserved, 0u);
  EXPECT_EQ(record.nextLocalHandshakeCounter, 10u);

  Storage.setDirectoryFailure(false);
  record.nextLocalHandshakeCounter = std::numeric_limits<uint64_t>::max();
  EXPECT_EQ(PairHandshake::reserveLocalCounter(record, store, reserved), PairHandshake::LocalCounterResult::Exhausted);
  EXPECT_EQ(record.nextLocalHandshakeCounter, std::numeric_limits<uint64_t>::max());
}

TEST_F(PairHandshakeTest, AcceptsPeerCounterOnceAndClassifiesDuplicatesAndReplays) {
  PairRecordStore store;
  PairRecord record = handshakeRecord();
  ASSERT_TRUE(store.save(record));

  ASSERT_EQ(PairHandshake::acceptPeerCounter(record, store, 8), PairHandshake::PeerCounterResult::Accepted);
  EXPECT_EQ(record.lastAcceptedPeerHandshakeCounter, 8u);
  const size_t mutationsAfterAccept = Storage.mutationCount();
  EXPECT_EQ(PairHandshake::acceptPeerCounter(record, store, 8), PairHandshake::PeerCounterResult::Duplicate);
  EXPECT_EQ(PairHandshake::acceptPeerCounter(record, store, 7), PairHandshake::PeerCounterResult::Replay);
  EXPECT_EQ(Storage.mutationCount(), mutationsAfterAccept);

  PairRecord loaded;
  ASSERT_EQ(store.load(record.peerDeviceId, loaded), PairRecordStore::LoadResult::Loaded);
  EXPECT_EQ(loaded.lastAcceptedPeerHandshakeCounter, 8u);
}

TEST_F(PairHandshakeTest, PeerCounterRollsBackWhenPersistenceFails) {
  PairRecordStore store;
  PairRecord record = handshakeRecord();
  Storage.setDirectoryFailure(true);

  EXPECT_EQ(PairHandshake::acceptPeerCounter(record, store, 6), PairHandshake::PeerCounterResult::StorageError);
  EXPECT_EQ(record.lastAcceptedPeerHandshakeCounter, 5u);
  EXPECT_EQ(PairHandshake::acceptPeerCounter(record, store, 0), PairHandshake::PeerCounterResult::InvalidRecord);
}

}  // namespace

#include <HalStorage.h>
#include <PairRecordStore.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {
using namespace DeviceSync;

PairRecord storedRecord(const uint64_t counter, const char* name = "Peer") {
  PairRecord record;
  for (size_t i = 0; i < record.peerDeviceId.size(); ++i) record.peerDeviceId[i] = static_cast<uint8_t>(i + 1);
  EXPECT_TRUE(record.setPeerDisplayName(name));
  for (size_t i = 0; i < record.pairSecret.size(); ++i) record.pairSecret[i] = static_cast<uint8_t>(0x80 + i);
  record.firstPairedLocalGeneration = 7;
  record.nextLocalHandshakeCounter = counter;
  record.lastAcceptedPeerHandshakeCounter = counter - 1;
  return record;
}

class PairRecordStoreTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(PairRecordStoreTest, MissingPeerReturnsResetRecordAndCanonicalPath) {
  PairRecordStore store;
  PairRecord record = storedRecord(2);
  const DeviceId peerDeviceId = record.peerDeviceId;

  EXPECT_EQ(store.load(peerDeviceId, record), PairRecordStore::LoadResult::Missing);
  EXPECT_FALSE(record.valid());
  EXPECT_STREQ(store.finalPath(), "/.crosspoint/device-sync/peers/0102030405060708090a0b0c0d0e0f10.bin");
}

TEST_F(PairRecordStoreTest, SavesAndLoadsRecordAtomically) {
  PairRecordStore store;
  const PairRecord expected = storedRecord(12, "X3 Reader");
  ASSERT_TRUE(store.save(expected));
  EXPECT_TRUE(Storage.exists(store.finalPath()));

  PairRecord actual;
  ASSERT_EQ(store.load(expected.peerDeviceId, actual), PairRecordStore::LoadResult::Loaded);
  EXPECT_EQ(actual.peerDeviceId, expected.peerDeviceId);
  EXPECT_STREQ(actual.peerDisplayName, expected.peerDisplayName);
  EXPECT_EQ(actual.pairSecret, expected.pairSecret);
  EXPECT_EQ(actual.nextLocalHandshakeCounter, 12u);
  EXPECT_EQ(actual.lastAcceptedPeerHandshakeCounter, 11u);
}

TEST_F(PairRecordStoreTest, RejectsRecordStoredUnderAnotherPeerId) {
  PairRecordStore store;
  const PairRecord first = storedRecord(3);
  ASSERT_TRUE(store.save(first));
  const std::string encoded = *Storage.getFile(store.finalPath());

  DeviceId otherPeer = first.peerDeviceId;
  otherPeer[0] = 0xEE;
  PairRecord ignored;
  ASSERT_EQ(store.load(otherPeer, ignored), PairRecordStore::LoadResult::Missing);
  const std::string otherPath = store.finalPath();
  Storage.setFile(otherPath, encoded);

  PairRecord output = storedRecord(9);
  EXPECT_EQ(store.load(otherPeer, output), PairRecordStore::LoadResult::Invalid);
  EXPECT_FALSE(output.valid());
  EXPECT_EQ(*Storage.getFile(otherPath), encoded);
}

TEST_F(PairRecordStoreTest, PreservesFutureRecordAndBlocksOlderSave) {
  PairRecordStore store;
  const PairRecord original = storedRecord(3);
  ASSERT_TRUE(store.save(original));
  const std::string path = store.finalPath();
  std::string future = *Storage.getFile(path);
  future[4] = 2;
  future[5] = 0;
  future.append(64, '\xA5');
  Storage.reset();
  Storage.setFile(path, future);

  PairRecord output;
  EXPECT_EQ(store.load(original.peerDeviceId, output), PairRecordStore::LoadResult::Unsupported);
  EXPECT_FALSE(output.valid());
  EXPECT_FALSE(store.save(storedRecord(4)));
  EXPECT_EQ(*Storage.getFile(path), future);
}

TEST_F(PairRecordStoreTest, EveryFirstWritePowerCutLoadsCompleteRecordOrReturnsMissing) {
  PairRecordStore store;
  const PairRecord record = storedRecord(10);
  ASSERT_TRUE(store.save(record));
  const size_t mutationCount = Storage.mutationCount();
  ASSERT_GT(mutationCount, 0u);

  for (size_t cut = 1; cut <= mutationCount; ++cut) {
    Storage.reset();
    Storage.cutPowerAfterMutation(cut);
    try {
      store.save(record);
    } catch (const FakePowerLoss&) {
    }
    Storage.disablePowerCut();

    PairRecord recovered;
    const PairRecordStore::LoadResult result = store.load(record.peerDeviceId, recovered);
    EXPECT_TRUE(result == PairRecordStore::LoadResult::Loaded || result == PairRecordStore::LoadResult::Missing) << cut;
    if (result == PairRecordStore::LoadResult::Loaded) {
      EXPECT_EQ(recovered.nextLocalHandshakeCounter, 10u) << cut;
    } else {
      EXPECT_FALSE(recovered.valid()) << cut;
    }
  }
}

TEST_F(PairRecordStoreTest, EveryUpdatePowerCutRecoversOldOrNewRecord) {
  PairRecordStore store;
  const PairRecord oldRecord = storedRecord(10, "Old Peer");
  const PairRecord newRecord = storedRecord(20, "New Peer");
  ASSERT_TRUE(store.save(oldRecord));
  const std::string path = store.finalPath();
  const std::string oldEncoded = *Storage.getFile(path);

  Storage.reset();
  Storage.setFile(path, oldEncoded);
  ASSERT_TRUE(store.save(newRecord));
  const size_t mutationCount = Storage.mutationCount();
  ASSERT_GT(mutationCount, 0u);

  for (size_t cut = 1; cut <= mutationCount; ++cut) {
    Storage.reset();
    Storage.setFile(path, oldEncoded);
    Storage.cutPowerAfterMutation(cut);
    try {
      store.save(newRecord);
    } catch (const FakePowerLoss&) {
    }
    Storage.disablePowerCut();

    PairRecord recovered;
    ASSERT_EQ(store.load(oldRecord.peerDeviceId, recovered), PairRecordStore::LoadResult::Loaded) << cut;
    const bool isOld =
        recovered.nextLocalHandshakeCounter == 10 && std::strcmp(recovered.peerDisplayName, "Old Peer") == 0;
    const bool isNew =
        recovered.nextLocalHandshakeCounter == 20 && std::strcmp(recovered.peerDisplayName, "New Peer") == 0;
    EXPECT_TRUE(isOld || isNew) << cut;
  }
}

TEST_F(PairRecordStoreTest, RemoveClearsRecordAndAtomicSidecars) {
  PairRecordStore store;
  const PairRecord record = storedRecord(5);
  ASSERT_TRUE(store.save(record));
  const std::string finalPath = store.finalPath();
  Storage.setFile(finalPath + ".tmp", "partial");
  Storage.setFile(finalPath + ".bak", *Storage.getFile(finalPath));

  ASSERT_TRUE(store.remove(record.peerDeviceId));
  EXPECT_FALSE(Storage.exists(finalPath.c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((finalPath + ".bak").c_str()));
}

}  // namespace

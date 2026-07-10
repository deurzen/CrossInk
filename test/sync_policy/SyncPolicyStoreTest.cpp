#include <HalStorage.h>
#include <SyncPolicyStore.h>
#include <gtest/gtest.h>

#include <string>

namespace {
using namespace DeviceSync;

class SyncPolicyStoreTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

void expectPolicyMarker(const SyncPolicy& policy, const Direction direction, const bool mirrorDeletions) {
  EXPECT_EQ(policy.direction(Category::BookContent), direction);
  EXPECT_EQ(policy.mirrorDeletions(), mirrorDeletions);
}

SyncPolicy markedPolicy(const Direction direction, const bool mirrorDeletions) {
  SyncPolicy policy;
  policy.setDefaults();
  EXPECT_TRUE(policy.setDirection(Category::BookContent, direction));
  policy.setMirrorDeletions(mirrorDeletions);
  return policy;
}

TEST_F(SyncPolicyStoreTest, MissingPolicyLoadsDefaultsWithoutWriting) {
  SyncPolicy policy;
  ASSERT_EQ(SyncPolicyStore::load(policy), SyncPolicyStore::LoadResult::Missing);

  EXPECT_EQ(policy.direction(Category::BookContent), Direction::Bidirectional);
  EXPECT_FALSE(policy.mirrorDeletions());
  EXPECT_FALSE(Storage.exists(SyncPolicyStore::POLICY_PATH));
}

TEST_F(SyncPolicyStoreTest, SavesAndLoadsPolicyThroughAtomicFile) {
  const SyncPolicy expected = markedPolicy(Direction::SendOnly, true);
  ASSERT_TRUE(SyncPolicyStore::save(expected));

  SyncPolicy actual;
  ASSERT_EQ(SyncPolicyStore::load(actual), SyncPolicyStore::LoadResult::Loaded);
  expectPolicyMarker(actual, Direction::SendOnly, true);
  EXPECT_TRUE(Storage.exists(SyncPolicyStore::POLICY_PATH));
  EXPECT_FALSE(Storage.exists(SyncPolicyStore::POLICY_TEMP_PATH));
}

TEST_F(SyncPolicyStoreTest, InvalidPolicyFailsClosed) {
  Storage.setFile(SyncPolicyStore::POLICY_PATH, "not a policy");
  SyncPolicy policy;
  policy.setDefaults();

  EXPECT_EQ(SyncPolicyStore::load(policy), SyncPolicyStore::LoadResult::Invalid);
  expectPolicyMarker(policy, Direction::Disabled, false);
  EXPECT_EQ(policy.pathRuleCount(), 0u);
}

TEST_F(SyncPolicyStoreTest, PreservesUnsupportedPolicyAndBlocksOlderSave) {
  ASSERT_TRUE(SyncPolicyStore::save(markedPolicy(Direction::SendOnly, false)));
  const std::string* stored = Storage.getFile(SyncPolicyStore::POLICY_PATH);
  ASSERT_NE(stored, nullptr);
  std::string future = *stored;
  ASSERT_GT(future.size(), 5u);
  future[4] = 2;
  future[5] = 0;

  Storage.reset();
  Storage.setFile(SyncPolicyStore::POLICY_PATH, future);
  SyncPolicy policy;
  policy.setDefaults();
  EXPECT_EQ(SyncPolicyStore::load(policy), SyncPolicyStore::LoadResult::Unsupported);
  expectPolicyMarker(policy, Direction::Disabled, false);

  EXPECT_FALSE(SyncPolicyStore::save(markedPolicy(Direction::ReceiveOnly, true)));
  ASSERT_NE(Storage.getFile(SyncPolicyStore::POLICY_PATH), nullptr);
  EXPECT_EQ(*Storage.getFile(SyncPolicyStore::POLICY_PATH), future);
}

TEST_F(SyncPolicyStoreTest, RecoversFirstWriteFromCompleteTemp) {
  const SyncPolicy expected = markedPolicy(Direction::ReceiveOnly, true);
  ASSERT_TRUE(SyncPolicyStore::save(expected));
  const std::string encoded = *Storage.getFile(SyncPolicyStore::POLICY_PATH);

  Storage.reset();
  Storage.setFile(SyncPolicyStore::POLICY_TEMP_PATH, encoded);
  SyncPolicy actual;
  ASSERT_EQ(SyncPolicyStore::load(actual), SyncPolicyStore::LoadResult::Loaded);
  expectPolicyMarker(actual, Direction::ReceiveOnly, true);
  EXPECT_TRUE(Storage.exists(SyncPolicyStore::POLICY_PATH));
  EXPECT_FALSE(Storage.exists(SyncPolicyStore::POLICY_TEMP_PATH));
}

TEST_F(SyncPolicyStoreTest, EverySavePowerCutRecoversOldOrNewPolicy) {
  const SyncPolicy oldPolicy = markedPolicy(Direction::SendOnly, false);
  const SyncPolicy newPolicy = markedPolicy(Direction::ReceiveOnly, true);
  ASSERT_TRUE(SyncPolicyStore::save(oldPolicy));
  const std::string oldEncoded = *Storage.getFile(SyncPolicyStore::POLICY_PATH);

  Storage.reset();
  Storage.setFile(SyncPolicyStore::POLICY_PATH, oldEncoded);
  ASSERT_TRUE(SyncPolicyStore::save(newPolicy));
  const size_t mutationCount = Storage.mutationCount();
  ASSERT_GT(mutationCount, 0u);

  for (size_t cut = 1; cut <= mutationCount; ++cut) {
    Storage.reset();
    Storage.setFile(SyncPolicyStore::POLICY_PATH, oldEncoded);
    Storage.cutPowerAfterMutation(cut);
    try {
      SyncPolicyStore::save(newPolicy);
    } catch (const FakePowerLoss&) {
    }
    Storage.disablePowerCut();

    SyncPolicy recovered;
    ASSERT_EQ(SyncPolicyStore::load(recovered), SyncPolicyStore::LoadResult::Loaded) << cut;
    const bool isOld =
        recovered.direction(Category::BookContent) == Direction::SendOnly && !recovered.mirrorDeletions();
    const bool isNew =
        recovered.direction(Category::BookContent) == Direction::ReceiveOnly && recovered.mirrorDeletions();
    EXPECT_TRUE(isOld || isNew) << cut;
  }
}

TEST_F(SyncPolicyStoreTest, DirectoryFailureLeavesExistingPolicyUntouched) {
  const SyncPolicy oldPolicy = markedPolicy(Direction::SendOnly, false);
  ASSERT_TRUE(SyncPolicyStore::save(oldPolicy));
  const std::string oldEncoded = *Storage.getFile(SyncPolicyStore::POLICY_PATH);
  Storage.setDirectoryFailure(true);

  EXPECT_FALSE(SyncPolicyStore::save(markedPolicy(Direction::ReceiveOnly, true)));
  ASSERT_NE(Storage.getFile(SyncPolicyStore::POLICY_PATH), nullptr);
  EXPECT_EQ(*Storage.getFile(SyncPolicyStore::POLICY_PATH), oldEncoded);
}

}  // namespace

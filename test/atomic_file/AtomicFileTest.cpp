#include <AtomicFile.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>

namespace {

constexpr AtomicFile::Paths kPaths{"/state.bin", "/state.bin.tmp", "/state.bin.bak"};
constexpr char kOldPayload[] = "old-state";
constexpr char kNewPayload[] = "new-state";

struct PayloadContext {
  const char* payload = kNewPayload;
};

bool writePayload(HalFile& file, const void* context) {
  const auto* payload = static_cast<const PayloadContext*>(context);
  const size_t length = std::char_traits<char>::length(payload->payload);
  return file.write(payload->payload, length) == length;
}

bool validatePayload(const char* path, const void*) {
  const std::string* data = Storage.getFile(path);
  return data != nullptr && (*data == kOldPayload || *data == kNewPayload);
}

void expectValidFinal() {
  const std::string* finalData = Storage.getFile(kPaths.finalPath);
  ASSERT_NE(finalData, nullptr);
  EXPECT_TRUE(*finalData == kOldPayload || *finalData == kNewPayload) << *finalData;
}

class AtomicFileTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    Storage.setFile(kPaths.finalPath, kOldPayload);
  }

  PayloadContext context;
};

TEST_F(AtomicFileTest, SuccessfulWritePromotesNewAndRetainsOldBackup) {
  ASSERT_TRUE(AtomicFile::write("TEST", kPaths, writePayload, validatePayload, &context));

  ASSERT_NE(Storage.getFile(kPaths.finalPath), nullptr);
  EXPECT_EQ(*Storage.getFile(kPaths.finalPath), kNewPayload);
  ASSERT_NE(Storage.getFile(kPaths.backupPath), nullptr);
  EXPECT_EQ(*Storage.getFile(kPaths.backupPath), kOldPayload);
  EXPECT_EQ(Storage.getFile(kPaths.tempPath), nullptr);
}

TEST_F(AtomicFileTest, EveryWritePowerCutRecoversOldOrNew) {
  ASSERT_TRUE(AtomicFile::write("TEST", kPaths, writePayload, validatePayload, &context));
  const size_t successfulMutationCount = Storage.mutationCount();
  ASSERT_GT(successfulMutationCount, 0u);

  for (size_t cut = 1; cut <= successfulMutationCount; ++cut) {
    Storage.reset();
    Storage.setFile(kPaths.finalPath, kOldPayload);
    Storage.cutPowerAfterMutation(cut);

    EXPECT_THROW(AtomicFile::write("TEST", kPaths, writePayload, validatePayload, &context), FakePowerLoss)
        << "cut=" << cut;

    Storage.disablePowerCut();
    ASSERT_TRUE(AtomicFile::recover("TEST", kPaths, validatePayload, &context)) << "cut=" << cut;
    expectValidFinal();
  }
}

TEST_F(AtomicFileTest, RecoveryCanItselfBeInterruptedAndRetried) {
  Storage.reset();
  Storage.setFile(kPaths.backupPath, kOldPayload);
  Storage.setFile(kPaths.tempPath, kNewPayload);
  Storage.cutPowerAfterMutation(1);

  EXPECT_THROW(AtomicFile::recover("TEST", kPaths, validatePayload, &context), FakePowerLoss);

  Storage.disablePowerCut();
  ASSERT_TRUE(AtomicFile::recover("TEST", kPaths, validatePayload, &context));
  expectValidFinal();
  EXPECT_EQ(Storage.getFile(kPaths.tempPath), nullptr);
}

TEST_F(AtomicFileTest, FirstWriteTempIsRecoveredWhenNoOlderFileExists) {
  Storage.reset();
  Storage.setFile(kPaths.tempPath, kNewPayload);

  ASSERT_TRUE(AtomicFile::recover("TEST", kPaths, validatePayload, &context));
  ASSERT_NE(Storage.getFile(kPaths.finalPath), nullptr);
  EXPECT_EQ(*Storage.getFile(kPaths.finalPath), kNewPayload);
}

TEST_F(AtomicFileTest, InvalidTempNeverReplacesValidFinal) {
  context.payload = "corrupt";

  EXPECT_FALSE(AtomicFile::write("TEST", kPaths, writePayload, validatePayload, &context));
  ASSERT_NE(Storage.getFile(kPaths.finalPath), nullptr);
  EXPECT_EQ(*Storage.getFile(kPaths.finalPath), kOldPayload);
  EXPECT_EQ(Storage.getFile(kPaths.tempPath), nullptr);
}

TEST_F(AtomicFileTest, ShortWriteLeavesOldFinal) {
  Storage.setShortWrite(true);

  EXPECT_FALSE(AtomicFile::write("TEST", kPaths, writePayload, validatePayload, &context));
  expectValidFinal();
  EXPECT_EQ(*Storage.getFile(kPaths.finalPath), kOldPayload);
}

TEST_F(AtomicFileTest, SyncFailureLeavesOldFinal) {
  Storage.setSyncFailure(true);

  EXPECT_FALSE(AtomicFile::write("TEST", kPaths, writePayload, validatePayload, &context));
  expectValidFinal();
  EXPECT_EQ(*Storage.getFile(kPaths.finalPath), kOldPayload);
}

TEST_F(AtomicFileTest, CloseFailureLeavesOldFinal) {
  Storage.setCloseFailure(true);

  EXPECT_FALSE(AtomicFile::write("TEST", kPaths, writePayload, validatePayload, &context));
  expectValidFinal();
  EXPECT_EQ(*Storage.getFile(kPaths.finalPath), kOldPayload);
}

TEST_F(AtomicFileTest, OpenFailureLeavesOldFinal) {
  Storage.setOpenFailure(true);

  EXPECT_FALSE(AtomicFile::write("TEST", kPaths, writePayload, validatePayload, &context));
  expectValidFinal();
  EXPECT_EQ(*Storage.getFile(kPaths.finalPath), kOldPayload);
}

TEST_F(AtomicFileTest, BackupRemovalFailureLeavesOldFinalAndRecovers) {
  Storage.setFile(kPaths.backupPath, kOldPayload);
  Storage.setRemoveFailure(true);

  EXPECT_FALSE(AtomicFile::write("TEST", kPaths, writePayload, validatePayload, &context));
  expectValidFinal();
  EXPECT_EQ(*Storage.getFile(kPaths.finalPath), kOldPayload);

  Storage.setRemoveFailure(false);
  ASSERT_TRUE(AtomicFile::recover("TEST", kPaths, validatePayload, &context));
  expectValidFinal();
}

TEST_F(AtomicFileTest, BackupRenameFailureLeavesOldFinal) {
  Storage.failRenameCall(1);

  EXPECT_FALSE(AtomicFile::write("TEST", kPaths, writePayload, validatePayload, &context));
  expectValidFinal();
  EXPECT_EQ(*Storage.getFile(kPaths.finalPath), kOldPayload);
}

TEST_F(AtomicFileTest, PromotionFailureRollsBackOldFinal) {
  Storage.failRenameCall(2);

  EXPECT_FALSE(AtomicFile::write("TEST", kPaths, writePayload, validatePayload, &context));
  expectValidFinal();
  EXPECT_EQ(*Storage.getFile(kPaths.finalPath), kOldPayload);
}

TEST_F(AtomicFileTest, InvalidFinalRestoresValidBackup) {
  Storage.setFile(kPaths.finalPath, "corrupt");
  Storage.setFile(kPaths.backupPath, kOldPayload);

  ASSERT_TRUE(AtomicFile::recover("TEST", kPaths, validatePayload, &context));
  ASSERT_NE(Storage.getFile(kPaths.finalPath), nullptr);
  EXPECT_EQ(*Storage.getFile(kPaths.finalPath), kOldPayload);
}

TEST_F(AtomicFileTest, RemoveDeletesSidecarsBeforeFinal) {
  Storage.setFile(kPaths.tempPath, kNewPayload);
  Storage.setFile(kPaths.backupPath, kOldPayload);

  ASSERT_TRUE(AtomicFile::remove("TEST", kPaths));
  EXPECT_EQ(Storage.getFile(kPaths.tempPath), nullptr);
  EXPECT_EQ(Storage.getFile(kPaths.backupPath), nullptr);
  EXPECT_EQ(Storage.getFile(kPaths.finalPath), nullptr);
}

TEST_F(AtomicFileTest, EveryRemovePowerCutCanBeRetriedWithoutResurrection) {
  Storage.setFile(kPaths.tempPath, kNewPayload);
  Storage.setFile(kPaths.backupPath, kOldPayload);
  ASSERT_TRUE(AtomicFile::remove("TEST", kPaths));
  const size_t successfulMutationCount = Storage.mutationCount();
  ASSERT_EQ(successfulMutationCount, 3u);

  for (size_t cut = 1; cut <= successfulMutationCount; ++cut) {
    Storage.reset();
    Storage.setFile(kPaths.finalPath, kOldPayload);
    Storage.setFile(kPaths.tempPath, kNewPayload);
    Storage.setFile(kPaths.backupPath, kOldPayload);
    Storage.cutPowerAfterMutation(cut);

    EXPECT_THROW(AtomicFile::remove("TEST", kPaths), FakePowerLoss) << "cut=" << cut;

    Storage.disablePowerCut();
    ASSERT_TRUE(AtomicFile::remove("TEST", kPaths)) << "cut=" << cut;
    EXPECT_EQ(Storage.getFile(kPaths.tempPath), nullptr);
    EXPECT_EQ(Storage.getFile(kPaths.backupPath), nullptr);
    EXPECT_EQ(Storage.getFile(kPaths.finalPath), nullptr);
  }
}

TEST_F(AtomicFileTest, BackupRemovalFailureKeepsFinalAuthoritative) {
  Storage.setFile(kPaths.backupPath, kOldPayload);
  Storage.setRemoveFailure(true);

  EXPECT_FALSE(AtomicFile::remove("TEST", kPaths));
  ASSERT_NE(Storage.getFile(kPaths.finalPath), nullptr);
  EXPECT_EQ(*Storage.getFile(kPaths.finalPath), kOldPayload);
}

TEST_F(AtomicFileTest, RejectsOverlappingPaths) {
  const AtomicFile::Paths invalid{kPaths.finalPath, kPaths.finalPath, kPaths.backupPath};
  EXPECT_FALSE(AtomicFile::write("TEST", invalid, writePayload, validatePayload, &context));
}

TEST_F(AtomicFileTest, RejectsCrossDirectoryPromotion) {
  const AtomicFile::Paths invalid{kPaths.finalPath, "/other/state.bin.tmp", kPaths.backupPath};
  EXPECT_FALSE(AtomicFile::write("TEST", invalid, writePayload, validatePayload, &context));
}

TEST_F(AtomicFileTest, RequiresValidationCallback) {
  EXPECT_FALSE(AtomicFile::write("TEST", kPaths, writePayload, nullptr, &context));
  EXPECT_FALSE(AtomicFile::recover("TEST", kPaths, nullptr, &context));
}

TEST_F(AtomicFileTest, CanonicalNameAcceptsFinalAndAtomicSidecars) {
  char output[32];
  bool isSidecar = true;

  ASSERT_TRUE(AtomicFile::canonicalName("epub_123.bin", ".bin", output, sizeof(output), isSidecar));
  EXPECT_STREQ(output, "epub_123.bin");
  EXPECT_FALSE(isSidecar);

  ASSERT_TRUE(AtomicFile::canonicalName("epub_123.bin.tmp", ".bin", output, sizeof(output), isSidecar));
  EXPECT_STREQ(output, "epub_123.bin");
  EXPECT_TRUE(isSidecar);

  ASSERT_TRUE(AtomicFile::canonicalName("epub_123.bin.bak", ".bin", output, sizeof(output), isSidecar));
  EXPECT_STREQ(output, "epub_123.bin");
  EXPECT_TRUE(isSidecar);
}

TEST_F(AtomicFileTest, CanonicalNameRejectsWrongSuffixAndSmallOutput) {
  char output[8];
  bool isSidecar = false;

  EXPECT_FALSE(AtomicFile::canonicalName("epub_123.txt.bak", ".bin", output, sizeof(output), isSidecar));
  EXPECT_FALSE(AtomicFile::canonicalName("epub_123.bin.bak", ".bin", output, sizeof(output), isSidecar));
  EXPECT_FALSE(AtomicFile::canonicalName(nullptr, ".bin", output, sizeof(output), isSidecar));
}

}  // namespace

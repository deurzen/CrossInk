#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "LexemeStateStore.h"
#include "LocalSuppressionProjection.h"

namespace {
using dictionary::lexeme_state::StateError;
using dictionary::lexeme_state::Status;
using dictionary::lexeme_state::StorageBackend;
using dictionary::lexeme_state::Store;

struct MemoryStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  int writeAtCalls = 0;
  int failWriteAtCall = -1;
  bool failRemove = false;
};

bool ensureDirectory(void*, const char*) { return true; }
bool exists(void* context, const char* path) { return static_cast<MemoryStorage*>(context)->files.contains(path); }
bool removeFile(void* context, const char* path) {
  auto& storage = *static_cast<MemoryStorage*>(context);
  if (storage.failRemove) return false;
  return storage.files.erase(path) > 0;
}
bool renameFile(void* context, const char* oldPath, const char* newPath) {
  auto& files = static_cast<MemoryStorage*>(context)->files;
  const auto found = files.find(oldPath);
  if (found == files.end()) return false;
  files[newPath] = std::move(found->second);
  files.erase(found);
  return true;
}
bool readAt(void* context, const char* path, const uint32_t offset, void* output, const size_t length) {
  const auto& files = static_cast<MemoryStorage*>(context)->files;
  const auto found = files.find(path);
  if (found == files.end() || static_cast<uint64_t>(offset) + length > found->second.size()) return false;
  std::memcpy(output, found->second.data() + offset, length);
  return true;
}
bool writeAt(void* context, const char* path, const uint32_t offset, const void* data, const size_t length) {
  auto& storage = *static_cast<MemoryStorage*>(context);
  ++storage.writeAtCalls;
  if (storage.writeAtCalls == storage.failWriteAtCall) return false;
  auto found = storage.files.find(path);
  if (found == storage.files.end() || static_cast<uint64_t>(offset) + length > found->second.size()) return false;
  std::memcpy(found->second.data() + offset, data, length);
  return true;
}
bool writeFile(void* context, const char* path, const void* data, const size_t length) {
  auto& output = static_cast<MemoryStorage*>(context)->files[path];
  output.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + length);
  return true;
}
bool createFile(void* context, const char* path, const void* prefix, const size_t prefixLength,
                const uint32_t totalSize) {
  if (prefixLength > totalSize) return false;
  auto& output = static_cast<MemoryStorage*>(context)->files[path];
  output.assign(totalSize, 0);
  std::memcpy(output.data(), prefix, prefixLength);
  return true;
}
uint64_t fileSize(void* context, const char* path) {
  const auto& files = static_cast<MemoryStorage*>(context)->files;
  const auto found = files.find(path);
  return found == files.end() ? UINT64_MAX : found->second.size();
}
StorageBackend backend(MemoryStorage& storage) {
  return {&storage, ensureDirectory, exists, removeFile, renameFile, readAt, writeAt, writeFile, createFile, fileSize};
}

constexpr uint8_t UUID[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

bool readLocalLemma(void* context, const uint16_t localId, uint32_t& globalId) {
  const auto& ids = *static_cast<const std::vector<uint32_t>*>(context);
  if (localId >= ids.size()) return false;
  globalId = ids[localId];
  return true;
}

dictionary::suppression::LocalLemmaSource lemmaSource(std::vector<uint32_t>& ids) {
  return {&ids, static_cast<uint32_t>(ids.size()), readLocalLemma};
}

}  // namespace

TEST(LexemeState, InitializesPackedStatusesAndPersistsUpdates) {
  MemoryStorage storage;
  Store store;
  StateError error;
  ASSERT_TRUE(store.open(backend(storage), "/state", UUID, 5, error));
  EXPECT_EQ(store.generation(), 0U);
  EXPECT_EQ(store.packedByteCount(), 3U);

  ASSERT_TRUE(store.set(0, Status::Known, error));
  ASSERT_TRUE(store.set(1, Status::Learning, error));
  ASSERT_TRUE(store.set(4, Status::Ignored, error));
  EXPECT_EQ(store.generation(), 3U);

  Store reopened;
  ASSERT_TRUE(reopened.open(backend(storage), "/state", UUID, 5, error));
  Status status;
  ASSERT_TRUE(reopened.get(0, status, error));
  EXPECT_EQ(status, Status::Known);
  ASSERT_TRUE(reopened.get(1, status, error));
  EXPECT_EQ(status, Status::Learning);
  ASSERT_TRUE(reopened.get(4, status, error));
  EXPECT_EQ(status, Status::Ignored);
  EXPECT_TRUE(dictionary::lexeme_state::isSuppressed(status));
}

TEST(LexemeState, RecoversPowerLossAfterStatusByteWrite) {
  MemoryStorage storage;
  Store store;
  StateError error;
  ASSERT_TRUE(store.open(backend(storage), "/state", UUID, 10, error));
  storage.failWriteAtCall = 2;  // Nibble write succeeds; generation write fails.
  EXPECT_FALSE(store.set(3, Status::Known, error));
  EXPECT_EQ(error, StateError::IO_FAILED);

  storage.failWriteAtCall = -1;
  Store recovered;
  ASSERT_TRUE(recovered.open(backend(storage), "/state", UUID, 10, error));
  EXPECT_EQ(recovered.generation(), 1U);
  Status status;
  ASSERT_TRUE(recovered.get(3, status, error));
  EXPECT_EQ(status, Status::Known);
}

TEST(LexemeState, ReplaysWalWhenCleanupWasInterrupted) {
  MemoryStorage storage;
  Store store;
  StateError error;
  ASSERT_TRUE(store.open(backend(storage), "/state", UUID, 4, error));
  storage.failRemove = true;
  ASSERT_TRUE(store.set(2, Status::Learning, error));
  storage.failRemove = false;

  Store recovered;
  ASSERT_TRUE(recovered.open(backend(storage), "/state", UUID, 4, error));
  EXPECT_EQ(recovered.generation(), 1U);
  Status status;
  ASSERT_TRUE(recovered.get(2, status, error));
  EXPECT_EQ(status, Status::Learning);
}

TEST(LocalSuppressionProjection, RebuildsOnGenerationMismatchAndFiltersConservatively) {
  MemoryStorage storage;
  Store state;
  StateError stateError;
  ASSERT_TRUE(state.open(backend(storage), "/state", UUID, 30, stateError));
  ASSERT_TRUE(state.set(10, Status::Known, stateError));
  ASSERT_TRUE(state.set(20, Status::Learning, stateError));

  std::vector<uint32_t> globalIds = {10, 20, 21};
  uint8_t bits[1]{};
  dictionary::suppression::Projection projection;
  dictionary::suppression::ProjectionError error;
  ASSERT_TRUE(projection.loadOrRebuild(backend(storage), "/book-cache", UUID, lemmaSource(globalIds), state, bits,
                                       sizeof(bits), error));
  EXPECT_TRUE(projection.isSuppressed(0));
  EXPECT_TRUE(projection.isSuppressed(1));
  EXPECT_FALSE(projection.isSuppressed(2));
  EXPECT_EQ(projection.generation(), 2U);

  dictionary::page_shortlist::Shortlist shortlist;
  shortlist.count = 2;
  shortlist.items[0].analysisCount = 2;
  shortlist.items[0].localLemmaIds[0] = 0;
  shortlist.items[0].localLemmaIds[1] = 2;  // One plausible analysis remains unseen.
  shortlist.items[1].analysisCount = 2;
  shortlist.items[1].localLemmaIds[0] = 0;
  shortlist.items[1].localLemmaIds[1] = 1;
  projection.filter(shortlist);
  ASSERT_EQ(shortlist.count, 1);
  EXPECT_EQ(shortlist.items[0].localLemmaIds[1], 2);

  ASSERT_TRUE(state.set(21, Status::Ignored, stateError));
  uint8_t rebuiltBits[1]{};
  dictionary::suppression::Projection rebuilt;
  ASSERT_TRUE(rebuilt.loadOrRebuild(backend(storage), "/book-cache", UUID, lemmaSource(globalIds), state, rebuiltBits,
                                    sizeof(rebuiltBits), error));
  EXPECT_TRUE(rebuilt.isSuppressed(2));
  EXPECT_EQ(rebuilt.generation(), state.generation());
}

TEST(LocalSuppressionProjection, SurvivesBookCacheDeletionAndPatchesCurrentBook) {
  MemoryStorage storage;
  Store state;
  StateError stateError;
  ASSERT_TRUE(state.open(backend(storage), "/state", UUID, 8, stateError));
  std::vector<uint32_t> globalIds = {2, 5};
  uint8_t bits[1]{};
  dictionary::suppression::Projection projection;
  dictionary::suppression::ProjectionError error;
  ASSERT_TRUE(projection.loadOrRebuild(backend(storage), "/book-cache", UUID, lemmaSource(globalIds), state, bits,
                                       sizeof(bits), error));

  ASSERT_TRUE(state.set(5, Status::Known, stateError));
  storage.failWriteAtCall = storage.writeAtCalls + 2;  // Projection bit succeeds; generation write fails.
  EXPECT_FALSE(projection.patch(1, Status::Known, state.generation(), error));
  EXPECT_EQ(error, dictionary::suppression::ProjectionError::IO_FAILED);
  storage.failWriteAtCall = -1;

  uint8_t recoveredBits[1]{};
  dictionary::suppression::Projection recovered;
  ASSERT_TRUE(recovered.loadOrRebuild(backend(storage), "/book-cache", UUID, lemmaSource(globalIds), state,
                                      recoveredBits, sizeof(recoveredBits), error));
  EXPECT_TRUE(recovered.isSuppressed(1));

  for (auto it = storage.files.begin(); it != storage.files.end();) {
    if (it->first.starts_with("/book-cache/")) {
      it = storage.files.erase(it);
    } else {
      ++it;
    }
  }
  uint8_t rebuiltBits[1]{};
  dictionary::suppression::Projection rebuilt;
  ASSERT_TRUE(rebuilt.loadOrRebuild(backend(storage), "/book-cache", UUID, lemmaSource(globalIds), state, rebuiltBits,
                                    sizeof(rebuiltBits), error));
  EXPECT_TRUE(rebuilt.isSuppressed(1));
  Status persisted;
  ASSERT_TRUE(state.get(5, persisted, stateError));
  EXPECT_EQ(persisted, Status::Known);
}

TEST(LexemeState, RejectsIdentityAndWalCorruption) {
  MemoryStorage storage;
  Store store;
  StateError error;
  ASSERT_TRUE(store.open(backend(storage), "/state", UUID, 4, error));
  EXPECT_FALSE(store.open(backend(storage), "/state", UUID, 5, error));
  EXPECT_EQ(error, StateError::METADATA_INVALID);

  ASSERT_TRUE(store.open(backend(storage), "/state", UUID, 4, error));
  storage.failWriteAtCall = storage.writeAtCalls + 1;
  EXPECT_FALSE(store.set(1, Status::Known, error));
  storage.failWriteAtCall = -1;
  auto wal = storage.files.find(std::string(store.directoryPath()) + "/status.wal");
  ASSERT_NE(wal, storage.files.end());
  wal->second[10] ^= 1U;
  Store corrupted;
  EXPECT_FALSE(corrupted.open(backend(storage), "/state", UUID, 4, error));
  EXPECT_EQ(error, StateError::WAL_INVALID);
}

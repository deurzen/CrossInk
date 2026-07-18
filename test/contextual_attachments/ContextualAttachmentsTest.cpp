#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ContextualAttachments.h"
#include "ContextualSourceCatalog.h"
#include "Crc32.h"

namespace {
using dictionary::contextual::AttachmentError;
using dictionary::contextual::AttachmentRecord;
using dictionary::contextual::AttachmentStorageBackend;
using dictionary::contextual::AttachmentStore;

constexpr uint8_t CANONICAL_UUID[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr uint8_t SOURCE_A[16] = {2, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr uint8_t SOURCE_B[16] = {3, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr uint8_t SOURCE_C[16] = {4, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr char DIRECTORY[] = "/.crosspoint/lexicons/fixture";
constexpr char FINAL[] = "/.crosspoint/lexicons/fixture/attachments.bin";
constexpr char TEMP[] = "/.crosspoint/lexicons/fixture/attachments.tmp";
constexpr char BACKUP[] = "/.crosspoint/lexicons/fixture/attachments.bak";

struct MemoryStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  int renameCalls = 0;
  int failRenameCall = -1;
  int syncedWrites = 0;
};

bool exists(void* context, const char* path) { return static_cast<MemoryStorage*>(context)->files.contains(path); }

bool readExact(void* context, const char* path, void* output, const size_t length) {
  const auto& files = static_cast<MemoryStorage*>(context)->files;
  const auto found = files.find(path);
  if (found == files.end() || found->second.size() != length) return false;
  std::memcpy(output, found->second.data(), length);
  return true;
}

bool writeSynced(void* context, const char* path, const void* data, const size_t length) {
  auto& storage = *static_cast<MemoryStorage*>(context);
  const auto* bytes = static_cast<const uint8_t*>(data);
  storage.files[path] = std::vector<uint8_t>(bytes, bytes + length);
  ++storage.syncedWrites;
  return true;
}

bool removeFile(void* context, const char* path) {
  static_cast<MemoryStorage*>(context)->files.erase(path);
  return true;
}

bool renameFile(void* context, const char* oldPath, const char* newPath) {
  auto& storage = *static_cast<MemoryStorage*>(context);
  ++storage.renameCalls;
  if (storage.renameCalls == storage.failRenameCall || storage.files.contains(newPath)) return false;
  const auto found = storage.files.find(oldPath);
  if (found == storage.files.end()) return false;
  storage.files.emplace(newPath, std::move(found->second));
  storage.files.erase(found);
  return true;
}

AttachmentStorageBackend backend(MemoryStorage& storage) {
  return {&storage, exists, readExact, writeSynced, removeFile, renameFile};
}

AttachmentStore openedStore(MemoryStorage& storage) {
  AttachmentStore store;
  AttachmentError error;
  EXPECT_TRUE(store.open(backend(storage), DIRECTORY, CANONICAL_UUID, error));
  return store;
}

struct Compatibility {
  bool allowA = true;
  bool allowB = true;
  uint32_t calls = 0;
};

bool compatible(void* context, const uint8_t (&sourceUuid)[16], const uint8_t (&canonicalUuid)[16]) {
  auto& compatibility = *static_cast<Compatibility*>(context);
  ++compatibility.calls;
  if (std::memcmp(canonicalUuid, CANONICAL_UUID, 16) != 0) return false;
  if (std::memcmp(sourceUuid, SOURCE_A, 16) == 0) return compatibility.allowA;
  if (std::memcmp(sourceUuid, SOURCE_B, 16) == 0) return compatibility.allowB;
  return false;
}

struct MetadataLoaderFixture {
  uint8_t unavailableUuid[16]{};
  uint8_t incompatibleUuid[16]{};
  uint8_t loadedUuids[3][16]{};
  uint8_t loadCount = 0;
};

bool loadMetadata(void* context, const uint8_t (&sourceUuid)[16], dictionary::contextual::DefinitionMetadata& output) {
  auto& fixture = *static_cast<MetadataLoaderFixture*>(context);
  std::memcpy(fixture.loadedUuids[fixture.loadCount++], sourceUuid, 16);
  if (std::memcmp(sourceUuid, fixture.unavailableUuid, 16) == 0) return false;
  output = {};
  std::memcpy(output.sourceUuid, sourceUuid, 16);
  std::memcpy(output.canonicalUuid, CANONICAL_UUID, 16);
  if (std::memcmp(sourceUuid, fixture.incompatibleUuid, 16) == 0) output.canonicalUuid[0] ^= 1U;
  output.canonicalLexemeCount = 42;
  return true;
}

std::vector<uint8_t> encodedRecord(const uint32_t generation, const uint8_t sourceCount, const uint8_t* sources) {
  AttachmentRecord record;
  std::memcpy(record.canonicalUuid, CANONICAL_UUID, 16);
  record.generation = generation;
  record.sourceCount = sourceCount;
  if (sourceCount != 0) std::memcpy(record.sourceUuids, sources, sourceCount * 16);
  uint8_t encoded[dictionary::contextual::kAttachmentRecordSize]{};
  AttachmentError error;
  EXPECT_TRUE(dictionary::contextual::encodeAttachmentRecord(record, encoded, error));
  return {encoded, encoded + sizeof(encoded)};
}

}  // namespace

TEST(ContextualAttachmentFormat, RoundTripsOrderAndRejectsCorruption) {
  uint8_t sources[32]{};
  std::memcpy(sources, SOURCE_A, 16);
  std::memcpy(sources + 16, SOURCE_B, 16);
  std::vector<uint8_t> encoded = encodedRecord(7, 2, sources);
  AttachmentRecord parsed;
  AttachmentError error;
  ASSERT_TRUE(
      dictionary::contextual::parseAttachmentRecord(encoded.data(), encoded.size(), CANONICAL_UUID, parsed, error));
  EXPECT_EQ(parsed.generation, 7U);
  EXPECT_EQ(parsed.sourceCount, 2U);
  EXPECT_EQ(std::memcmp(parsed.sourceUuids[0], SOURCE_A, 16), 0);
  EXPECT_EQ(std::memcmp(parsed.sourceUuids[1], SOURCE_B, 16), 0);

  encoded[32] = 4;
  EXPECT_FALSE(
      dictionary::contextual::parseAttachmentRecord(encoded.data(), encoded.size(), CANONICAL_UUID, parsed, error));
  EXPECT_EQ(error, AttachmentError::BAD_CRC);

  encoded = encodedRecord(7, 2, sources);
  std::memcpy(encoded.data() + 52, SOURCE_A, 16);
  const uint32_t crc = dictionary::updateCrc32(0, encoded.data(), 84);
  encoded[84] = static_cast<uint8_t>(crc);
  encoded[85] = static_cast<uint8_t>(crc >> 8U);
  encoded[86] = static_cast<uint8_t>(crc >> 16U);
  encoded[87] = static_cast<uint8_t>(crc >> 24U);
  EXPECT_FALSE(
      dictionary::contextual::parseAttachmentRecord(encoded.data(), encoded.size(), CANONICAL_UUID, parsed, error));
  EXPECT_EQ(error, AttachmentError::DUPLICATE_SOURCE_UUID);
}

TEST(ContextualAttachmentStore, WritesSyncedOrderedRecordAndChecksGeneration) {
  MemoryStorage storage;
  AttachmentStore store = openedStore(storage);
  Compatibility compatibility;
  uint8_t sources[32]{};
  std::memcpy(sources, SOURCE_B, 16);
  std::memcpy(sources + 16, SOURCE_A, 16);
  AttachmentRecord output;
  AttachmentError error;

  ASSERT_TRUE(store.replace(sources, 2, 0, compatible, &compatibility, output, error));
  EXPECT_EQ(output.generation, 1U);
  EXPECT_EQ(std::memcmp(output.sourceUuids[0], SOURCE_B, 16), 0);
  EXPECT_EQ(storage.syncedWrites, 1);
  EXPECT_TRUE(storage.files.contains(FINAL));
  EXPECT_FALSE(storage.files.contains(TEMP));

  EXPECT_FALSE(store.replace(sources, 2, 0, compatible, &compatibility, output, error));
  EXPECT_EQ(error, AttachmentError::GENERATION_MISMATCH);
}

TEST(ContextualAttachmentStore, RejectsUnavailableSourceBeforeWriting) {
  MemoryStorage storage;
  AttachmentStore store = openedStore(storage);
  Compatibility compatibility;
  compatibility.allowB = false;
  uint8_t sources[32]{};
  std::memcpy(sources, SOURCE_A, 16);
  std::memcpy(sources + 16, SOURCE_B, 16);
  AttachmentRecord output;
  AttachmentError error;

  EXPECT_FALSE(store.replace(sources, 2, 0, compatible, &compatibility, output, error));
  EXPECT_EQ(error, AttachmentError::SOURCE_INCOMPATIBLE);
  EXPECT_EQ(storage.syncedWrites, 0);
  EXPECT_TRUE(storage.files.empty());
}

TEST(ContextualAttachmentStore, FailedReplacementRestoresPreviousRecord) {
  MemoryStorage storage;
  AttachmentStore store = openedStore(storage);
  Compatibility compatibility;
  AttachmentRecord output;
  AttachmentError error;
  ASSERT_TRUE(store.replace(SOURCE_A, 1, 0, compatible, &compatibility, output, error));

  storage.failRenameCall = storage.renameCalls + 2;
  EXPECT_FALSE(store.replace(SOURCE_B, 1, 1, compatible, &compatibility, output, error));
  EXPECT_EQ(error, AttachmentError::RENAME_FAILED);
  ASSERT_TRUE(store.load(output, error));
  EXPECT_EQ(output.generation, 1U);
  EXPECT_EQ(std::memcmp(output.sourceUuids[0], SOURCE_A, 16), 0);
  EXPECT_FALSE(storage.files.contains(BACKUP));
}

TEST(ContextualSourceCatalog, RetainsValidMetadataInAttachmentOrderAndSkipsMissingSources) {
  AttachmentRecord attachments;
  std::memcpy(attachments.canonicalUuid, CANONICAL_UUID, 16);
  attachments.generation = 9;
  attachments.sourceCount = 3;
  std::memcpy(attachments.sourceUuids[0], SOURCE_A, 16);
  std::memcpy(attachments.sourceUuids[1], SOURCE_B, 16);
  std::memcpy(attachments.sourceUuids[2], SOURCE_C, 16);
  MetadataLoaderFixture loader;
  std::memcpy(loader.unavailableUuid, SOURCE_B, 16);
  dictionary::contextual::DefinitionSourceCatalog catalog;
  dictionary::contextual::SourceCatalogError error;

  ASSERT_TRUE(dictionary::contextual::buildDefinitionSourceCatalog(attachments, CANONICAL_UUID, 42, loadMetadata,
                                                                   &loader, catalog, error));
  EXPECT_EQ(catalog.attachmentGeneration, 9U);
  EXPECT_EQ(catalog.attachedCount, 3);
  EXPECT_EQ(catalog.sourceCount, 2);
  EXPECT_EQ(catalog.skippedCount, 1);
  EXPECT_EQ(std::memcmp(catalog.sources[0].sourceUuid, SOURCE_A, 16), 0);
  EXPECT_EQ(std::memcmp(catalog.sources[1].sourceUuid, SOURCE_C, 16), 0);
  EXPECT_EQ(loader.loadCount, 3);
  EXPECT_EQ(std::memcmp(loader.loadedUuids[0], SOURCE_A, 16), 0);
  EXPECT_EQ(std::memcmp(loader.loadedUuids[1], SOURCE_B, 16), 0);
  EXPECT_EQ(std::memcmp(loader.loadedUuids[2], SOURCE_C, 16), 0);
}

TEST(ContextualSourceCatalog, SkipsLoadedMetadataWithTheWrongCanonicalIdentity) {
  AttachmentRecord attachments;
  std::memcpy(attachments.canonicalUuid, CANONICAL_UUID, 16);
  attachments.sourceCount = 2;
  std::memcpy(attachments.sourceUuids[0], SOURCE_A, 16);
  std::memcpy(attachments.sourceUuids[1], SOURCE_B, 16);
  MetadataLoaderFixture loader;
  std::memcpy(loader.incompatibleUuid, SOURCE_A, 16);
  dictionary::contextual::DefinitionSourceCatalog catalog;
  dictionary::contextual::SourceCatalogError error;

  ASSERT_TRUE(dictionary::contextual::buildDefinitionSourceCatalog(attachments, CANONICAL_UUID, 42, loadMetadata,
                                                                   &loader, catalog, error));
  ASSERT_EQ(catalog.sourceCount, 1);
  EXPECT_EQ(catalog.skippedCount, 1);
  EXPECT_EQ(std::memcmp(catalog.sources[0].sourceUuid, SOURCE_B, 16), 0);
}

TEST(ContextualSourceCatalog, RejectsCanonicalMismatchBeforeLoadingMetadata) {
  AttachmentRecord attachments;
  std::memcpy(attachments.canonicalUuid, CANONICAL_UUID, 16);
  attachments.sourceCount = 1;
  std::memcpy(attachments.sourceUuids[0], SOURCE_A, 16);
  uint8_t otherCanonical[16]{};
  std::memcpy(otherCanonical, CANONICAL_UUID, 16);
  otherCanonical[0] ^= 1U;
  MetadataLoaderFixture loader;
  dictionary::contextual::DefinitionSourceCatalog catalog;
  dictionary::contextual::SourceCatalogError error;

  EXPECT_FALSE(dictionary::contextual::buildDefinitionSourceCatalog(attachments, otherCanonical, 42, loadMetadata,
                                                                    &loader, catalog, error));
  EXPECT_EQ(error, dictionary::contextual::SourceCatalogError::CANONICAL_MISMATCH);
  EXPECT_EQ(loader.loadCount, 0);
}

TEST(ContextualAttachmentStore, RecoveryChoosesCommittedFinalOrValidBackup) {
  MemoryStorage storage;
  AttachmentStore store = openedStore(storage);
  storage.files[BACKUP] = encodedRecord(3, 1, SOURCE_A);
  storage.files[TEMP] = encodedRecord(4, 1, SOURCE_B);
  AttachmentRecord output;
  AttachmentError error;

  ASSERT_TRUE(store.load(output, error));
  EXPECT_EQ(output.generation, 3U);
  EXPECT_EQ(std::memcmp(output.sourceUuids[0], SOURCE_A, 16), 0);
  EXPECT_FALSE(storage.files.contains(TEMP));

  storage.files[BACKUP] = encodedRecord(3, 1, SOURCE_A);
  storage.files[FINAL] = encodedRecord(4, 1, SOURCE_B);
  ASSERT_TRUE(store.load(output, error));
  EXPECT_EQ(output.generation, 4U);
  EXPECT_EQ(std::memcmp(output.sourceUuids[0], SOURCE_B, 16), 0);
  EXPECT_FALSE(storage.files.contains(BACKUP));
}

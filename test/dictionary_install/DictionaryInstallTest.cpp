#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ContextualRuntimeFormat.h"
#include "Crc32.h"
#include "DictionaryInstaller.h"

namespace {
using dictionary::installer::Installer;
using dictionary::installer::InstallError;
using dictionary::installer::RuntimeFile;
using dictionary::installer::StorageBackend;

constexpr uint8_t UUID[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr uint8_t OTHER_UUID[16] = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
constexpr char UUID_HEX[] = "0102030405060708090a0b0c0d0e0f10";

struct MemoryStorage {
  std::set<std::string> directories;
  std::map<std::string, std::vector<uint8_t>> files;
  int renameCalls = 0;
  int failRenameCall = -1;
  int validateCrcCalls = 0;
  bool failRemove = false;
};

bool ensureDirectory(void* context, const char* path) {
  static_cast<MemoryStorage*>(context)->directories.emplace(path);
  return true;
}

bool exists(void* context, const char* path) {
  auto& storage = *static_cast<MemoryStorage*>(context);
  return storage.directories.contains(path) || storage.files.contains(path);
}

bool removeTree(void* context, const char* path) {
  auto& storage = *static_cast<MemoryStorage*>(context);
  if (storage.failRemove) return false;
  const std::string prefix = std::string(path) + "/";
  storage.directories.erase(path);
  for (auto it = storage.directories.begin(); it != storage.directories.end();) {
    it = it->starts_with(prefix) ? storage.directories.erase(it) : std::next(it);
  }
  for (auto it = storage.files.begin(); it != storage.files.end();) {
    it = it->first.starts_with(prefix) ? storage.files.erase(it) : std::next(it);
  }
  return true;
}

bool renameTree(void* context, const char* oldPath, const char* newPath) {
  auto& storage = *static_cast<MemoryStorage*>(context);
  ++storage.renameCalls;
  if (storage.renameCalls == storage.failRenameCall || !storage.directories.contains(oldPath) ||
      storage.directories.contains(newPath)) {
    return false;
  }
  const std::string oldPrefix = std::string(oldPath) + "/";
  const std::string newPrefix = std::string(newPath) + "/";
  storage.directories.erase(oldPath);
  storage.directories.emplace(newPath);

  std::vector<std::pair<std::string, std::vector<uint8_t>>> movedFiles;
  for (auto it = storage.files.begin(); it != storage.files.end();) {
    if (it->first.starts_with(oldPrefix)) {
      movedFiles.emplace_back(newPrefix + it->first.substr(oldPrefix.size()), std::move(it->second));
      it = storage.files.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& [path, bytes] : movedFiles) storage.files.emplace(std::move(path), std::move(bytes));
  return true;
}

uint64_t fileSize(void* context, const char* path) {
  const auto& files = static_cast<MemoryStorage*>(context)->files;
  const auto found = files.find(path);
  return found == files.end() ? std::numeric_limits<uint64_t>::max() : found->second.size();
}

bool readAt(void* context, const char* path, const uint32_t offset, void* output, const size_t length) {
  const auto& files = static_cast<MemoryStorage*>(context)->files;
  const auto found = files.find(path);
  if (found == files.end() || static_cast<uint64_t>(offset) + length > found->second.size()) return false;
  std::memcpy(output, found->second.data() + offset, length);
  return true;
}

bool validateCrc(void* context, const char* path, const uint32_t expectedCrc, uint8_t*, size_t) {
  auto& storage = *static_cast<MemoryStorage*>(context);
  ++storage.validateCrcCalls;
  const auto& files = storage.files;
  const auto found = files.find(path);
  return found != files.end() && dictionary::updateCrc32(0, found->second.data(), found->second.size()) == expectedCrc;
}

StorageBackend backend(MemoryStorage& storage) {
  return {&storage, ensureDirectory, exists, removeTree, renameTree, fileSize, readAt, validateCrc};
}

void writeU16(std::vector<uint8_t>& data, const size_t offset, const uint16_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

void writeU32(std::vector<uint8_t>& data, const size_t offset, const uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8U);
  data[offset + 2] = static_cast<uint8_t>(value >> 16U);
  data[offset + 3] = static_cast<uint8_t>(value >> 24U);
}

void writeU64(std::vector<uint8_t>& data, const size_t offset, const uint64_t value) {
  writeU32(data, offset, static_cast<uint32_t>(value));
  writeU32(data, offset + 4, static_cast<uint32_t>(value >> 32U));
}

Installer openedInstaller(MemoryStorage& storage, const char* root = "/.crosspoint/lexicons") {
  Installer installer;
  InstallError error;
  EXPECT_TRUE(installer.open(backend(storage), root, error));
  return installer;
}

struct CanonicalFixture {
  std::vector<uint8_t> meta;
  std::vector<uint8_t> lexemes;
  std::vector<uint8_t> headwords;
  std::vector<uint8_t> licenses;
};

CanonicalFixture canonicalFixture(const uint8_t (&uuid)[16] = UUID) {
  CanonicalFixture result;
  result.meta.resize(dictionary::contextual::kCanonicalMetaSize, 0);
  result.lexemes.resize(dictionary::contextual::kCanonicalLexemeRecordSize, 0);
  result.headwords = {'H', 'a', 'u', 's'};
  result.licenses = {'C', 'C', '-', 'B', 'Y', '\n'};

  writeU32(result.lexemes, 0, 0);
  writeU64(result.lexemes, 4, 0x123456789ABCDEF0ULL);
  writeU16(result.lexemes, 12, result.headwords.size());
  result.lexemes[14] = 1;

  std::memcpy(result.meta.data(), "CXCL", 4);
  writeU16(result.meta, 4, dictionary::contextual::kCanonicalFormatVersion);
  writeU16(result.meta, 6, dictionary::contextual::kCanonicalMetaSize);
  std::memcpy(result.meta.data() + 12, uuid, 16);
  std::memcpy(result.meta.data() + 28, "de", 2);
  writeU32(result.meta, 36, 1);
  writeU16(result.meta, 40, dictionary::contextual::kCanonicalLexemeRecordSize);
  writeU16(result.meta, 42, dictionary::contextual::kCanonicalPosVersion);
  writeU32(result.meta, 44, result.lexemes.size());
  writeU32(result.meta, 48, result.headwords.size());
  writeU32(result.meta, 52, dictionary::updateCrc32(0, result.lexemes.data(), result.lexemes.size()));
  writeU32(result.meta, 56, dictionary::updateCrc32(0, result.headwords.data(), result.headwords.size()));
  for (size_t i = 0; i < 32; ++i) result.meta[60 + i] = static_cast<uint8_t>(i + 1);
  writeU32(result.meta, 108, dictionary::updateCrc32(0, result.meta.data(), 108));
  return result;
}

struct DefinitionFixture {
  std::vector<uint8_t> meta;
  std::vector<uint8_t> index;
  std::vector<uint8_t> entries;
  std::vector<uint8_t> licenses;
};

DefinitionFixture definitionFixture(const uint8_t (&sourceUuid)[16] = OTHER_UUID,
                                    const uint8_t (&canonicalUuid)[16] = UUID) {
  DefinitionFixture result;
  result.meta.resize(dictionary::contextual::kDefinitionMetaSize, 0);
  result.index.resize(2 * dictionary::contextual::kDefinitionIndexRecordSize, 0);
  result.entries = {1, 0, 1, 0, 1, 0, 1, 0, 'x'};
  result.licenses = {'C', 'C', '-', 'B', 'Y', '\n'};

  writeU32(result.index, 0, 0);
  writeU32(result.index, 4, result.entries.size());
  std::memcpy(result.meta.data(), "CXDS", 4);
  writeU16(result.meta, 4, dictionary::contextual::kDefinitionFormatVersion);
  writeU16(result.meta, 6, dictionary::contextual::kDefinitionMetaSize);
  std::memcpy(result.meta.data() + 12, sourceUuid, 16);
  std::memcpy(result.meta.data() + 28, canonicalUuid, 16);
  std::memcpy(result.meta.data() + 44, "de", 2);
  std::memcpy(result.meta.data() + 52, "en", 2);
  std::memcpy(result.meta.data() + 60, "Fixture", 7);
  writeU32(result.meta, 92, 2);
  writeU16(result.meta, 96, dictionary::contextual::kDefinitionIndexRecordSize);
  writeU16(result.meta, 98, dictionary::contextual::kDefinitionEntryVersion);
  writeU32(result.meta, 100, result.index.size());
  writeU32(result.meta, 104, result.entries.size());
  writeU32(result.meta, 108, dictionary::updateCrc32(0, result.index.data(), result.index.size()));
  writeU32(result.meta, 112, dictionary::updateCrc32(0, result.entries.data(), result.entries.size()));
  writeU32(result.meta, 116, 1);
  writeU32(result.meta, 140, dictionary::updateCrc32(0, result.meta.data(), 140));
  return result;
}

void stageCanonical(Installer& installer, MemoryStorage& storage, const CanonicalFixture& data,
                    const uint8_t (&uuid)[16] = UUID) {
  InstallError error;
  ASSERT_TRUE(installer.begin(uuid, error));
  const struct {
    RuntimeFile file;
    const std::vector<uint8_t>* bytes;
  } files[] = {{RuntimeFile::Meta, &data.meta},
               {RuntimeFile::Lexemes, &data.lexemes},
               {RuntimeFile::Headwords, &data.headwords},
               {RuntimeFile::Licenses, &data.licenses}};
  for (const auto& item : files) {
    char path[dictionary::installer::kMaxInstallPath]{};
    ASSERT_TRUE(installer.stagingFilePath(uuid, item.file, path, sizeof(path), error));
    storage.files[path] = *item.bytes;
  }
}

void stageDefinition(Installer& installer, MemoryStorage& storage, const DefinitionFixture& data,
                     const uint8_t (&uuid)[16] = OTHER_UUID) {
  InstallError error;
  ASSERT_TRUE(installer.begin(uuid, error));
  const struct {
    RuntimeFile file;
    const std::vector<uint8_t>* bytes;
  } files[] = {{RuntimeFile::Meta, &data.meta},
               {RuntimeFile::EntryIndex, &data.index},
               {RuntimeFile::Entries, &data.entries},
               {RuntimeFile::Licenses, &data.licenses}};
  for (const auto& item : files) {
    char path[dictionary::installer::kMaxInstallPath]{};
    ASSERT_TRUE(installer.stagingFilePath(uuid, item.file, path, sizeof(path), error));
    storage.files[path] = *item.bytes;
  }
}

}  // namespace

TEST(DictionaryInstaller, ParsesOnlyCanonicalOrCompactPackageUuids) {
  uint8_t uuid[16]{};
  EXPECT_TRUE(dictionary::installer::parsePackageUuid("0102030405060708090a0b0c0d0e0f10", uuid));
  EXPECT_EQ(std::memcmp(uuid, UUID, sizeof(uuid)), 0);
  EXPECT_TRUE(dictionary::installer::parsePackageUuid("01020304-0506-0708-090a-0b0c0d0e0f10", uuid));
  EXPECT_EQ(std::memcmp(uuid, UUID, sizeof(uuid)), 0);
  EXPECT_FALSE(dictionary::installer::parsePackageUuid("../0102030405060708090a0b0c0d0e0f10", uuid));
  EXPECT_FALSE(dictionary::installer::parsePackageUuid("00000000-0000-0000-0000-000000000000", uuid));
  EXPECT_FALSE(dictionary::installer::parsePackageUuid("01020304_0506-0708-090a-0b0c0d0e0f10", uuid));

  char formatted[37]{};
  dictionary::installer::formatPackageUuid(UUID, formatted);
  EXPECT_STREQ(formatted, "01020304-0506-0708-090a-0b0c0d0e0f10");
}

TEST(DictionaryInstaller, ValidatesAndPublishesCanonicalLexicon) {
  MemoryStorage storage;
  Installer installer = openedInstaller(storage, "/.crosspoint/lexicons");
  stageCanonical(installer, storage, canonicalFixture());

  uint8_t scratch[64]{};
  dictionary::installer::CanonicalPackageInfo info;
  InstallError error;
  ASSERT_TRUE(installer.commitCanonical(UUID, scratch, sizeof(scratch), info, error))
      << dictionary::installer::installErrorName(error);
  EXPECT_EQ(info.lexemeCount, 1U);
  EXPECT_STREQ(info.sourceLanguage, "de");
  EXPECT_EQ(storage.validateCrcCalls, 2);
  EXPECT_TRUE(storage.directories.contains(std::string("/.crosspoint/lexicons/") + UUID_HEX));

  const int crcCalls = storage.validateCrcCalls;
  dictionary::installer::CanonicalPackageInfo inspected;
  ASSERT_TRUE(installer.inspectInstalledCanonical(UUID, inspected, error));
  EXPECT_EQ(inspected.lexemeCount, 1U);
  EXPECT_EQ(storage.validateCrcCalls, crcCalls);
}

TEST(DictionaryInstaller, CanonicalReplacementRejectsCorruptionAndRecoversRenameFailure) {
  MemoryStorage storage;
  Installer installer = openedInstaller(storage, "/.crosspoint/lexicons");
  uint8_t scratch[64]{};
  dictionary::installer::CanonicalPackageInfo info;
  InstallError error;
  stageCanonical(installer, storage, canonicalFixture());
  ASSERT_TRUE(installer.commitCanonical(UUID, scratch, sizeof(scratch), info, error));
  const std::string final = std::string("/.crosspoint/lexicons/") + UUID_HEX;
  const std::string installedMeta = final + "/meta.bin";
  const auto originalMeta = storage.files.at(installedMeta);

  CanonicalFixture corrupt = canonicalFixture();
  corrupt.headwords[0] ^= 1U;
  stageCanonical(installer, storage, corrupt);
  EXPECT_FALSE(installer.commitCanonical(UUID, scratch, sizeof(scratch), info, error));
  EXPECT_EQ(error, InstallError::CRC_MISMATCH);
  EXPECT_EQ(storage.files.at(installedMeta), originalMeta);

  stageCanonical(installer, storage, canonicalFixture());
  storage.failRenameCall = storage.renameCalls + 2;
  EXPECT_FALSE(installer.commitCanonical(UUID, scratch, sizeof(scratch), info, error));
  EXPECT_EQ(error, InstallError::RENAME_FAILED);
  EXPECT_TRUE(storage.directories.contains(final));
  EXPECT_FALSE(storage.directories.contains(std::string("/.crosspoint/lexicons/.backup-") + UUID_HEX));
}

TEST(DictionaryInstaller, ValidatesDefinitionIdentityIndexAndAtomicRemoval) {
  MemoryStorage storage;
  Installer installer = openedInstaller(storage, "/.crosspoint/definition-sources");
  stageDefinition(installer, storage, definitionFixture());

  uint8_t scratch[64]{};
  dictionary::installer::DefinitionSourcePackageInfo info;
  InstallError error;
  ASSERT_TRUE(installer.commitDefinition(OTHER_UUID, UUID, 2, scratch, sizeof(scratch), info, error))
      << dictionary::installer::installErrorName(error);
  EXPECT_EQ(info.canonicalLexemeCount, 2U);
  EXPECT_EQ(info.coverageCount, 1U);
  EXPECT_STREQ(info.sourceLabel, "Fixture");
  EXPECT_EQ(storage.validateCrcCalls, 2);
  dictionary::installer::DefinitionSourcePackageInfo inspected;
  ASSERT_TRUE(installer.inspectInstalledDefinitionMetadata(OTHER_UUID, inspected, error));
  EXPECT_EQ(std::memcmp(inspected.canonicalUuid, UUID, 16), 0);
  EXPECT_EQ(inspected.canonicalLexemeCount, 2U);

  const std::string final = "/.crosspoint/definition-sources/100f0e0d0c0b0a090807060504030201";
  EXPECT_TRUE(storage.directories.contains(final));
  storage.failRemove = true;
  ASSERT_TRUE(installer.remove(OTHER_UUID, error));
  EXPECT_FALSE(storage.directories.contains(final));
  storage.failRemove = false;
  ASSERT_TRUE(installer.recover(OTHER_UUID, error));
  EXPECT_FALSE(storage.directories.contains(final));
}

TEST(DictionaryInstaller, DefinitionInstallRejectsCanonicalAndIndexMismatchBeforePublish) {
  MemoryStorage storage;
  Installer installer = openedInstaller(storage, "/.crosspoint/definition-sources");
  uint8_t scratch[64]{};
  dictionary::installer::DefinitionSourcePackageInfo info;
  InstallError error;
  stageDefinition(installer, storage, definitionFixture());

  EXPECT_FALSE(installer.commitDefinition(OTHER_UUID, OTHER_UUID, 2, scratch, sizeof(scratch), info, error));
  EXPECT_EQ(error, InstallError::UUID_MISMATCH);

  DefinitionFixture malformed = definitionFixture();
  writeU32(malformed.index, 8, 1);
  writeU32(malformed.meta, 108, dictionary::updateCrc32(0, malformed.index.data(), malformed.index.size()));
  writeU32(malformed.meta, 140, dictionary::updateCrc32(0, malformed.meta.data(), 140));
  stageDefinition(installer, storage, malformed);
  EXPECT_FALSE(installer.commitDefinition(OTHER_UUID, UUID, 2, scratch, sizeof(scratch), info, error));
  EXPECT_EQ(error, InstallError::PACKAGE_INVALID);
  EXPECT_FALSE(storage.directories.contains("/.crosspoint/definition-sources/100f0e0d0c0b0a090807060504030201"));
}

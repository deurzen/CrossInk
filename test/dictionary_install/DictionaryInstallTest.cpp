#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Crc32.h"
#include "DictionaryInstaller.h"

namespace {
using dictionary::installer::Installer;
using dictionary::installer::InstallError;
using dictionary::installer::PackageInfo;
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

StorageBackend backend(MemoryStorage& storage) {
  return {&storage, ensureDirectory, exists, removeTree, renameTree, fileSize, readAt};
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

struct Fixture {
  std::vector<uint8_t> meta;
  std::vector<uint8_t> lexemes;
  std::vector<uint8_t> headwords;
  std::vector<uint8_t> entries;
  std::vector<uint8_t> licenses;
};

Fixture fixture(const uint8_t (&uuid)[16] = UUID) {
  Fixture result;
  result.meta.resize(dictionary::kDictionaryMetaSize, 0);
  result.lexemes.resize(dictionary::kLexemeRecordSize, 0);
  result.headwords = {'H', 'a', 'u', 's'};
  result.entries = {1, 0, 1, 0, 1, 0, 5, 0, 'h', 'o', 'u', 's', 'e'};
  result.licenses = {'C', 'C', '0', '-', '1', '.', '0', '\n'};

  writeU32(result.lexemes, 0, 0);
  writeU32(result.lexemes, 4, 0);
  writeU32(result.lexemes, 8, result.entries.size());
  writeU32(result.lexemes, 12, 1);
  writeU32(result.lexemes, 16, 0);
  writeU16(result.lexemes, 20, result.headwords.size());
  result.lexemes[22] = 1;

  std::memcpy(result.meta.data(), "CXDM", 4);
  writeU16(result.meta, 4, dictionary::kDictionaryPackageVersion);
  writeU16(result.meta, 6, dictionary::kDictionaryMetaSize);
  std::memcpy(result.meta.data() + 12, uuid, 16);
  std::memcpy(result.meta.data() + 28, "de", 2);
  std::memcpy(result.meta.data() + 36, "en", 2);
  writeU32(result.meta, 44, 1);
  writeU16(result.meta, 48, dictionary::kLexemeRecordSize);
  writeU32(result.meta, 52, result.lexemes.size());
  writeU32(result.meta, 56, result.headwords.size());
  writeU32(result.meta, 60, result.entries.size());
  writeU32(result.meta, 64, dictionary::updateCrc32(0, result.lexemes.data(), result.lexemes.size()));
  writeU32(result.meta, 68, dictionary::updateCrc32(0, result.headwords.data(), result.headwords.size()));
  writeU32(result.meta, 72, dictionary::updateCrc32(0, result.entries.data(), result.entries.size()));
  writeU32(result.meta, 76, dictionary::updateCrc32(0, result.meta.data(), 76));
  return result;
}

void stage(Installer& installer, MemoryStorage& storage, const Fixture& data, const uint8_t (&uuid)[16] = UUID) {
  InstallError error;
  ASSERT_TRUE(installer.begin(uuid, error)) << dictionary::installer::installErrorName(error);
  const struct {
    RuntimeFile file;
    const std::vector<uint8_t>* bytes;
  } files[] = {{RuntimeFile::Meta, &data.meta},
               {RuntimeFile::Lexemes, &data.lexemes},
               {RuntimeFile::Headwords, &data.headwords},
               {RuntimeFile::Entries, &data.entries},
               {RuntimeFile::Licenses, &data.licenses}};
  for (const auto& item : files) {
    char path[dictionary::installer::kMaxInstallPath]{};
    ASSERT_TRUE(installer.stagingFilePath(uuid, item.file, path, sizeof(path), error));
    storage.files[path] = *item.bytes;
  }
}

Installer openedInstaller(MemoryStorage& storage) {
  Installer installer;
  InstallError error;
  EXPECT_TRUE(installer.open(backend(storage), "/.crosspoint/dictionaries", error));
  return installer;
}

}  // namespace

TEST(DictionaryInstaller, ValidatesAndAtomicallyPublishesPackage) {
  MemoryStorage storage;
  Installer installer = openedInstaller(storage);
  stage(installer, storage, fixture());

  uint8_t scratch[64]{};
  PackageInfo info;
  InstallError error;
  ASSERT_TRUE(installer.commit(UUID, scratch, sizeof(scratch), info, error))
      << dictionary::installer::installErrorName(error);
  EXPECT_EQ(info.lexemeCount, 1U);
  EXPECT_STREQ(info.sourceLanguage, "de");
  EXPECT_TRUE(storage.directories.contains(std::string("/.crosspoint/dictionaries/") + UUID_HEX));
  EXPECT_FALSE(storage.directories.contains(std::string("/.crosspoint/dictionaries/.installing-") + UUID_HEX));
}

TEST(DictionaryInstaller, RejectsCorruptionWithoutReplacingInstalledPackage) {
  MemoryStorage storage;
  Installer installer = openedInstaller(storage);
  stage(installer, storage, fixture());
  uint8_t scratch[64]{};
  PackageInfo info;
  InstallError error;
  ASSERT_TRUE(installer.commit(UUID, scratch, sizeof(scratch), info, error));
  const std::string installedMeta = std::string("/.crosspoint/dictionaries/") + UUID_HEX + "/meta.bin";
  const auto originalMeta = storage.files.at(installedMeta);

  Fixture corrupt = fixture();
  corrupt.entries.back() ^= 0x01U;
  stage(installer, storage, corrupt);
  EXPECT_FALSE(installer.commit(UUID, scratch, sizeof(scratch), info, error));
  EXPECT_EQ(error, InstallError::CRC_MISMATCH);
  EXPECT_EQ(storage.files.at(installedMeta), originalMeta);
}

TEST(DictionaryInstaller, RejectsManifestUuidMismatch) {
  MemoryStorage storage;
  Installer installer = openedInstaller(storage);
  stage(installer, storage, fixture(OTHER_UUID));
  uint8_t scratch[64]{};
  PackageInfo info;
  InstallError error;
  EXPECT_FALSE(installer.validateStaged(UUID, scratch, sizeof(scratch), info, error));
  EXPECT_EQ(error, InstallError::UUID_MISMATCH);
}

TEST(DictionaryInstaller, RestoresPreviousPackageWhenPublishFails) {
  MemoryStorage storage;
  Installer installer = openedInstaller(storage);
  stage(installer, storage, fixture());
  uint8_t scratch[64]{};
  PackageInfo info;
  InstallError error;
  ASSERT_TRUE(installer.commit(UUID, scratch, sizeof(scratch), info, error));

  stage(installer, storage, fixture());
  storage.failRenameCall = storage.renameCalls + 2;  // Old -> backup succeeds, stage -> final fails.
  EXPECT_FALSE(installer.commit(UUID, scratch, sizeof(scratch), info, error));
  EXPECT_EQ(error, InstallError::RENAME_FAILED);
  EXPECT_TRUE(storage.directories.contains(std::string("/.crosspoint/dictionaries/") + UUID_HEX));
  EXPECT_FALSE(storage.directories.contains(std::string("/.crosspoint/dictionaries/.backup-") + UUID_HEX));
}

TEST(DictionaryInstaller, RecoversBothInterruptedReplacementPhases) {
  MemoryStorage storage;
  Installer installer = openedInstaller(storage);
  stage(installer, storage, fixture());
  uint8_t scratch[64]{};
  PackageInfo info;
  InstallError error;
  ASSERT_TRUE(installer.commit(UUID, scratch, sizeof(scratch), info, error));

  const std::string final = std::string("/.crosspoint/dictionaries/") + UUID_HEX;
  const std::string backup = std::string("/.crosspoint/dictionaries/.backup-") + UUID_HEX;
  ASSERT_TRUE(renameTree(&storage, final.c_str(), backup.c_str()));
  ASSERT_TRUE(installer.recover(UUID, error));
  EXPECT_TRUE(storage.directories.contains(final));
  EXPECT_FALSE(storage.directories.contains(backup));

  storage.directories.emplace(backup);
  ASSERT_TRUE(installer.recover(UUID, error));
  EXPECT_TRUE(storage.directories.contains(final));
  EXPECT_FALSE(storage.directories.contains(backup));
}

TEST(DictionaryInstaller, FailedRemovalRestoresInstalledPackage) {
  MemoryStorage storage;
  Installer installer = openedInstaller(storage);
  stage(installer, storage, fixture());
  uint8_t scratch[64]{};
  PackageInfo info;
  InstallError error;
  ASSERT_TRUE(installer.commit(UUID, scratch, sizeof(scratch), info, error));

  storage.failRemove = true;
  EXPECT_FALSE(installer.remove(UUID, error));
  EXPECT_EQ(error, InstallError::REMOVE_FAILED);
  EXPECT_TRUE(storage.directories.contains(std::string("/.crosspoint/dictionaries/") + UUID_HEX));
}

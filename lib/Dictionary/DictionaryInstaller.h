#pragma once

#include <cstddef>
#include <cstdint>

#include "DictionaryPackage.h"

namespace dictionary::installer {

constexpr size_t kMaxRootPath = 128;
constexpr size_t kMaxInstallPath = 192;
constexpr uint32_t kMaxLicenseBytes = 1024U * 1024U;
constexpr size_t kMinimumValidationScratch = 64;

enum class RuntimeFile : uint8_t {
  Meta = 0,
  Lexemes,
  Headwords,
  Entries,
  Licenses,
  EntryIndex,
};

struct StorageBackend {
  void* context = nullptr;
  bool (*ensureDirectory)(void* context, const char* path) = nullptr;
  bool (*exists)(void* context, const char* path) = nullptr;
  bool (*removeTree)(void* context, const char* path) = nullptr;
  bool (*rename)(void* context, const char* oldPath, const char* newPath) = nullptr;
  uint64_t (*fileSize)(void* context, const char* path) = nullptr;
  bool (*readAt)(void* context, const char* path, uint32_t offset, void* output, size_t length) = nullptr;
  bool (*validateCrc)(void* context, const char* path, uint32_t expectedCrc, uint8_t* scratch,
                      size_t scratchSize) = nullptr;
};

struct PackageInfo {
  uint8_t bundleUuid[16]{};
  char sourceLanguage[8]{};
  char targetLanguage[8]{};
  uint32_t lexemeCount = 0;
  uint64_t runtimeBytes = 0;
};

struct CanonicalPackageInfo {
  uint8_t canonicalUuid[16]{};
  char sourceLanguage[8]{};
  uint32_t lexemeCount = 0;
  uint64_t runtimeBytes = 0;
};

struct DefinitionSourcePackageInfo {
  uint8_t sourceUuid[16]{};
  uint8_t canonicalUuid[16]{};
  char sourceLanguage[8]{};
  char targetLanguage[8]{};
  char sourceLabel[32]{};
  uint32_t canonicalLexemeCount = 0;
  uint32_t coverageCount = 0;
  uint64_t runtimeBytes = 0;
};

enum class InstallError : uint8_t {
  NONE = 0,
  INVALID_INPUT,
  STORAGE_UNAVAILABLE,
  DIRECTORY_FAILED,
  PATH_TOO_LONG,
  STAGING_MISSING,
  PACKAGE_MISSING,
  REQUIRED_FILE_MISSING,
  LICENSE_INVALID,
  PACKAGE_INVALID,
  UUID_MISMATCH,
  CRC_MISMATCH,
  PREPARE_FAILED,
  RENAME_FAILED,
  REMOVE_FAILED,
};

// Manages one package UUID at a time. Upload handlers write only to paths
// returned by stagingFilePath(); commit validates every runtime file before a
// directory rename publishes it. A same-UUID replacement retains the previous
// directory as a recoverable backup until the new directory is visible.
using PrepareCallback = bool (*)(void* context, const uint8_t (&bundleUuid)[16], uint32_t lexemeCount);

class Installer {
 public:
  bool open(const StorageBackend& storage, const char* rootPath, InstallError& error);

  bool begin(const uint8_t (&bundleUuid)[16], InstallError& error);
  bool stagingFilePath(const uint8_t (&bundleUuid)[16], RuntimeFile file, char* output, size_t capacity,
                       InstallError& error);
  bool installedDirectoryPath(const uint8_t (&bundleUuid)[16], char* output, size_t capacity,
                              InstallError& error) const;
  bool validateStaged(const uint8_t (&bundleUuid)[16], uint8_t* scratch, size_t scratchSize, PackageInfo& info,
                      InstallError& error);
  bool inspectInstalled(const uint8_t (&bundleUuid)[16], PackageInfo& info, InstallError& error);
  bool commit(const uint8_t (&bundleUuid)[16], uint8_t* scratch, size_t scratchSize, PackageInfo& info,
              InstallError& error, PrepareCallback prepare = nullptr, void* prepareContext = nullptr);

  bool validateStagedCanonical(const uint8_t (&canonicalUuid)[16], uint8_t* scratch, size_t scratchSize,
                               CanonicalPackageInfo& info, InstallError& error);
  bool inspectInstalledCanonical(const uint8_t (&canonicalUuid)[16], CanonicalPackageInfo& info, InstallError& error);
  bool commitCanonical(const uint8_t (&canonicalUuid)[16], uint8_t* scratch, size_t scratchSize,
                       CanonicalPackageInfo& info, InstallError& error);

  bool validateStagedDefinition(const uint8_t (&sourceUuid)[16], const uint8_t (&expectedCanonicalUuid)[16],
                                uint32_t expectedCanonicalCount, uint8_t* scratch, size_t scratchSize,
                                DefinitionSourcePackageInfo& info, InstallError& error);
  bool inspectInstalledDefinition(const uint8_t (&sourceUuid)[16], const uint8_t (&expectedCanonicalUuid)[16],
                                  uint32_t expectedCanonicalCount, DefinitionSourcePackageInfo& info,
                                  InstallError& error);
  bool inspectInstalledDefinitionMetadata(const uint8_t (&sourceUuid)[16], DefinitionSourcePackageInfo& info,
                                          InstallError& error);
  bool commitDefinition(const uint8_t (&sourceUuid)[16], const uint8_t (&expectedCanonicalUuid)[16],
                        uint32_t expectedCanonicalCount, uint8_t* scratch, size_t scratchSize,
                        DefinitionSourcePackageInfo& info, InstallError& error);
  bool cancel(const uint8_t (&bundleUuid)[16], InstallError& error);
  bool remove(const uint8_t (&bundleUuid)[16], InstallError& error);
  bool recover(const uint8_t (&bundleUuid)[16], InstallError& error);

 private:
  struct SourceContext {
    Installer* installer = nullptr;
    const uint8_t* bundleUuid = nullptr;
    const char* prefix = nullptr;
    RuntimeFile file = RuntimeFile::Meta;
  };

  StorageBackend storage_{};
  char rootPath_[kMaxRootPath]{};
  mutable char pathScratch_[kMaxInstallPath]{};
  mutable char pathScratch2_[kMaxInstallPath]{};
  mutable char pathScratch3_[kMaxInstallPath]{};
  bool open_ = false;

  bool directoryPath(const uint8_t (&bundleUuid)[16], const char* prefix, char* output, size_t capacity,
                     InstallError& error) const;
  bool filePath(const uint8_t (&bundleUuid)[16], const char* prefix, RuntimeFile file, char* output, size_t capacity,
                InstallError& error) const;
  bool makeSource(SourceContext& context, const uint8_t (&bundleUuid)[16], const char* prefix, RuntimeFile file,
                  RandomAccessSource& source, InstallError& error);
  bool validatePackage(const uint8_t (&bundleUuid)[16], const char* prefix, uint8_t* scratch, size_t scratchSize,
                       bool verifyCrc, PackageInfo& info, InstallError& error);
  bool validateCanonicalPackage(const uint8_t (&canonicalUuid)[16], const char* prefix, uint8_t* scratch,
                                size_t scratchSize, bool verifyPayload, CanonicalPackageInfo& info,
                                InstallError& error);
  bool validateDefinitionPackage(const uint8_t (&sourceUuid)[16], const char* prefix,
                                 const uint8_t (&expectedCanonicalUuid)[16], uint32_t expectedCanonicalCount,
                                 uint8_t* scratch, size_t scratchSize, bool verifyPayload,
                                 DefinitionSourcePackageInfo& info, InstallError& error);
  bool publishStaged(const uint8_t (&uuid)[16], InstallError& error);
  static bool sourceReadAt(void* context, uint32_t offset, void* output, size_t length);
};

bool parseBundleUuid(const char* text, uint8_t (&uuid)[16]);
void formatBundleUuid(const uint8_t (&uuid)[16], char (&output)[37]);
const char* runtimeFileName(RuntimeFile file);
const char* installErrorName(InstallError error);

}  // namespace dictionary::installer

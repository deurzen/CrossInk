#include "DictionaryInstaller.h"

#include <cstdio>
#include <cstring>
#include <limits>

#include "ContextualRuntimeFormat.h"

namespace dictionary::installer {
namespace {

constexpr char kStagePrefix[] = ".installing-";
constexpr char kBackupPrefix[] = ".backup-";
constexpr char kRemovalPrefix[] = ".removing-";

bool validUuid(const uint8_t (&uuid)[16]) {
  for (const uint8_t byte : uuid) {
    if (byte != 0) return true;
  }
  return false;
}

void uuidHex(const uint8_t (&uuid)[16], char (&output)[33]) {
  static constexpr char HEX[] = "0123456789abcdef";
  for (size_t i = 0; i < 16; ++i) {
    output[i * 2] = HEX[uuid[i] >> 4U];
    output[i * 2 + 1] = HEX[uuid[i] & 0x0FU];
  }
  output[32] = '\0';
}

bool backendValid(const StorageBackend& storage) {
  return storage.ensureDirectory != nullptr && storage.exists != nullptr && storage.removeTree != nullptr &&
         storage.rename != nullptr && storage.fileSize != nullptr && storage.readAt != nullptr &&
         storage.validateCrc != nullptr;
}

uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

int hexValue(const char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

}  // namespace

bool parsePackageUuid(const char* text, uint8_t (&uuid)[16]) {
  std::memset(uuid, 0, sizeof(uuid));
  if (text == nullptr) return false;
  const size_t length = std::strlen(text);
  if (length != 32 && length != 36) return false;
  size_t input = 0;
  for (size_t output = 0; output < sizeof(uuid); ++output) {
    if (length == 36 && (input == 8 || input == 13 || input == 18 || input == 23)) {
      if (text[input++] != '-') return false;
    }
    const int high = hexValue(text[input++]);
    const int low = hexValue(text[input++]);
    if (high < 0 || low < 0) {
      std::memset(uuid, 0, sizeof(uuid));
      return false;
    }
    uuid[output] = static_cast<uint8_t>((high << 4U) | low);
  }
  return input == length && validUuid(uuid);
}

void formatPackageUuid(const uint8_t (&uuid)[16], char (&output)[37]) {
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  size_t position = 0;
  for (size_t i = 0; i < sizeof(uuid); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) output[position++] = '-';
    output[position++] = HEX_DIGITS[uuid[i] >> 4U];
    output[position++] = HEX_DIGITS[uuid[i] & 0x0FU];
  }
  output[position] = '\0';
}

const char* runtimeFileName(const RuntimeFile file) {
  switch (file) {
    case RuntimeFile::Meta:
      return "meta.bin";
    case RuntimeFile::Lexemes:
      return "lexemes.bin";
    case RuntimeFile::Headwords:
      return "headwords.bin";
    case RuntimeFile::Entries:
      return "entries.bin";
    case RuntimeFile::Licenses:
      return "licenses.txt";
    case RuntimeFile::EntryIndex:
      return "entry-index.bin";
  }
  return nullptr;
}

bool Installer::open(const StorageBackend& storage, const char* rootPath, InstallError& error) {
  open_ = false;
  storage_ = {};
  rootPath_[0] = '\0';
  error = InstallError::NONE;
  if (!backendValid(storage)) {
    error = InstallError::STORAGE_UNAVAILABLE;
    return false;
  }
  if (rootPath == nullptr || rootPath[0] != '/') {
    error = InstallError::INVALID_INPUT;
    return false;
  }
  const size_t length = std::strlen(rootPath);
  if (length == 0 || length >= sizeof(rootPath_)) {
    error = InstallError::PATH_TOO_LONG;
    return false;
  }
  storage_ = storage;
  std::memcpy(rootPath_, rootPath, length + 1);
  if (!storage_.ensureDirectory(storage_.context, rootPath_)) {
    storage_ = {};
    error = InstallError::DIRECTORY_FAILED;
    return false;
  }
  open_ = true;
  return true;
}

bool Installer::directoryPath(const uint8_t (&packageUuid)[16], const char* prefix, char* output, const size_t capacity,
                              InstallError& error) const {
  if (!open_ || !validUuid(packageUuid) || prefix == nullptr || output == nullptr || capacity == 0) {
    error = InstallError::INVALID_INPUT;
    return false;
  }
  char hex[33]{};
  uuidHex(packageUuid, hex);
  const int written = std::snprintf(output, capacity, "%s/%s%s", rootPath_, prefix, hex);
  if (written < 0 || static_cast<size_t>(written) >= capacity) {
    error = InstallError::PATH_TOO_LONG;
    return false;
  }
  return true;
}

bool Installer::filePath(const uint8_t (&packageUuid)[16], const char* prefix, const RuntimeFile file, char* output,
                         const size_t capacity, InstallError& error) const {
  const char* name = runtimeFileName(file);
  if (name == nullptr || !directoryPath(packageUuid, prefix, output, capacity, error)) return false;
  const size_t used = std::strlen(output);
  const int written = std::snprintf(output + used, capacity - used, "/%s", name);
  if (written < 0 || static_cast<size_t>(written) >= capacity - used) {
    error = InstallError::PATH_TOO_LONG;
    return false;
  }
  return true;
}

bool Installer::installedDirectoryPath(const uint8_t (&packageUuid)[16], char* output, const size_t capacity,
                                       InstallError& error) const {
  error = InstallError::NONE;
  return directoryPath(packageUuid, "", output, capacity, error);
}

bool Installer::begin(const uint8_t (&packageUuid)[16], InstallError& error) {
  error = InstallError::NONE;
  if (!recover(packageUuid, error) ||
      !directoryPath(packageUuid, kStagePrefix, pathScratch_, sizeof(pathScratch_), error)) {
    return false;
  }
  if (storage_.exists(storage_.context, pathScratch_) && !storage_.removeTree(storage_.context, pathScratch_)) {
    error = InstallError::REMOVE_FAILED;
    return false;
  }
  if (!storage_.ensureDirectory(storage_.context, pathScratch_)) {
    error = InstallError::DIRECTORY_FAILED;
    return false;
  }
  return true;
}

bool Installer::stagingFilePath(const uint8_t (&packageUuid)[16], const RuntimeFile file, char* output,
                                const size_t capacity, InstallError& error) {
  error = InstallError::NONE;
  return filePath(packageUuid, kStagePrefix, file, output, capacity, error);
}

bool Installer::sourceReadAt(void* rawContext, const uint32_t offset, void* output, const size_t length) {
  auto& context = *static_cast<SourceContext*>(rawContext);
  if (context.installer == nullptr || context.packageUuid == nullptr) return false;
  const auto& uuid = *reinterpret_cast<const uint8_t (*)[16]>(context.packageUuid);
  InstallError error = InstallError::NONE;
  if (!context.installer->filePath(uuid, context.prefix, context.file, context.installer->pathScratch_,
                                   sizeof(context.installer->pathScratch_), error)) {
    return false;
  }
  return context.installer->storage_.readAt(context.installer->storage_.context, context.installer->pathScratch_,
                                            offset, output, length);
}

bool Installer::makeSource(SourceContext& context, const uint8_t (&packageUuid)[16], const char* prefix,
                           const RuntimeFile file, RandomAccessSource& source, InstallError& error) {
  if (!filePath(packageUuid, prefix, file, pathScratch_, sizeof(pathScratch_), error)) return false;
  const uint64_t size = storage_.fileSize(storage_.context, pathScratch_);
  if (size == std::numeric_limits<uint64_t>::max()) {
    error = InstallError::REQUIRED_FILE_MISSING;
    return false;
  }
  context = {this, packageUuid, prefix, file};
  source = {&context, size, sourceReadAt};
  return true;
}

bool Installer::validateCanonicalPackage(const uint8_t (&canonicalUuid)[16], const char* prefix, uint8_t* scratch,
                                         const size_t scratchSize, const bool verifyPayload, CanonicalPackageInfo& info,
                                         InstallError& error) {
  info = {};
  error = InstallError::NONE;
  if (!open_ || !validUuid(canonicalUuid) || prefix == nullptr ||
      (verifyPayload && (scratch == nullptr || scratchSize < kMinimumValidationScratch))) {
    error = InstallError::INVALID_INPUT;
    return false;
  }
  if (!directoryPath(canonicalUuid, prefix, pathScratch_, sizeof(pathScratch_), error)) return false;
  if (!storage_.exists(storage_.context, pathScratch_)) {
    error = prefix[0] == '\0' ? InstallError::PACKAGE_MISSING : InstallError::STAGING_MISSING;
    return false;
  }

  SourceContext metaContext;
  SourceContext lexemesContext;
  SourceContext headwordsContext;
  RandomAccessSource meta;
  RandomAccessSource lexemes;
  RandomAccessSource headwords;
  if (!makeSource(metaContext, canonicalUuid, prefix, RuntimeFile::Meta, meta, error) ||
      !makeSource(lexemesContext, canonicalUuid, prefix, RuntimeFile::Lexemes, lexemes, error) ||
      !makeSource(headwordsContext, canonicalUuid, prefix, RuntimeFile::Headwords, headwords, error)) {
    return false;
  }
  if (!filePath(canonicalUuid, prefix, RuntimeFile::Licenses, pathScratch_, sizeof(pathScratch_), error)) return false;
  const uint64_t licenseSize = storage_.fileSize(storage_.context, pathScratch_);
  if (licenseSize == std::numeric_limits<uint64_t>::max()) {
    error = InstallError::REQUIRED_FILE_MISSING;
    return false;
  }
  if (licenseSize == 0 || licenseSize > kMaxLicenseBytes) {
    error = InstallError::LICENSE_INVALID;
    return false;
  }

  contextual::CanonicalLexiconReader package;
  contextual::RuntimeFormatError formatError;
  if (!package.open(meta, lexemes, headwords, formatError)) {
    error = InstallError::PACKAGE_INVALID;
    return false;
  }
  const contextual::CanonicalMetadata& metadata = package.metadata();
  if (std::memcmp(metadata.canonicalUuid, canonicalUuid, sizeof(metadata.canonicalUuid)) != 0) {
    error = InstallError::UUID_MISMATCH;
    return false;
  }
  if (verifyPayload) {
    const struct {
      RuntimeFile file;
      uint32_t crc;
    } checks[] = {{RuntimeFile::Lexemes, metadata.lexemesCrc32}, {RuntimeFile::Headwords, metadata.headwordsCrc32}};
    for (const auto& check : checks) {
      if (!filePath(canonicalUuid, prefix, check.file, pathScratch_, sizeof(pathScratch_), error)) return false;
      if (!storage_.validateCrc(storage_.context, pathScratch_, check.crc, scratch, scratchSize)) {
        error = InstallError::CRC_MISMATCH;
        return false;
      }
    }
    if (!package.validateLexemes(scratch, scratchSize, formatError)) {
      error = InstallError::PACKAGE_INVALID;
      return false;
    }
  }

  std::memcpy(info.canonicalUuid, metadata.canonicalUuid, sizeof(info.canonicalUuid));
  std::memcpy(info.sourceLanguage, metadata.sourceLanguage, sizeof(info.sourceLanguage));
  info.lexemeCount = metadata.lexemeCount;
  info.runtimeBytes = contextual::kCanonicalMetaSize + static_cast<uint64_t>(metadata.lexemesFileSize) +
                      metadata.headwordsFileSize + licenseSize;
  return true;
}

bool Installer::validateDefinitionPackage(const uint8_t (&sourceUuid)[16], const char* prefix,
                                          const uint8_t (&expectedCanonicalUuid)[16],
                                          const uint32_t expectedCanonicalCount, uint8_t* scratch,
                                          const size_t scratchSize, const bool verifyPayload,
                                          DefinitionSourcePackageInfo& info, InstallError& error) {
  info = {};
  error = InstallError::NONE;
  if (!open_ || !validUuid(sourceUuid) || !validUuid(expectedCanonicalUuid) || expectedCanonicalCount == 0 ||
      prefix == nullptr || (verifyPayload && (scratch == nullptr || scratchSize < kMinimumValidationScratch))) {
    error = InstallError::INVALID_INPUT;
    return false;
  }
  if (!directoryPath(sourceUuid, prefix, pathScratch_, sizeof(pathScratch_), error)) return false;
  if (!storage_.exists(storage_.context, pathScratch_)) {
    error = prefix[0] == '\0' ? InstallError::PACKAGE_MISSING : InstallError::STAGING_MISSING;
    return false;
  }

  SourceContext metaContext;
  SourceContext indexContext;
  SourceContext entriesContext;
  RandomAccessSource meta;
  RandomAccessSource index;
  RandomAccessSource entries;
  if (!makeSource(metaContext, sourceUuid, prefix, RuntimeFile::Meta, meta, error) ||
      !makeSource(indexContext, sourceUuid, prefix, RuntimeFile::EntryIndex, index, error) ||
      !makeSource(entriesContext, sourceUuid, prefix, RuntimeFile::Entries, entries, error)) {
    return false;
  }
  if (!filePath(sourceUuid, prefix, RuntimeFile::Licenses, pathScratch_, sizeof(pathScratch_), error)) return false;
  const uint64_t licenseSize = storage_.fileSize(storage_.context, pathScratch_);
  if (licenseSize == std::numeric_limits<uint64_t>::max()) {
    error = InstallError::REQUIRED_FILE_MISSING;
    return false;
  }
  if (licenseSize == 0 || licenseSize > kMaxLicenseBytes) {
    error = InstallError::LICENSE_INVALID;
    return false;
  }

  contextual::DefinitionSourceReader package;
  contextual::RuntimeFormatError formatError;
  if (!package.open(meta, index, entries, expectedCanonicalUuid, expectedCanonicalCount, formatError)) {
    error = formatError == contextual::RuntimeFormatError::CANONICAL_MISMATCH ? InstallError::UUID_MISMATCH
                                                                              : InstallError::PACKAGE_INVALID;
    return false;
  }
  const contextual::DefinitionMetadata& metadata = package.metadata();
  if (std::memcmp(metadata.sourceUuid, sourceUuid, sizeof(metadata.sourceUuid)) != 0) {
    error = InstallError::UUID_MISMATCH;
    return false;
  }
  if (verifyPayload) {
    const struct {
      RuntimeFile file;
      uint32_t crc;
    } checks[] = {{RuntimeFile::EntryIndex, metadata.indexCrc32}, {RuntimeFile::Entries, metadata.entriesCrc32}};
    for (const auto& check : checks) {
      if (!filePath(sourceUuid, prefix, check.file, pathScratch_, sizeof(pathScratch_), error)) return false;
      if (!storage_.validateCrc(storage_.context, pathScratch_, check.crc, scratch, scratchSize)) {
        error = InstallError::CRC_MISMATCH;
        return false;
      }
    }
    if (!package.validateIndex(scratch, scratchSize, formatError)) {
      error = InstallError::PACKAGE_INVALID;
      return false;
    }
  }

  std::memcpy(info.sourceUuid, metadata.sourceUuid, sizeof(info.sourceUuid));
  std::memcpy(info.canonicalUuid, metadata.canonicalUuid, sizeof(info.canonicalUuid));
  std::memcpy(info.sourceLanguage, metadata.sourceLanguage, sizeof(info.sourceLanguage));
  std::memcpy(info.targetLanguage, metadata.targetLanguage, sizeof(info.targetLanguage));
  std::memcpy(info.sourceLabel, metadata.sourceLabel, sizeof(info.sourceLabel));
  info.canonicalLexemeCount = metadata.canonicalLexemeCount;
  info.coverageCount = metadata.coverageCount;
  info.runtimeBytes = contextual::kDefinitionMetaSize + static_cast<uint64_t>(metadata.indexFileSize) +
                      metadata.entriesFileSize + licenseSize;
  return true;
}

bool Installer::validateStagedCanonical(const uint8_t (&canonicalUuid)[16], uint8_t* scratch, const size_t scratchSize,
                                        CanonicalPackageInfo& info, InstallError& error) {
  return validateCanonicalPackage(canonicalUuid, kStagePrefix, scratch, scratchSize, true, info, error);
}

bool Installer::inspectInstalledCanonical(const uint8_t (&canonicalUuid)[16], CanonicalPackageInfo& info,
                                          InstallError& error) {
  if (!recover(canonicalUuid, error)) return false;
  return validateCanonicalPackage(canonicalUuid, "", nullptr, 0, false, info, error);
}

bool Installer::validateStagedDefinition(const uint8_t (&sourceUuid)[16], const uint8_t (&expectedCanonicalUuid)[16],
                                         const uint32_t expectedCanonicalCount, uint8_t* scratch,
                                         const size_t scratchSize, DefinitionSourcePackageInfo& info,
                                         InstallError& error) {
  return validateDefinitionPackage(sourceUuid, kStagePrefix, expectedCanonicalUuid, expectedCanonicalCount, scratch,
                                   scratchSize, true, info, error);
}

bool Installer::inspectInstalledDefinition(const uint8_t (&sourceUuid)[16], const uint8_t (&expectedCanonicalUuid)[16],
                                           const uint32_t expectedCanonicalCount, DefinitionSourcePackageInfo& info,
                                           InstallError& error) {
  if (!recover(sourceUuid, error)) return false;
  return validateDefinitionPackage(sourceUuid, "", expectedCanonicalUuid, expectedCanonicalCount, nullptr, 0, false,
                                   info, error);
}

bool Installer::inspectInstalledDefinitionMetadata(const uint8_t (&sourceUuid)[16], DefinitionSourcePackageInfo& info,
                                                   InstallError& error) {
  if (!recover(sourceUuid, error)) return false;
  SourceContext metaContext;
  RandomAccessSource meta;
  if (!makeSource(metaContext, sourceUuid, "", RuntimeFile::Meta, meta, error)) return false;
  if (meta.size != contextual::kDefinitionMetaSize) {
    error = InstallError::PACKAGE_INVALID;
    return false;
  }
  uint8_t header[contextual::kDefinitionMetaSize]{};
  if (!meta.readAt(meta.context, 0, header, sizeof(header))) {
    error = InstallError::PACKAGE_INVALID;
    return false;
  }
  uint8_t canonicalUuid[16]{};
  std::memcpy(canonicalUuid, header + 28, sizeof(canonicalUuid));
  const uint32_t canonicalCount = readU32(header + 92);
  if (!validUuid(canonicalUuid) || canonicalCount == 0) {
    error = InstallError::PACKAGE_INVALID;
    return false;
  }
  return validateDefinitionPackage(sourceUuid, "", canonicalUuid, canonicalCount, nullptr, 0, false, info, error);
}

bool Installer::recover(const uint8_t (&packageUuid)[16], InstallError& error) {
  error = InstallError::NONE;
  if (!directoryPath(packageUuid, "", pathScratch_, sizeof(pathScratch_), error) ||
      !directoryPath(packageUuid, kBackupPrefix, pathScratch2_, sizeof(pathScratch2_), error) ||
      !directoryPath(packageUuid, kRemovalPrefix, pathScratch3_, sizeof(pathScratch3_), error)) {
    return false;
  }
  // A removal directory means the final->hidden rename already committed.
  // Cleanup is best-effort; it must never make a partially deleted package
  // visible again.
  if (storage_.exists(storage_.context, pathScratch3_)) storage_.removeTree(storage_.context, pathScratch3_);
  const bool installed = storage_.exists(storage_.context, pathScratch_);
  const bool backup = storage_.exists(storage_.context, pathScratch2_);
  if (!backup) return true;
  if (installed) {
    if (!storage_.removeTree(storage_.context, pathScratch2_)) {
      error = InstallError::REMOVE_FAILED;
      return false;
    }
    return true;
  }
  if (!storage_.rename(storage_.context, pathScratch2_, pathScratch_)) {
    error = InstallError::RENAME_FAILED;
    return false;
  }
  return true;
}

bool Installer::publishStaged(const uint8_t (&uuid)[16], InstallError& error) {
  if (!recover(uuid, error) || !directoryPath(uuid, "", pathScratch_, sizeof(pathScratch_), error) ||
      !directoryPath(uuid, kStagePrefix, pathScratch2_, sizeof(pathScratch2_), error) ||
      !directoryPath(uuid, kBackupPrefix, pathScratch3_, sizeof(pathScratch3_), error)) {
    return false;
  }

  const bool replacing = storage_.exists(storage_.context, pathScratch_);
  if (replacing && !storage_.rename(storage_.context, pathScratch_, pathScratch3_)) {
    error = InstallError::RENAME_FAILED;
    return false;
  }
  if (!storage_.rename(storage_.context, pathScratch2_, pathScratch_)) {
    if (replacing) storage_.rename(storage_.context, pathScratch3_, pathScratch_);
    error = InstallError::RENAME_FAILED;
    return false;
  }
  // The final directory rename is the commit point. A leftover backup is
  // harmless and recover() removes it on the next operation.
  if (replacing) storage_.removeTree(storage_.context, pathScratch3_);
  error = InstallError::NONE;
  return true;
}

bool Installer::commitCanonical(const uint8_t (&canonicalUuid)[16], uint8_t* scratch, const size_t scratchSize,
                                CanonicalPackageInfo& info, InstallError& error) {
  if (!validateStagedCanonical(canonicalUuid, scratch, scratchSize, info, error)) return false;
  return publishStaged(canonicalUuid, error);
}

bool Installer::commitDefinition(const uint8_t (&sourceUuid)[16], const uint8_t (&expectedCanonicalUuid)[16],
                                 const uint32_t expectedCanonicalCount, uint8_t* scratch, const size_t scratchSize,
                                 DefinitionSourcePackageInfo& info, InstallError& error) {
  if (!validateStagedDefinition(sourceUuid, expectedCanonicalUuid, expectedCanonicalCount, scratch, scratchSize, info,
                                error)) {
    return false;
  }
  return publishStaged(sourceUuid, error);
}

bool Installer::cancel(const uint8_t (&packageUuid)[16], InstallError& error) {
  error = InstallError::NONE;
  if (!directoryPath(packageUuid, kStagePrefix, pathScratch_, sizeof(pathScratch_), error)) return false;
  if (!storage_.exists(storage_.context, pathScratch_)) return true;
  if (!storage_.removeTree(storage_.context, pathScratch_)) {
    error = InstallError::REMOVE_FAILED;
    return false;
  }
  return true;
}

bool Installer::remove(const uint8_t (&packageUuid)[16], InstallError& error) {
  if (!recover(packageUuid, error) || !directoryPath(packageUuid, "", pathScratch_, sizeof(pathScratch_), error) ||
      !directoryPath(packageUuid, kRemovalPrefix, pathScratch2_, sizeof(pathScratch2_), error)) {
    return false;
  }
  if (!storage_.exists(storage_.context, pathScratch_)) return true;
  if (!storage_.rename(storage_.context, pathScratch_, pathScratch2_)) {
    error = InstallError::RENAME_FAILED;
    return false;
  }
  // The rename is the removal commit point. Recursive cleanup may be
  // interrupted or fail after deleting some files, so never restore this
  // hidden directory as an installed package.
  storage_.removeTree(storage_.context, pathScratch2_);
  error = InstallError::NONE;
  return true;
}

const char* installErrorName(const InstallError error) {
  switch (error) {
    case InstallError::NONE:
      return "none";
    case InstallError::INVALID_INPUT:
      return "invalid-input";
    case InstallError::STORAGE_UNAVAILABLE:
      return "storage-unavailable";
    case InstallError::DIRECTORY_FAILED:
      return "directory-failed";
    case InstallError::PATH_TOO_LONG:
      return "path-too-long";
    case InstallError::STAGING_MISSING:
      return "staging-missing";
    case InstallError::PACKAGE_MISSING:
      return "package-missing";
    case InstallError::REQUIRED_FILE_MISSING:
      return "required-file-missing";
    case InstallError::LICENSE_INVALID:
      return "license-invalid";
    case InstallError::PACKAGE_INVALID:
      return "package-invalid";
    case InstallError::UUID_MISMATCH:
      return "uuid-mismatch";
    case InstallError::CRC_MISMATCH:
      return "crc-mismatch";
    case InstallError::RENAME_FAILED:
      return "rename-failed";
    case InstallError::REMOVE_FAILED:
      return "remove-failed";
  }
  return "unknown";
}

}  // namespace dictionary::installer

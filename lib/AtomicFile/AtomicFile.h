#pragma once

#include <cstddef>
#include <cstdint>

class HalFile;

namespace AtomicFile {

struct Paths {
  const char* finalPath;
  const char* tempPath;
  const char* backupPath;
};

enum class ValidationResult : uint8_t {
  Valid,
  Invalid,
  Unsupported,
};

using WriteCallback = bool (*)(HalFile& file, const void* context);
using ValidateCallback = ValidationResult (*)(const char* path, const void* context);

// Restores a usable authoritative file after an interrupted replacement.
// When both an old backup and a complete temp exist, recovery conservatively
// restores the old backup. The validator is required. Unsupported candidates
// are preserved without mutation. Returns false if no valid candidate can be restored.
bool recover(const char* moduleName, const Paths& paths, ValidateCallback validator, const void* context = nullptr);

// Writes and synchronizes a temp file, validates it, then promotes it while
// retaining the previous final as a backup. The callback must check every
// write it performs. The validator is required, and all three paths must share
// one directory so promotion never crosses FAT directories. No authoritative
// file is opened with O_TRUNC. A newer unsupported final or sidecar blocks the
// write so data from a future firmware version is never discarded.
bool write(const char* moduleName, const Paths& paths, WriteCallback writer, ValidateCallback validator,
           const void* context = nullptr);

// Removes temp and backup before the final so an interrupted deletion cannot
// resurrect old state. Missing files are treated as success.
bool remove(const char* moduleName, const Paths& paths);

// Copies a canonical filename to output, stripping one recognized atomic
// sidecar suffix (.tmp or .bak). requiredSuffix may be null or empty.
bool canonicalName(const char* entryName, const char* requiredSuffix, char* output, size_t outputSize, bool& isSidecar);

}  // namespace AtomicFile

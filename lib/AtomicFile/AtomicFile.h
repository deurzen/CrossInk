#pragma once

class HalFile;

namespace AtomicFile {

struct Paths {
  const char* finalPath;
  const char* tempPath;
  const char* backupPath;
};

using WriteCallback = bool (*)(HalFile& file, const void* context);
using ValidateCallback = bool (*)(const char* path, const void* context);

// Restores a usable authoritative file after an interrupted replacement.
// When both an old backup and a complete temp exist, recovery conservatively
// restores the old backup. The validator is required. Returns false if no valid
// candidate can be restored.
bool recover(const char* moduleName, const Paths& paths, ValidateCallback validator, const void* context = nullptr);

// Writes and synchronizes a temp file, validates it, then promotes it while
// retaining the previous final as a backup. The callback must check every
// write it performs. The validator is required, and all three paths must share
// one directory so promotion never crosses FAT directories. No authoritative
// file is opened with O_TRUNC.
bool write(const char* moduleName, const Paths& paths, WriteCallback writer, ValidateCallback validator,
           const void* context = nullptr);

// Removes temp and backup before the final so an interrupted deletion cannot
// resurrect old state. Missing files are treated as success.
bool remove(const char* moduleName, const Paths& paths);

}  // namespace AtomicFile

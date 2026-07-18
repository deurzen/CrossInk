#pragma once

#include <DictionaryInstaller.h>

#include <cstddef>
#include <cstdint>

namespace dictionary::storage {

inline constexpr char CANONICAL_ROOT_PATH[] = "/.crosspoint/lexicons";
inline constexpr char DEFINITION_SOURCE_ROOT_PATH[] = "/.crosspoint/definition-sources";
inline constexpr size_t MAX_INSTALLED_BUNDLES = 64;

installer::StorageBackend backend();

bool parseUuid(const char* text, uint8_t (&uuid)[16]);
void formatUuid(const uint8_t (&uuid)[16], char (&output)[37]);

// Collects final and recoverable-backup UUIDs without retaining open directory
// handles. Output is a flat capacity*16-byte caller-owned array.
bool collectPackageUuids(const char* rootPath, uint8_t* output, size_t capacity, size_t& count);

}  // namespace dictionary::storage

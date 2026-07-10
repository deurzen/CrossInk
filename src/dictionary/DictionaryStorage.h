#pragma once

#include <DictionaryInstaller.h>

#include <cstddef>
#include <cstdint>

namespace dictionary::storage {

inline constexpr char ROOT_PATH[] = "/.crosspoint/dictionaries";
inline constexpr size_t MAX_INSTALLED_BUNDLES = 64;

installer::StorageBackend backend();

bool parseUuid(const char* text, uint8_t (&uuid)[16]);
void formatUuid(const uint8_t (&uuid)[16], char (&output)[37]);

// Collects final and recoverable-backup UUIDs without retaining open directory
// handles. Output is a flat capacity*16-byte caller-owned array.
bool collectBundleUuids(uint8_t* output, size_t capacity, size_t& count);

}  // namespace dictionary::storage

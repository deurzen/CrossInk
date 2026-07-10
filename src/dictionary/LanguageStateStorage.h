#pragma once

#include <LexemeStateStore.h>

namespace dictionary::language_state_storage {

inline constexpr char ROOT_PATH[] = "/.crosspoint/language-state";

// HAL-backed callbacks for LexemeStateStore. Each operation opens only one
// file, preserving the hardware SD single-reader constraint.
lexeme_state::StorageBackend backend();

}  // namespace dictionary::language_state_storage

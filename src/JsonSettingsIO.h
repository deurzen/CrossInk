#pragma once

#include <cstdint>

class CrossPointSettings;
class CrossPointState;

namespace JsonSettingsIO {

enum class LoadResult : uint8_t {
  Loaded,
  Missing,
  Failed,
};

// CrossPointSettings
bool saveSettings(const CrossPointSettings& s, const char* path);
LoadResult loadSettings(CrossPointSettings& s, const char* path, bool* needsResave = nullptr);

// CrossPointState
bool saveState(const CrossPointState& s, const char* path);
LoadResult loadState(CrossPointState& s, const char* path);

}  // namespace JsonSettingsIO

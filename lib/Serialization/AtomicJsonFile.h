#pragma once

#include <ArduinoJson.h>

#include <cstdint>

namespace AtomicJsonFile {

enum class ReadResult : uint8_t {
  Loaded,
  Missing,
  Failed,
};

// Atomically serializes a JSON document and validates it before promotion.
bool write(const char* moduleName, const char* path, const JsonDocument& document);

// Recovers an interrupted transaction, then parses the authoritative document.
ReadResult read(const char* moduleName, const char* path, JsonDocument& document);

}  // namespace AtomicJsonFile

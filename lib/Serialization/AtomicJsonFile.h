#pragma once

#include <ArduinoJson.h>

namespace AtomicJsonFile {

// Atomically serializes a JSON document and validates it before promotion.
bool write(const char* moduleName, const char* path, const JsonDocument& document);

// Recovers an interrupted transaction, then parses the authoritative document.
bool read(const char* moduleName, const char* path, JsonDocument& document);

}  // namespace AtomicJsonFile

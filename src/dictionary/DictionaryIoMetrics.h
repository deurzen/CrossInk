#pragma once

#include <cstddef>
#include <cstdint>

#if defined(ARDUINO) && !defined(SIMULATOR)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace dictionary::io_metrics {

inline uint32_t currentTaskStackHighWaterBytes() {
#if defined(ARDUINO) && !defined(SIMULATOR)
  // ESP-IDF reports this value in bytes, unlike standard FreeRTOS ports.
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
#else
  return 0;
#endif
}

// Per-session counters for locating SD access amplification. They are retained
// inside the heap-owned lookup session, so instrumentation adds no static DRAM.
struct Counters {
  uint32_t metadataCalls = 0;
  uint32_t openAttempts = 0;
  uint32_t sourceSwitches = 0;
  uint32_t seekAttempts = 0;
  uint32_t readCalls = 0;
  uint32_t writeCalls = 0;
  uint32_t syncCalls = 0;
  uint64_t bytesRead = 0;
  uint64_t bytesWritten = 0;
  uint32_t lastSourceToken = 0;

  void reset() { *this = {}; }

  void noteSource(uint32_t token) {
    if (token == 0) return;
    if (lastSourceToken != 0 && lastSourceToken != token) ++sourceSwitches;
    lastSourceToken = token;
  }
};

inline Counters difference(const Counters& after, const Counters& before) {
  Counters result;
  result.metadataCalls = after.metadataCalls - before.metadataCalls;
  result.openAttempts = after.openAttempts - before.openAttempts;
  result.sourceSwitches = after.sourceSwitches - before.sourceSwitches;
  result.seekAttempts = after.seekAttempts - before.seekAttempts;
  result.readCalls = after.readCalls - before.readCalls;
  result.writeCalls = after.writeCalls - before.writeCalls;
  result.syncCalls = after.syncCalls - before.syncCalls;
  result.bytesRead = after.bytesRead - before.bytesRead;
  result.bytesWritten = after.bytesWritten - before.bytesWritten;
  return result;
}

}  // namespace dictionary::io_metrics

#pragma once

#include <cstddef>
#include <cstdint>

#include "DictionaryIoMetrics.h"

namespace dictionary::io {

// Reuses exactly one read handle and closes it before changing sources. File is
// a small HAL-compatible type; the template keeps the switching policy host
// testable without exposing SDK file classes to library code.
template <typename File>
class SwitchingFileReader {
 public:
  using OpenForRead = bool (*)(void* context, const char* path, File& file);

  SwitchingFileReader(void* context, OpenForRead openForRead) : context_(context), openForRead_(openForRead) {}
  ~SwitchingFileReader() { close(); }

  SwitchingFileReader(const SwitchingFileReader&) = delete;
  SwitchingFileReader& operator=(const SwitchingFileReader&) = delete;

  bool fileSize(const char* path, uint8_t sourceToken, io_metrics::Counters* metrics, uint64_t& size) {
    size = 0;
    if (!select(path, sourceToken, metrics)) return false;
    size = file_.fileSize64();
    return true;
  }

  bool readAt(const char* path, uint8_t sourceToken, uint64_t sourceSize, uint32_t offset, void* output, size_t length,
              io_metrics::Counters* metrics) {
    if (!output || static_cast<uint64_t>(offset) + length > sourceSize || !select(path, sourceToken, metrics)) {
      return false;
    }
    if (metrics) ++metrics->seekAttempts;
    if (!file_.seek(offset)) return false;
    if (metrics) ++metrics->readCalls;
    const int bytesRead = file_.read(output, length);
    if (bytesRead > 0 && metrics) metrics->bytesRead += static_cast<uint32_t>(bytesRead);
    return bytesRead == static_cast<int>(length);
  }

  void close() {
    if (file_.isOpen()) file_.close();
    activeSourceToken_ = 0;
  }

  uint8_t activeSourceToken() const { return activeSourceToken_; }

 private:
  bool select(const char* path, uint8_t sourceToken, io_metrics::Counters* metrics) {
    if (!path || path[0] == '\0' || sourceToken == 0 || !openForRead_) return false;
    if (metrics) metrics->noteSource(sourceToken);
    if (activeSourceToken_ == sourceToken && file_.isOpen()) return true;

    close();
    if (metrics) ++metrics->openAttempts;
    if (!openForRead_(context_, path, file_)) return false;
    activeSourceToken_ = sourceToken;
    return true;
  }

  void* context_ = nullptr;
  OpenForRead openForRead_ = nullptr;
  File file_{};
  uint8_t activeSourceToken_ = 0;
};

}  // namespace dictionary::io

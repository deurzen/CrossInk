#pragma once

#include <AtomicFile.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <string>

namespace EpubReaderUtils {

struct Progress {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  bool hasPageCount = false;
};

namespace detail {

struct ProgressWriteContext {
  uint16_t spineIndex;
  uint16_t pageNumber;
  uint16_t pageCount;
};

inline AtomicFile::ValidationResult validateProgressFile(const char* path, const void*) {
  HalFile file;
  if (!Storage.openFileForRead("ERS", path, file)) return AtomicFile::ValidationResult::Invalid;
  const uint64_t size = file.fileSize64();
  file.close();
  return size == 4 || size == 6 ? AtomicFile::ValidationResult::Valid : AtomicFile::ValidationResult::Invalid;
}

inline bool writeProgressFile(HalFile& file, const void* context) {
  const auto* progress = static_cast<const ProgressWriteContext*>(context);
  const uint8_t data[6] = {
      static_cast<uint8_t>(progress->spineIndex), static_cast<uint8_t>(progress->spineIndex >> 8),
      static_cast<uint8_t>(progress->pageNumber), static_cast<uint8_t>(progress->pageNumber >> 8),
      static_cast<uint8_t>(progress->pageCount),  static_cast<uint8_t>(progress->pageCount >> 8),
  };
  const size_t written = file.write(data, sizeof(data));
  if (written == sizeof(data)) return true;
  LOG_ERR("ERS", "Short write saving progress: %u/%u bytes", static_cast<unsigned>(written),
          static_cast<unsigned>(sizeof(data)));
  return false;
}

}  // namespace detail

inline bool readProgressFile(const char* moduleName, const std::string& path, Progress& progress) {
  if (!Storage.exists(path.c_str())) {
    return false;
  }

  FsFile f;
  if (!Storage.openFileForRead(moduleName, path, f)) {
    return false;
  }

  uint8_t data[6];
  const int dataSize = f.read(data, sizeof(data));
  f.close();
  if (dataSize != 4 && dataSize != 6) {
    LOG_ERR(moduleName, "Progress file has unexpected size: %d", dataSize);
    return false;
  }

  progress.spineIndex = static_cast<int>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
  progress.pageNumber = static_cast<int>(static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8));
  if (progress.pageNumber == UINT16_MAX) {
    progress.pageNumber = 0;
  }
  if (dataSize == 6) {
    progress.pageCount = static_cast<int>(static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8));
    progress.hasPageCount = true;
  } else {
    progress.pageCount = 0;
    progress.hasPageCount = false;
  }
  return true;
}

inline bool loadProgress(const Epub& epub, Progress& progress, const char* moduleName = "ERS") {
  const std::string progressPath = epub.getCachePath() + "/progress.bin";
  // Cache paths are dynamic, so these cold-path strings cannot use a safely bounded stack buffer.
  const std::string tmpPath = progressPath + ".tmp";
  const std::string backupPath = progressPath + ".bak";
  const AtomicFile::Paths paths{progressPath.c_str(), tmpPath.c_str(), backupPath.c_str()};
  if (!AtomicFile::recover(moduleName, paths, detail::validateProgressFile)) {
    LOG_ERR(moduleName, "Could not recover progress file before load");
  }

  if (readProgressFile(moduleName, progressPath, progress)) {
    return true;
  }

  if (readProgressFile(moduleName, backupPath, progress)) {
    LOG_DBG("ERS", "Recovered progress from backup");
    return true;
  }
  return false;
}

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  const std::string progressPath = epub.getCachePath() + "/progress.bin";
  // Cache paths are dynamic, so these cold-path strings cannot use a safely bounded stack buffer.
  const std::string tmpPath = progressPath + ".tmp";
  const std::string backupPath = progressPath + ".bak";
  const AtomicFile::Paths paths{progressPath.c_str(), tmpPath.c_str(), backupPath.c_str()};
  const detail::ProgressWriteContext context{static_cast<uint16_t>(spineIndex), static_cast<uint16_t>(pageNumber),
                                             static_cast<uint16_t>(pageCount)};
  if (!AtomicFile::write("ERS", paths, detail::writeProgressFile, detail::validateProgressFile, &context)) {
    LOG_ERR("ERS", "Could not atomically save progress file");
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

}  // namespace EpubReaderUtils

#include "BookReadingStats.h"

#include <AtomicFile.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

namespace {
// Binary layout v1 (11 bytes):
//   [0]     version (= 1)
//   [1-2]   sessionCount        uint16_t LE
//   [3-6]   totalReadingSeconds uint32_t LE
//   [7-10]  totalPagesTurned    uint32_t LE
//
// Binary layout v2 (12 bytes):
//   [0]     version (= 2)
//   [1-2]   sessionCount        uint16_t LE
//   [3-6]   totalReadingSeconds uint32_t LE
//   [7-10]  totalPagesTurned    uint32_t LE
//   [11]    isCompleted         uint8_t
//
// Binary layout v3 (16 bytes):
//   [0]      version (= 3)
//   [1-2]    sessionCount              uint16_t LE
//   [3-6]    totalReadingSeconds       uint32_t LE
//   [7-10]   totalPagesTurned          uint32_t LE
//   [11]     isCompleted               uint8_t
//   [12-13]  avgSecondsPerForwardPage  uint16_t LE
//   [14-15]  paceSampleCount           uint16_t LE
//
// Binary layout v4 (69 bytes):
//   [0]      version (= 4)
//   [1-2]    sessionCount              uint16_t LE
//   [3-6]    totalReadingSeconds       uint32_t LE
//   [7-10]   totalPagesTurned          uint32_t LE
//   [11]     isCompleted               uint8_t
//   [12-13]  avgSecondsPerForwardPage  uint16_t LE
//   [14-15]  paceSampleCount           uint16_t LE
//   [16]     flags bit0=startDateManual bit1=finishedDateManual
//   [17-18]  startDate.year            uint16_t LE
//   [19]     startDate.month           uint8_t
//   [20]     startDate.day             uint8_t
//   [21-22]  finishedDate.year         uint16_t LE
//   [23]     finishedDate.month        uint8_t
//   [24]     finishedDate.day          uint8_t
//   [25-40]  timeOfDaySeconds[4]       uint32_t LE each
//   [41-68]  dayOfWeekSeconds[7]       uint32_t LE each
//
// Binary layout v5 (73 bytes):
//   [0-68]   v4 fields
//   [69-72]  estimatedTimeLeftSeconds  uint32_t LE, 0 means unavailable
static constexpr uint8_t STATS_FILE_VERSION = 5;
static constexpr uint8_t STATS_FILE_VERSION_V2 = 2;
static constexpr uint8_t STATS_FILE_VERSION_V1 = 1;
static constexpr uint8_t STATS_FILE_VERSION_V3 = 3;
static constexpr uint8_t STATS_FILE_VERSION_V4 = 4;
static constexpr int STATS_FILE_SIZE_V1 = 11;
static constexpr int STATS_FILE_SIZE_V2 = 12;
static constexpr int STATS_FILE_SIZE_V3 = 16;
static constexpr int STATS_FILE_SIZE_V4 = 69;
static constexpr int STATS_FILE_SIZE = 73;
static constexpr uint16_t MAX_PACE_SAMPLE_COUNT = 1000;
static constexpr uint8_t FLAG_START_DATE_MANUAL = 1u << 0;
static constexpr uint8_t FLAG_FINISHED_DATE_MANUAL = 1u << 1;
static constexpr uint8_t PREVIOUS_VERSIONED_STATS_FILE_VERSION = STATS_FILE_VERSION - 1;
static constexpr const char* LEGACY_STATS_FILE_NAME = "stats.bin";

std::string statsFileNameForVersion(const uint8_t version) {
  char buf[16];
  snprintf(buf, sizeof(buf), "stats_v%u.bin", version);
  return std::string(buf);
}

bool openStatsFileForRead(const std::string& cachePath, FsFile& f) {
  const std::string currentName = statsFileNameForVersion(STATS_FILE_VERSION);
  const std::string currentPath = cachePath + "/" + currentName;
  if (Storage.openFileForRead("STATS", currentPath, f)) {
    return true;
  }
  if (Storage.openFileForRead("STATS", currentPath + ".bak", f)) {
    LOG_DBG("STATS", "Recovered %s from atomic backup", currentName.c_str());
    return true;
  }

  // When bumping STATS_FILE_VERSION, this automatically tries the previous
  // versioned filename (e.g. v6 falls back to stats_v5.bin) before the original
  // unversioned stats.bin migration source.
  const std::string previousName = statsFileNameForVersion(PREVIOUS_VERSIONED_STATS_FILE_VERSION);
  if (Storage.openFileForRead("STATS", cachePath + "/" + previousName, f)) {
    LOG_DBG("STATS", "Migrating %s to %s", previousName.c_str(), currentName.c_str());
    return true;
  }

  if (Storage.openFileForRead("STATS", cachePath + "/" + LEGACY_STATS_FILE_NAME, f)) {
    LOG_DBG("STATS", "Migrating legacy %s to %s", LEGACY_STATS_FILE_NAME, currentName.c_str());
    return true;
  }

  return false;
}

uint16_t readLe16(const uint8_t* data, const int offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t readLe32(const uint8_t* data, const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void readCommonStats(const uint8_t* data, BookReadingStats& stats) {
  stats.sessionCount = readLe16(data, 1);
  stats.totalReadingSeconds = readLe32(data, 3);
  stats.totalPagesTurned = readLe32(data, 7);
}

void writeLe16(uint8_t* data, const int offset, const uint16_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
}

void writeLe32(uint8_t* data, const int offset, const uint32_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
  data[offset + 2] = (value >> 16) & 0xFF;
  data[offset + 3] = (value >> 24) & 0xFF;
}

ReadingStatsDate readDate(const uint8_t* data, const int offset) {
  ReadingStatsDate date;
  date.year = readLe16(data, offset);
  date.month = data[offset + 2];
  date.day = data[offset + 3];
  if (!date.isValid()) {
    date.clear();
  }
  return date;
}

void serializeStats(const BookReadingStats& stats, uint8_t* data) {
  memset(data, 0, STATS_FILE_SIZE);
  data[0] = STATS_FILE_VERSION;
  writeLe16(data, 1, stats.sessionCount);
  writeLe32(data, 3, stats.totalReadingSeconds);
  writeLe32(data, 7, stats.totalPagesTurned);
  data[11] = stats.isCompleted ? 1 : 0;
  writeLe16(data, 12, stats.avgSecondsPerForwardPage);
  writeLe16(data, 14, stats.paceSampleCount);
  data[16] = (stats.startDateManual ? FLAG_START_DATE_MANUAL : 0u) |
             (stats.finishedDateManual ? FLAG_FINISHED_DATE_MANUAL : 0u);
  writeLe16(data, 17, stats.startDate.isValid() ? stats.startDate.year : 0);
  data[19] = stats.startDate.isValid() ? stats.startDate.month : 0;
  data[20] = stats.startDate.isValid() ? stats.startDate.day : 0;
  writeLe16(data, 21, stats.finishedDate.isValid() ? stats.finishedDate.year : 0);
  data[23] = stats.finishedDate.isValid() ? stats.finishedDate.month : 0;
  data[24] = stats.finishedDate.isValid() ? stats.finishedDate.day : 0;
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    writeLe32(data, 25 + static_cast<int>(i) * 4, stats.timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    writeLe32(data, 41 + static_cast<int>(i) * 4, stats.dayOfWeekSeconds[i]);
  }
  writeLe32(data, 69, stats.estimatedTimeLeftSeconds);
}

struct StatsWriteContext {
  const BookReadingStats* stats;
};

bool writeStatsFile(HalFile& file, const void* context) {
  const auto* writeContext = static_cast<const StatsWriteContext*>(context);
  uint8_t data[STATS_FILE_SIZE];
  serializeStats(*writeContext->stats, data);
  const size_t written = file.write(data, sizeof(data));
  if (written == sizeof(data)) return true;
  LOG_ERR("STATS", "Short write saving stats: %u/%u bytes", static_cast<unsigned>(written),
          static_cast<unsigned>(sizeof(data)));
  return false;
}

bool validateStatsFile(const char* path, const void*) {
  HalFile file;
  if (!Storage.openFileForRead("STATS", path, file)) return false;
  uint8_t version = 0;
  const bool valid = file.fileSize64() == STATS_FILE_SIZE &&
                     file.read(&version, sizeof(version)) == static_cast<int>(sizeof(version)) &&
                     version == STATS_FILE_VERSION;
  file.close();
  return valid;
}

}  // namespace

BookReadingStats BookReadingStats::load(const std::string& cachePath) {
  BookReadingStats stats;
  const std::string statsPath = cachePath + "/" + statsFileNameForVersion(STATS_FILE_VERSION);
  // Recover before opening so a missing/corrupt final never hides a valid backup.
  const std::string tempPath = statsPath + ".tmp";
  const std::string backupPath = statsPath + ".bak";
  const AtomicFile::Paths paths{statsPath.c_str(), tempPath.c_str(), backupPath.c_str()};
  if (!AtomicFile::recover("STATS", paths, validateStatsFile)) {
    LOG_ERR("STATS", "Could not recover %s before load", statsPath.c_str());
  }

  FsFile f;
  if (!openStatsFileForRead(cachePath, f)) {
    return stats;
  }
  uint8_t data[STATS_FILE_SIZE] = {};
  const int n = f.read(data, STATS_FILE_SIZE);
  f.close();

  if (n == STATS_FILE_SIZE_V1 && data[0] == STATS_FILE_VERSION_V1) {
    readCommonStats(data, stats);
    return stats;
  }

  if (n == STATS_FILE_SIZE_V2 && data[0] == STATS_FILE_VERSION_V2) {
    readCommonStats(data, stats);
    stats.isCompleted = data[11] != 0;
    return stats;
  }

  if (n == STATS_FILE_SIZE_V3 && data[0] == STATS_FILE_VERSION_V3) {
    readCommonStats(data, stats);
    stats.isCompleted = data[11] != 0;
    stats.avgSecondsPerForwardPage = readLe16(data, 12);
    stats.paceSampleCount = readLe16(data, 14);
    return stats;
  }

  if (n != STATS_FILE_SIZE && n != STATS_FILE_SIZE_V4) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return stats;
  }
  if (n == STATS_FILE_SIZE_V4 && data[0] != STATS_FILE_VERSION_V4) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return stats;
  }
  if (n == STATS_FILE_SIZE && data[0] != STATS_FILE_VERSION) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return stats;
  }
  readCommonStats(data, stats);
  stats.isCompleted = data[11] != 0;
  stats.avgSecondsPerForwardPage = readLe16(data, 12);
  stats.paceSampleCount = readLe16(data, 14);
  const uint8_t flags = data[16];
  stats.startDateManual = (flags & FLAG_START_DATE_MANUAL) != 0;
  stats.finishedDateManual = (flags & FLAG_FINISHED_DATE_MANUAL) != 0;
  stats.startDate = readDate(data, 17);
  stats.finishedDate = readDate(data, 21);
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    stats.timeOfDaySeconds[i] = readLe32(data, 25 + static_cast<int>(i) * 4);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    stats.dayOfWeekSeconds[i] = readLe32(data, 41 + static_cast<int>(i) * 4);
  }
  if (n == STATS_FILE_SIZE) {
    stats.estimatedTimeLeftSeconds = readLe32(data, 69);
  }
  return stats;
}

void BookReadingStats::recordForwardPageRead(uint32_t seconds) {
  if (seconds == 0) {
    return;
  }
  if (seconds > UINT16_MAX) {
    seconds = UINT16_MAX;
  }

  const uint16_t sample = static_cast<uint16_t>(seconds);
  if (paceSampleCount == 0 || avgSecondsPerForwardPage == 0) {
    avgSecondsPerForwardPage = sample;
    paceSampleCount = 1;
    return;
  }

  const uint16_t weight = paceSampleCount < MAX_PACE_SAMPLE_COUNT ? paceSampleCount : MAX_PACE_SAMPLE_COUNT;
  const uint32_t nextAverage =
      (static_cast<uint32_t>(avgSecondsPerForwardPage) * weight + sample) / (static_cast<uint32_t>(weight) + 1U);
  avgSecondsPerForwardPage = static_cast<uint16_t>(nextAverage);
  if (paceSampleCount < MAX_PACE_SAMPLE_COUNT) {
    paceSampleCount++;
  }
}

void BookReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
}

void BookReadingStats::formatDuration(uint32_t seconds, char* buf, size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, "%luh %lu min", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}

void BookReadingStats::save(const std::string& cachePath) const {
  const std::string statsPath = cachePath + "/" + statsFileNameForVersion(STATS_FILE_VERSION);
  // These cold-path strings must outlive AtomicFile::write; cache paths are not safely bounded for stack buffers.
  const std::string tempPath = statsPath + ".tmp";
  const std::string backupPath = statsPath + ".bak";
  const AtomicFile::Paths paths{statsPath.c_str(), tempPath.c_str(), backupPath.c_str()};
  StatsWriteContext context{this};
  if (!AtomicFile::write("STATS", paths, writeStatsFile, validateStatsFile, &context)) {
    LOG_ERR("STATS", "Could not atomically save %s", statsPath.c_str());
  }
}

bool BookReadingStats::remove(const std::string& cachePath) {
  const std::string statsFileName = statsFileNameForVersion(STATS_FILE_VERSION);
  const std::string statsPath = cachePath + "/" + statsFileName;
  bool ok = true;
  if (Storage.exists(statsPath.c_str()) && !Storage.remove(statsPath.c_str())) {
    LOG_ERR("STATS", "Could not delete %s", statsFileName.c_str());
    ok = false;
  }
  const std::string backupPath = statsPath + ".bak";
  if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    LOG_ERR("STATS", "Could not delete %s backup", statsFileName.c_str());
    ok = false;
  }
  const std::string tempPath = statsPath + ".tmp";
  if (Storage.exists(tempPath.c_str()) && !Storage.remove(tempPath.c_str())) {
    LOG_ERR("STATS", "Could not delete %s temp", statsFileName.c_str());
    ok = false;
  }

  const std::string previousStatsFileName = statsFileNameForVersion(PREVIOUS_VERSIONED_STATS_FILE_VERSION);
  const std::string previousStatsPath = cachePath + "/" + previousStatsFileName;
  if (Storage.exists(previousStatsPath.c_str()) && !Storage.remove(previousStatsPath.c_str())) {
    LOG_ERR("STATS", "Could not delete %s", previousStatsFileName.c_str());
    ok = false;
  }

  const std::string legacyStatsPath = cachePath + "/" + LEGACY_STATS_FILE_NAME;
  if (Storage.exists(legacyStatsPath.c_str()) && !Storage.remove(legacyStatsPath.c_str())) {
    LOG_ERR("STATS", "Could not delete %s", LEGACY_STATS_FILE_NAME);
    ok = false;
  }
  return ok;
}

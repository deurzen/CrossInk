#include "ClippingStore.h"

#include <Arduino.h>
#include <AtomicFile.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <uzlib.h>

#include <algorithm>
#include <functional>
#include <limits>

namespace {
constexpr uint8_t VERSION = 1;
constexpr size_t INITIAL_CLIPPING_RESERVE = 4;
constexpr char CLIPPINGS_DIR[] = "/.crosspoint/clippings";
constexpr size_t CLIPPING_STORE_NAME_MAX = 40;

struct ClippingFileHeader {
  std::string title;
  std::string author;
  std::string path;
  std::string bookType;
  uint16_t count = 0;
};

std::string storeFilePathForBook(const std::string& filePath, const std::string& bookType) {
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  return std::string(CLIPPINGS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".bin";
}

void copyBounded(char* dst, const size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  if (!src) src = "";
  snprintf(dst, dstSize, "%s", src);
}

bool skipBytes(HalFile& file, const uint32_t length) {
  if (length > static_cast<uint32_t>(std::numeric_limits<int>::max()) || file.available() < static_cast<int>(length)) {
    return false;
  }
  return length == 0 || file.seekCur(length);
}

bool skipSerializedString(HalFile& file) {
  uint32_t length = 0;
  return serialization::tryReadPod(file, length) && skipBytes(file, length);
}

bool readClippingFileHeader(const std::string& fullPath, const char* name, ClippingFileHeader& header) {
  FsFile f;
  if (!Storage.openFileForRead("CLIP", fullPath, f)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;
  if (!serialization::tryReadPod(f, version) || version != VERSION || !serialization::tryReadPod(f, count) ||
      !serialization::tryReadString(f, header.title) || !serialization::tryReadString(f, header.author) ||
      !serialization::tryReadString(f, header.path)) {
    f.close();
    return false;
  }
  f.close();

  header.count = count;
  header.bookType = "epub";
  const std::string nameStr = name ? name : "";
  const size_t underscorePos = nameStr.find('_');
  if (underscorePos != std::string::npos) {
    header.bookType = nameStr.substr(0, underscorePos);
  }
  return true;
}
}  // namespace

ClippingStore ClippingStore::instance;

bool ClippingStore::writeAtomicFile(HalFile& file, const void* context) {
  const auto* store = static_cast<const ClippingStore*>(context);
  const uint16_t count = static_cast<uint16_t>(std::min<size_t>(store->clippings.size(), CLIPPING_MAX_PER_BOOK));
  if (!serialization::tryWritePod(file, VERSION) || !serialization::tryWritePod(file, count) ||
      !serialization::tryWriteString(file, store->bookTitle) ||
      !serialization::tryWriteString(file, store->bookAuthor) ||
      !serialization::tryWriteString(file, store->bookFilePath)) {
    LOG_ERR("CLIP", "Failed to write clipping header: %s", store->storeFilePath.c_str());
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const Clipping& clipping = store->clippings[i];
    if (!serialization::tryWritePod(file, clipping.spineIndex) ||
        !serialization::tryWritePod(file, clipping.startPage) || !serialization::tryWritePod(file, clipping.endPage) ||
        !serialization::tryWritePod(file, clipping.pageCount) ||
        !serialization::tryWritePod(file, clipping.startWordIndex) ||
        !serialization::tryWritePod(file, clipping.endWordIndex) ||
        !serialization::tryWritePod(file, clipping.wordCount) ||
        !serialization::tryWritePod(file, clipping.paragraphIndex) ||
        !serialization::tryWritePod(file, clipping.timestamp) ||
        file.write(reinterpret_cast<const uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
            sizeof(clipping.chapterTitle) ||
        !serialization::tryWriteString(file, clipping.text)) {
      LOG_ERR("CLIP", "Failed to write clipping record %u: %s", i, store->storeFilePath.c_str());
      return false;
    }
  }
  return true;
}

AtomicFile::ValidationResult ClippingStore::validateAtomicFile(const char* path, const void*) {
  HalFile file;
  if (!Storage.openFileForRead("CLIP", path, file)) return AtomicFile::ValidationResult::Invalid;

  uint8_t version = 0;
  if (!serialization::tryReadPod(file, version)) {
    file.close();
    return AtomicFile::ValidationResult::Invalid;
  }
  if (version > VERSION) {
    file.close();
    return AtomicFile::ValidationResult::Unsupported;
  }

  uint16_t count = 0;
  bool valid = version == VERSION && serialization::tryReadPod(file, count) && count <= CLIPPING_MAX_PER_BOOK &&
               skipSerializedString(file) && skipSerializedString(file) && skipSerializedString(file);

  constexpr uint32_t fixedRecordSize = sizeof(uint16_t) * 8 + sizeof(uint32_t) + CLIPPING_CHAPTER_TITLE_MAX;
  for (uint16_t i = 0; valid && i < count; ++i) {
    valid = skipBytes(file, fixedRecordSize) && skipSerializedString(file);
  }
  valid = valid && file.available() == 0;
  file.close();
  return valid ? AtomicFile::ValidationResult::Valid : AtomicFile::ValidationResult::Invalid;
}

bool ClippingStore::recoverAtomicFile(const std::string& path) {
  // Store paths are dynamic, so sidecar paths cannot use safely bounded stack buffers.
  const std::string tempPath = path + ".tmp";
  const std::string backupPath = path + ".bak";
  const AtomicFile::Paths paths{path.c_str(), tempPath.c_str(), backupPath.c_str()};
  return AtomicFile::recover("CLIP", paths, validateAtomicFile);
}

bool ClippingStore::removeAtomicFile(const std::string& path) {
  const std::string tempPath = path + ".tmp";
  const std::string backupPath = path + ".bak";
  const AtomicFile::Paths paths{path.c_str(), tempPath.c_str(), backupPath.c_str()};
  return AtomicFile::remove("CLIP", paths);
}

bool ClippingStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  if (bookType != "epub") {
    LOG_ERR("CLIP", "Unknown clipping book type: %s", bookType.c_str());
    return false;
  }

  bookFilePath = filePath;
  bookTitle = title;
  bookAuthor = author;
  dirty = false;
  clippings.clear();
  if (clippings.capacity() < INITIAL_CLIPPING_RESERVE) {
    clippings.reserve(INITIAL_CLIPPING_RESERVE);
  }

  storeFilePath = storeFilePathForBook(filePath, bookType);
  if (!recoverAtomicFile(storeFilePath)) {
    LOG_ERR("CLIP", "Failed to recover clipping store: %s", storeFilePath.c_str());
  }
  if (!Storage.exists(storeFilePath.c_str())) {
    return true;
  }

  return readFromFile();
}

void ClippingStore::unload() {
  if (dirty) saveToFile();
  clippings.clear();
  bookFilePath.clear();
  bookTitle.clear();
  bookAuthor.clear();
  storeFilePath.clear();
  dirty = false;
}

ClippingStore::AddResult ClippingStore::addClipping(const uint16_t spineIndex, const uint16_t startPage,
                                                    const uint16_t endPage, const uint16_t pageCount,
                                                    const uint16_t startWordIndex, const uint16_t endWordIndex,
                                                    const uint16_t wordCount, const char* chapterTitle,
                                                    const uint16_t paragraphIndex, const std::string& text) {
  if (clippings.size() >= CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping limit (%u) reached", CLIPPING_MAX_PER_BOOK);
    return AddResult::LimitReached;
  }

  Clipping clipping;
  clipping.spineIndex = spineIndex;
  clipping.startPage = startPage;
  clipping.endPage = endPage;
  clipping.pageCount = std::max<uint16_t>(1, pageCount);
  clipping.startWordIndex = startWordIndex;
  clipping.endWordIndex = endWordIndex;
  clipping.wordCount = wordCount;
  clipping.paragraphIndex = paragraphIndex;
  clipping.timestamp = static_cast<uint32_t>(millis() / 1000UL);
  copyBounded(clipping.chapterTitle, sizeof(clipping.chapterTitle), chapterTitle);
  // Keep the in-app store bounded. The full Kindle-style export is still written separately.
  clipping.text.assign(text.data(), std::min(text.size(), CLIPPING_TEXT_MAX));

  clippings.push_back(std::move(clipping));
  dirty = true;
  if (!saveToFile()) {
    clippings.pop_back();
    dirty = true;
    return AddResult::SaveFailed;
  }
  return AddResult::Added;
}

bool ClippingStore::removeClippingAt(const size_t index) {
  if (index >= clippings.size()) return false;
  Clipping clipping = std::move(clippings[index]);
  clippings.erase(clippings.begin() + index);
  dirty = true;
  if (!saveToFile()) {
    clippings.insert(clippings.begin() + index, std::move(clipping));
    dirty = true;
    return false;
  }
  return true;
}

bool ClippingStore::hasClippingForPage(const uint16_t spineIndex, const uint16_t page) const {
  return std::any_of(clippings.begin(), clippings.end(), [&](const Clipping& clipping) {
    return clipping.spineIndex == spineIndex && page >= clipping.startPage && page <= clipping.endPage;
  });
}

bool ClippingStore::saveToFile() {
  if (!dirty) return true;
  if (writeToFile()) {
    dirty = false;
    return true;
  }
  return false;
}

void ClippingStore::clearAll() {
  if (!storeFilePath.empty() && !removeAtomicFile(storeFilePath)) {
    LOG_ERR("CLIP", "Failed to delete clipping store: %s", storeFilePath.c_str());
    return;
  }
  clippings.clear();
  dirty = false;
}

bool ClippingStore::readFromFile() { return readFromFile(storeFilePath, clippings); }

bool ClippingStore::readFromFile(const std::string& path, std::vector<Clipping>& out) const {
  out.clear();
  FsFile f;
  if (!Storage.openFileForRead("CLIP", path, f)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;
  std::string title;
  std::string author;
  std::string storedPath;
  if (!serialization::tryReadPod(f, version) || version != VERSION || !serialization::tryReadPod(f, count) ||
      !serialization::tryReadString(f, title) || !serialization::tryReadString(f, author) ||
      !serialization::tryReadString(f, storedPath)) {
    f.close();
    LOG_ERR("CLIP", "Failed to read clipping header: %s", path.c_str());
    return false;
  }

  if (count > CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping count %u exceeds max, file may be corrupt: %s", count, path.c_str());
    f.close();
    return false;
  }

  out.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    Clipping clipping;
    if (!serialization::tryReadPod(f, clipping.spineIndex) || !serialization::tryReadPod(f, clipping.startPage) ||
        !serialization::tryReadPod(f, clipping.endPage) || !serialization::tryReadPod(f, clipping.pageCount) ||
        !serialization::tryReadPod(f, clipping.startWordIndex) ||
        !serialization::tryReadPod(f, clipping.endWordIndex) || !serialization::tryReadPod(f, clipping.wordCount) ||
        !serialization::tryReadPod(f, clipping.paragraphIndex) || !serialization::tryReadPod(f, clipping.timestamp)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at record %u: %s", i, path.c_str());
      return false;
    }
    if (f.read(reinterpret_cast<uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
        sizeof(clipping.chapterTitle)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at chapter title, record %u: %s", i, path.c_str());
      return false;
    }
    clipping.chapterTitle[sizeof(clipping.chapterTitle) - 1] = '\0';
    if (!serialization::tryReadString(f, clipping.text)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at text, record %u: %s", i, path.c_str());
      return false;
    }
    if (clipping.text.size() > CLIPPING_TEXT_MAX) {
      clipping.text.resize(CLIPPING_TEXT_MAX);
    }
    out.push_back(std::move(clipping));
  }

  f.close();
  return true;
}

bool ClippingStore::writeToFile() const {
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CLIPPINGS_DIR);

  // Store paths are dynamic, so sidecar paths cannot use safely bounded stack buffers.
  const std::string tempPath = storeFilePath + ".tmp";
  const std::string backupPath = storeFilePath + ".bak";
  const AtomicFile::Paths paths{storeFilePath.c_str(), tempPath.c_str(), backupPath.c_str()};
  return AtomicFile::write("CLIP", paths, writeAtomicFile, validateAtomicFile, this);
}

bool ClippingStore::hasAnyClippings() {
  if (!Storage.exists(CLIPPINGS_DIR)) return false;
  const auto files = Storage.listFiles(CLIPPINGS_DIR);
  return std::any_of(files.begin(), files.end(), [](const auto& entry) {
    char canonicalName[CLIPPING_STORE_NAME_MAX];
    bool isSidecar = false;
    if (!AtomicFile::canonicalName(entry.c_str(), ".bin", canonicalName, sizeof(canonicalName), isSidecar))
      return false;
    char canonicalPath[sizeof(CLIPPINGS_DIR) + CLIPPING_STORE_NAME_MAX + 1];
    snprintf(canonicalPath, sizeof(canonicalPath), "%s/%s", CLIPPINGS_DIR, canonicalName);
    if (isSidecar && !recoverAtomicFile(canonicalPath)) return false;
    return Storage.exists(canonicalPath);
  });
}

bool ClippingStore::getAllClippedBooks(std::vector<ClippedBookEntry>& out) {
  if (!Storage.exists(CLIPPINGS_DIR)) return true;

  const auto files = Storage.listFiles(CLIPPINGS_DIR);
  for (const auto& name : files) {
    char canonicalName[CLIPPING_STORE_NAME_MAX];
    bool isSidecar = false;
    if (!AtomicFile::canonicalName(name.c_str(), ".bin", canonicalName, sizeof(canonicalName), isSidecar)) continue;
    const std::string fullPath = std::string(CLIPPINGS_DIR) + "/" + canonicalName;
    if (isSidecar && !recoverAtomicFile(fullPath)) continue;
    if (!Storage.exists(fullPath.c_str())) continue;

    ClippingFileHeader header;
    if (!readClippingFileHeader(fullPath, canonicalName, header)) continue;
    if (header.path.empty() || header.count == 0 || !Storage.exists(header.path.c_str())) continue;

    auto existing = std::find_if(out.begin(), out.end(), [&](const ClippedBookEntry& entry) {
      return entry.bookPath == header.path && entry.bookType == header.bookType;
    });
    if (existing != out.end()) {
      existing->count = std::max(existing->count, header.count);
      continue;
    }
    out.push_back({std::move(header.title), std::move(header.author), std::move(header.path),
                   std::move(header.bookType), header.count});
  }
  return true;
}

void ClippingStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  const std::string path = storeFilePathForBook(filePath, bookType);
  if (!removeAtomicFile(path)) {
    LOG_ERR("CLIP", "Failed to delete clipping store for: %s", filePath.c_str());
  }
}

bool ClippingStore::migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                       const std::string& title, const std::string& author,
                                       const std::string& bookType) {
  const std::string oldStorePath = storeFilePathForBook(oldFilePath, bookType);
  if (!recoverAtomicFile(oldStorePath)) {
    LOG_ERR("CLIP", "Failed to recover clipping store before migration: %s", oldStorePath.c_str());
    return false;
  }
  if (!Storage.exists(oldStorePath.c_str())) {
    return true;
  }

  ClippingStore reader;
  std::vector<Clipping> migratedClippings;
  if (!reader.readFromFile(oldStorePath, migratedClippings)) {
    return false;
  }

  ClippingStore writer;
  writer.bookFilePath = newFilePath;
  writer.bookTitle = title;
  writer.bookAuthor = author;
  writer.storeFilePath = storeFilePathForBook(newFilePath, bookType);
  writer.clippings = std::move(migratedClippings);
  if (!writer.writeToFile()) {
    return false;
  }

  if (!removeAtomicFile(oldStorePath)) {
    LOG_ERR("CLIP", "Failed to remove old clipping store after migration: %s", oldStorePath.c_str());
    return false;
  }
  return true;
}

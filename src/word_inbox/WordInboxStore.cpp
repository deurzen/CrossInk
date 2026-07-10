#include "WordInboxStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <uzlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>

#include "util/ScreenshotUtil.h"

namespace {

constexpr char ROOT_DIR[] = "/.crosspoint/word_inbox";
constexpr char BOOK_MAGIC[] = "WIBK";
constexpr char CONTEXT_MAGIC[] = "WICT";
constexpr uint8_t FORMAT_VERSION = 1;
constexpr uint8_t FLAG_TEXT_AVAILABLE = 1U << 0;
constexpr uint8_t FLAG_TEXT_TRUNCATED = 1U << 1;
constexpr uint8_t FLAG_SCREENSHOT_AVAILABLE = 1U << 2;
constexpr size_t PATH_CAPACITY = 96;
constexpr uint32_t MAX_CAPTURE_ID = 99999999;
constexpr uint32_t MAX_TITLE_BYTES = 512;
constexpr uint32_t MAX_AUTHOR_BYTES = 512;
constexpr uint32_t MAX_BOOK_PATH_BYTES = 1024;
constexpr uint32_t MAX_CHAPTER_BYTES = 512;
constexpr uint32_t MAX_TEXT_BYTES = 8192;

const char* bookTypeName(const WordInboxBookType type) {
  switch (type) {
    case WordInboxBookType::Epub:
      return "epub";
    case WordInboxBookType::Txt:
      return "txt";
    case WordInboxBookType::Xtc:
      return "xtc";
  }
  return nullptr;
}

bool writeBytes(HalFile& file, const void* data, const size_t length) {
  return length == 0 || file.write(data, length) == length;
}

template <typename T>
bool writePod(HalFile& file, const T& value) {
  return writeBytes(file, &value, sizeof(value));
}

template <typename T>
bool readPod(HalFile& file, T& value) {
  return file.read(&value, sizeof(value)) == static_cast<int>(sizeof(value));
}

bool readString(HalFile& file, std::string& value, const uint32_t maximumLength) {
  uint32_t length = 0;
  if (!readPod(file, length) || length > maximumLength) return false;
  value.resize(length);
  return length == 0 || file.read(value.data(), length) == static_cast<int>(length);
}

bool writeString(HalFile& file, const std::string_view value, const uint32_t maximumLength) {
  if (value.size() > maximumLength || value.size() > std::numeric_limits<uint32_t>::max()) return false;
  const uint32_t length = static_cast<uint32_t>(value.size());
  return writePod(file, length) && writeBytes(file, value.data(), length);
}

bool skipString(HalFile& file, const uint32_t maximumLength) {
  uint32_t length = 0;
  return readPod(file, length) && length <= maximumLength && file.seekCur(length);
}

enum class StringComparison : uint8_t {
  Match,
  Mismatch,
  ReadError,
};

StringComparison compareString(HalFile& file, const std::string_view expected, const uint32_t maximumLength) {
  uint32_t length = 0;
  if (!readPod(file, length) || length > maximumLength) return StringComparison::ReadError;
  if (length != expected.size()) return StringComparison::Mismatch;

  char buffer[64];
  size_t offset = 0;
  while (offset < expected.size()) {
    const size_t chunk = std::min(sizeof(buffer), expected.size() - offset);
    if (file.read(buffer, chunk) != static_cast<int>(chunk)) return StringComparison::ReadError;
    if (std::memcmp(buffer, expected.data() + offset, chunk) != 0) return StringComparison::Mismatch;
    offset += chunk;
  }
  return StringComparison::Match;
}

bool isValidBookKeyInternal(const std::string_view key) {
  size_t prefixLength = 0;
  if (key.size() > 5 && key.substr(0, 5) == "epub_") {
    prefixLength = 5;
  } else if (key.size() > 4 && (key.substr(0, 4) == "txt_" || key.substr(0, 4) == "xtc_")) {
    prefixLength = 4;
  } else {
    return false;
  }

  if (key.size() - prefixLength > 10) return false;
  for (size_t index = prefixLength; index < key.size(); ++index) {
    if (key[index] < '0' || key[index] > '9') return false;
  }
  return true;
}

WordInboxBookType bookTypeFromKey(const std::string_view key) {
  return key.substr(0, 5) == "epub_"  ? WordInboxBookType::Epub
         : key.substr(0, 4) == "txt_" ? WordInboxBookType::Txt
                                      : WordInboxBookType::Xtc;
}

bool buildDirectoryForKey(const std::string_view key, char* const output, const size_t outputSize) {
  if (!isValidBookKeyInternal(key)) return false;
  const int written = snprintf(output, outputSize, "%s/%.*s", ROOT_DIR, static_cast<int>(key.size()), key.data());
  return written > 0 && static_cast<size_t>(written) < outputSize;
}

bool buildBookDirectory(const WordInboxCapture& capture, char* const output, const size_t outputSize) {
  const char* typeName = bookTypeName(capture.bookType);
  if (!typeName || capture.bookPath.empty() || capture.bookPath.size() > MAX_BOOK_PATH_BYTES ||
      capture.bookPath.size() > std::numeric_limits<unsigned int>::max()) {
    return false;
  }
  const uint32_t crc = uzlib_crc32(capture.bookPath.data(), static_cast<unsigned int>(capture.bookPath.size()), 0);
  const int written = snprintf(output, outputSize, "%s/%s_%lu", ROOT_DIR, typeName, static_cast<unsigned long>(crc));
  return written > 0 && static_cast<size_t>(written) < outputSize;
}

bool buildBookMetadataPath(const char* const directory, const bool temporary, char* const output,
                           const size_t outputSize) {
  const int written = snprintf(output, outputSize, "%s/book.bin%s", directory, temporary ? ".tmp" : "");
  return written > 0 && static_cast<size_t>(written) < outputSize;
}

bool buildCapturePath(const char* const directory, const uint32_t id, const char* const extension, const bool temporary,
                      char* const output, const size_t outputSize) {
  const int written = snprintf(output, outputSize, "%s/%08lu.%s%s", directory, static_cast<unsigned long>(id),
                               extension, temporary ? ".tmp" : "");
  return written > 0 && static_cast<size_t>(written) < outputSize;
}

enum class BookValidation : uint8_t {
  Valid,
  Invalid,
  PathMismatch,
};

BookValidation validateExistingBook(const char* const path, const WordInboxCapture& capture) {
  HalFile file;
  if (!Storage.openFileForRead("WIN", path, file)) return BookValidation::Invalid;

  char magic[sizeof(BOOK_MAGIC) - 1];
  uint8_t version = 0;
  uint8_t type = 0;
  if (file.read(magic, sizeof(magic)) != static_cast<int>(sizeof(magic)) ||
      std::memcmp(magic, BOOK_MAGIC, sizeof(magic)) != 0 || !readPod(file, version) || version != FORMAT_VERSION ||
      !readPod(file, type) || type != static_cast<uint8_t>(capture.bookType) || !skipString(file, MAX_TITLE_BYTES) ||
      !skipString(file, MAX_AUTHOR_BYTES)) {
    return BookValidation::Invalid;
  }

  const StringComparison pathComparison = compareString(file, capture.bookPath, MAX_BOOK_PATH_BYTES);
  if (pathComparison == StringComparison::Match) return BookValidation::Valid;
  return pathComparison == StringComparison::Mismatch ? BookValidation::PathMismatch : BookValidation::Invalid;
}

WordInboxSaveResult ensureBookMetadata(const char* const directory, const WordInboxCapture& capture) {
  char finalPath[PATH_CAPACITY];
  if (!buildBookMetadataPath(directory, false, finalPath, sizeof(finalPath))) {
    return WordInboxSaveResult::StorageError;
  }
  if (Storage.exists(finalPath)) {
    const BookValidation validation = validateExistingBook(finalPath, capture);
    if (validation == BookValidation::Valid) return WordInboxSaveResult::Saved;
    if (validation == BookValidation::PathMismatch) {
      LOG_ERR("WIN", "Book path hash collision: %s", finalPath);
      return WordInboxSaveResult::HashCollision;
    }
    LOG_ERR("WIN", "Invalid book metadata: %s", finalPath);
    return WordInboxSaveResult::StorageError;
  }

  char temporaryPath[PATH_CAPACITY];
  if (!buildBookMetadataPath(directory, true, temporaryPath, sizeof(temporaryPath))) {
    return WordInboxSaveResult::StorageError;
  }
  if (Storage.exists(temporaryPath)) Storage.remove(temporaryPath);

  HalFile file;
  if (!Storage.openFileForWrite("WIN", temporaryPath, file)) {
    LOG_ERR("WIN", "Failed to create book metadata: %s", temporaryPath);
    return WordInboxSaveResult::StorageError;
  }
  const bool written = writeBytes(file, BOOK_MAGIC, sizeof(BOOK_MAGIC) - 1) && writePod(file, FORMAT_VERSION) &&
                       writePod(file, static_cast<uint8_t>(capture.bookType)) &&
                       writeString(file, capture.title, MAX_TITLE_BYTES) &&
                       writeString(file, capture.author, MAX_AUTHOR_BYTES) &&
                       writeString(file, capture.bookPath, MAX_BOOK_PATH_BYTES) && file.sync();
  file.close();
  if (!written || !Storage.rename(temporaryPath, finalPath)) {
    LOG_ERR("WIN", "Failed to commit book metadata: %s", finalPath);
    Storage.remove(temporaryPath);
    return WordInboxSaveResult::StorageError;
  }
  return WordInboxSaveResult::Saved;
}

bool parseContextId(const char* name, uint32_t& id) {
  if (!name) return false;
  const char* baseName = std::strrchr(name, '/');
  baseName = baseName ? baseName + 1 : name;
  if (std::strlen(baseName) != 12 || std::strcmp(baseName + 8, ".ctx") != 0) return false;

  uint32_t value = 0;
  for (size_t index = 0; index < 8; ++index) {
    const auto valueByte = static_cast<unsigned char>(baseName[index]);
    if (!std::isdigit(valueByte)) return false;
    value = value * 10 + static_cast<uint32_t>(valueByte - '0');
  }
  id = value;
  return true;
}

WordInboxSaveResult findNextCaptureId(const char* const directory, uint32_t& id) {
  HalFile root = Storage.open(directory);
  if (!root || !root.isDirectory()) {
    LOG_ERR("WIN", "Failed to open inbox directory: %s", directory);
    return WordInboxSaveResult::StorageError;
  }

  uint32_t maximumId = 0;
  for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (entry.isDirectory()) continue;
    char name[32];
    if (entry.getName(name, sizeof(name)) == 0) continue;
    uint32_t candidate = 0;
    if (parseContextId(name, candidate)) maximumId = std::max(maximumId, candidate);
  }
  if (maximumId >= MAX_CAPTURE_ID) return WordInboxSaveResult::IdExhausted;
  id = maximumId + 1;
  return WordInboxSaveResult::Saved;
}

bool writeContextTemporary(const char* const directory, const uint32_t id, const WordInboxCapture& capture) {
  char path[PATH_CAPACITY];
  if (!buildCapturePath(directory, id, "ctx", true, path, sizeof(path))) return false;
  if (Storage.exists(path)) Storage.remove(path);

  HalFile file;
  if (!Storage.openFileForWrite("WIN", path, file)) {
    LOG_ERR("WIN", "Failed to create context: %s", path);
    return false;
  }

  uint8_t flags = FLAG_SCREENSHOT_AVAILABLE;
  if (!capture.text.empty()) flags |= FLAG_TEXT_AVAILABLE;
  if (capture.textTruncated) flags |= FLAG_TEXT_TRUNCATED;
  const uint8_t progress = std::min<uint8_t>(capture.progressPercent, 100);
  const bool written = writeBytes(file, CONTEXT_MAGIC, sizeof(CONTEXT_MAGIC) - 1) && writePod(file, FORMAT_VERSION) &&
                       writePod(file, flags) && writePod(file, id) && writePod(file, capture.spineIndex) &&
                       writePod(file, capture.currentPage) && writePod(file, capture.totalPages) &&
                       writePod(file, progress) && writeString(file, capture.chapterTitle, MAX_CHAPTER_BYTES) &&
                       writeString(file, capture.text, MAX_TEXT_BYTES) && file.sync();
  file.close();
  if (!written) {
    LOG_ERR("WIN", "Failed to write context: %s", path);
    Storage.remove(path);
  }
  return written;
}

bool saveScreenshotTemporary(const char* const directory, const uint32_t id, const WordInboxCapture& capture) {
  char path[PATH_CAPACITY];
  if (!buildCapturePath(directory, id, "bmp", true, path, sizeof(path))) return false;
  if (Storage.exists(path)) Storage.remove(path);
  return ScreenshotUtil::saveFramebufferAsBmp(path, capture.framebuffer, capture.displayWidth, capture.displayHeight);
}

bool promoteCaptureFile(const char* const directory, const uint32_t id, const char* const extension) {
  char temporaryPath[PATH_CAPACITY];
  char finalPath[PATH_CAPACITY];
  if (!buildCapturePath(directory, id, extension, true, temporaryPath, sizeof(temporaryPath)) ||
      !buildCapturePath(directory, id, extension, false, finalPath, sizeof(finalPath))) {
    return false;
  }
  return Storage.rename(temporaryPath, finalPath);
}

void removeCaptureFile(const char* const directory, const uint32_t id, const char* const extension,
                       const bool temporary) {
  char path[PATH_CAPACITY];
  if (buildCapturePath(directory, id, extension, temporary, path, sizeof(path)) && Storage.exists(path)) {
    Storage.remove(path);
  }
}

bool isValidContextFile(const char* const directory, const uint32_t id) {
  char path[PATH_CAPACITY];
  if (!buildCapturePath(directory, id, "ctx", false, path, sizeof(path))) return false;
  HalFile file;
  if (!Storage.openFileForRead("WIN", path, file)) return false;

  char magic[sizeof(CONTEXT_MAGIC) - 1];
  uint8_t version = 0;
  uint8_t flags = 0;
  uint32_t storedId = 0;
  int32_t spineIndex = -1;
  uint32_t currentPage = 0;
  uint32_t totalPages = 0;
  uint8_t progress = 0;
  uint32_t textLength = 0;
  if (file.read(magic, sizeof(magic)) != static_cast<int>(sizeof(magic)) ||
      std::memcmp(magic, CONTEXT_MAGIC, sizeof(magic)) != 0 || !readPod(file, version) || version != FORMAT_VERSION ||
      !readPod(file, flags) || !readPod(file, storedId) || storedId != id || !readPod(file, spineIndex) ||
      !readPod(file, currentPage) || !readPod(file, totalPages) || !readPod(file, progress) || progress > 100 ||
      !skipString(file, MAX_CHAPTER_BYTES) || !readPod(file, textLength) || textLength > MAX_TEXT_BYTES) {
    return false;
  }
  return file.position() + textLength <= file.size();
}

struct ContextScanResult {
  uint32_t count = 0;
  uint32_t latestId = 0;
  uint32_t position = 0;
  uint32_t previousId = 0;
  uint32_t nextId = 0;
  bool targetFound = false;
};

bool scanContexts(const char* const directory, const uint32_t targetId, ContextScanResult& result) {
  HalFile root = Storage.open(directory);
  if (!root || !root.isDirectory()) return false;

  for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (entry.isDirectory()) continue;
    char name[32];
    if (entry.getName(name, sizeof(name)) == 0) continue;
    uint32_t candidate = 0;
    if (!parseContextId(name, candidate)) continue;
    entry.close();  // validation reopens the context file.
    if (!isValidContextFile(directory, candidate)) continue;

    char screenshotPath[PATH_CAPACITY];
    if (!buildCapturePath(directory, candidate, "bmp", false, screenshotPath, sizeof(screenshotPath)) ||
        !Storage.exists(screenshotPath)) {
      continue;
    }

    result.count++;
    result.latestId = std::max(result.latestId, candidate);
    if (candidate == targetId) result.targetFound = true;
    if (targetId > 0 && candidate <= targetId) result.position++;
    if (targetId > 0 && candidate < targetId) result.previousId = std::max(result.previousId, candidate);
    if (targetId > 0 && candidate > targetId && (result.nextId == 0 || candidate < result.nextId)) {
      result.nextId = candidate;
    }
  }

  return true;
}

bool readBookInfo(const char* const directory, const char* const key, WordInboxBookInfo& info) {
  char path[PATH_CAPACITY];
  if (!buildBookMetadataPath(directory, false, path, sizeof(path))) return false;

  HalFile file;
  if (!Storage.openFileForRead("WIN", path, file)) return false;
  char magic[sizeof(BOOK_MAGIC) - 1];
  uint8_t version = 0;
  uint8_t type = 0;
  if (file.read(magic, sizeof(magic)) != static_cast<int>(sizeof(magic)) ||
      std::memcmp(magic, BOOK_MAGIC, sizeof(magic)) != 0 || !readPod(file, version) || version != FORMAT_VERSION ||
      !readPod(file, type) || type != static_cast<uint8_t>(bookTypeFromKey(key)) ||
      !readString(file, info.title, MAX_TITLE_BYTES) || !readString(file, info.author, MAX_AUTHOR_BYTES) ||
      !readString(file, info.bookPath, MAX_BOOK_PATH_BYTES)) {
    LOG_ERR("WIN", "Invalid Word Inbox book metadata: %s", path);
    return false;
  }

  file.close();
  snprintf(info.key, sizeof(info.key), "%s", key);
  info.bookType = static_cast<WordInboxBookType>(type);
  ContextScanResult scan;
  if (!scanContexts(directory, 0, scan)) return false;
  info.contextCount = scan.count;
  info.latestContextId = scan.latestId;
  return true;
}

bool readContextInfo(const char* const directory, const uint32_t id, WordInboxContextInfo& info) {
  char path[PATH_CAPACITY];
  if (!buildCapturePath(directory, id, "ctx", false, path, sizeof(path))) return false;

  HalFile file;
  if (!Storage.openFileForRead("WIN", path, file)) return false;
  char magic[sizeof(CONTEXT_MAGIC) - 1];
  uint8_t version = 0;
  uint8_t flags = 0;
  uint32_t storedId = 0;
  if (file.read(magic, sizeof(magic)) != static_cast<int>(sizeof(magic)) ||
      std::memcmp(magic, CONTEXT_MAGIC, sizeof(magic)) != 0 || !readPod(file, version) || version != FORMAT_VERSION ||
      !readPod(file, flags) || !readPod(file, storedId) || storedId != id || !readPod(file, info.spineIndex) ||
      !readPod(file, info.currentPage) || !readPod(file, info.totalPages) || !readPod(file, info.progressPercent) ||
      info.progressPercent > 100 || !readString(file, info.chapterTitle, MAX_CHAPTER_BYTES) ||
      !readPod(file, info.textLength) || info.textLength > MAX_TEXT_BYTES) {
    LOG_ERR("WIN", "Invalid Word Inbox context: %s", path);
    return false;
  }

  const size_t textOffset = file.position();
  if (textOffset > std::numeric_limits<uint32_t>::max() || textOffset + info.textLength > file.size()) {
    LOG_ERR("WIN", "Truncated Word Inbox context: %s", path);
    return false;
  }

  info.id = id;
  info.textOffset = static_cast<uint32_t>(textOffset);
  info.hasText = (flags & FLAG_TEXT_AVAILABLE) != 0 && info.textLength > 0;
  info.textTruncated = (flags & FLAG_TEXT_TRUNCATED) != 0;
  info.hasScreenshot = (flags & FLAG_SCREENSHOT_AVAILABLE) != 0;
  return true;
}

}  // namespace

WordInboxSaveResult WordInboxStore::save(const WordInboxCapture& capture, uint32_t& outCaptureId) {
  outCaptureId = 0;
  if (!capture.framebuffer || capture.displayWidth <= 0 || capture.displayWidth % 8 != 0 ||
      capture.displayHeight <= 0 || capture.title.size() > MAX_TITLE_BYTES ||
      capture.author.size() > MAX_AUTHOR_BYTES || capture.chapterTitle.size() > MAX_CHAPTER_BYTES ||
      capture.text.size() > MAX_TEXT_BYTES) {
    LOG_ERR("WIN", "Invalid word inbox capture");
    return WordInboxSaveResult::InvalidInput;
  }

  char directory[PATH_CAPACITY];
  if (!buildBookDirectory(capture, directory, sizeof(directory))) {
    LOG_ERR("WIN", "Invalid word inbox book identity");
    return WordInboxSaveResult::InvalidInput;
  }
  if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
      (!Storage.exists(ROOT_DIR) && !Storage.mkdir(ROOT_DIR)) ||
      (!Storage.exists(directory) && !Storage.mkdir(directory))) {
    LOG_ERR("WIN", "Failed to create inbox directory: %s", directory);
    return WordInboxSaveResult::StorageError;
  }

  const WordInboxSaveResult metadataResult = ensureBookMetadata(directory, capture);
  if (metadataResult != WordInboxSaveResult::Saved) return metadataResult;

  uint32_t id = 0;
  const WordInboxSaveResult idResult = findNextCaptureId(directory, id);
  if (idResult != WordInboxSaveResult::Saved) return idResult;

  // Recover from a power loss after the BMP commit but before the context
  // commit. Without a final .ctx file this ID is not a visible capture.
  removeCaptureFile(directory, id, "bmp", false);
  removeCaptureFile(directory, id, "bmp", true);
  removeCaptureFile(directory, id, "ctx", true);

  if (!saveScreenshotTemporary(directory, id, capture)) {
    LOG_ERR("WIN", "Failed to save inbox screenshot");
    removeCaptureFile(directory, id, "bmp", true);
    return WordInboxSaveResult::ScreenshotError;
  }
  if (!writeContextTemporary(directory, id, capture)) {
    removeCaptureFile(directory, id, "bmp", true);
    return WordInboxSaveResult::StorageError;
  }
  if (!promoteCaptureFile(directory, id, "bmp")) {
    LOG_ERR("WIN", "Failed to commit inbox screenshot");
    removeCaptureFile(directory, id, "bmp", true);
    removeCaptureFile(directory, id, "ctx", true);
    return WordInboxSaveResult::StorageError;
  }
  if (!promoteCaptureFile(directory, id, "ctx")) {
    LOG_ERR("WIN", "Failed to commit inbox context");
    removeCaptureFile(directory, id, "bmp", false);
    removeCaptureFile(directory, id, "ctx", true);
    return WordInboxSaveResult::StorageError;
  }

  outCaptureId = id;
  LOG_INF("WIN", "Saved word inbox context %lu", static_cast<unsigned long>(id));
  return WordInboxSaveResult::Saved;
}

bool WordInboxStore::isValidBookKey(const std::string_view key) { return isValidBookKeyInternal(key); }

bool WordInboxStore::visitBooks(void* const context, const WordInboxBookVisitor visitor) {
  if (!visitor) return false;
  if (!Storage.exists(ROOT_DIR)) return true;

  HalFile root = Storage.open(ROOT_DIR);
  if (!root || !root.isDirectory()) {
    LOG_ERR("WIN", "Failed to open Word Inbox root");
    return false;
  }

  for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (!entry.isDirectory()) continue;
    char key[64];
    if (entry.getName(key, sizeof(key)) == 0) continue;
    const char* baseName = std::strrchr(key, '/');
    baseName = baseName ? baseName + 1 : key;
    if (!isValidBookKeyInternal(baseName)) continue;
    entry.close();  // readBookInfo reopens this directory; only one reader may hold a path.

    char directory[PATH_CAPACITY];
    if (!buildDirectoryForKey(baseName, directory, sizeof(directory))) continue;
    WordInboxBookInfo info;
    if (!readBookInfo(directory, baseName, info) || info.contextCount == 0) continue;
    if (!visitor(context, info)) break;
  }
  return true;
}

bool WordInboxStore::getContext(const std::string_view bookKey, const uint32_t id, WordInboxContextInfo& out) {
  if (!isValidBookKeyInternal(bookKey) || id == 0 || id > MAX_CAPTURE_ID) return false;

  char directory[PATH_CAPACITY];
  if (!buildDirectoryForKey(bookKey, directory, sizeof(directory))) return false;
  ContextScanResult scan;
  if (!scanContexts(directory, id, scan) || !scan.targetFound || !readContextInfo(directory, id, out)) return false;

  out.position = scan.position;
  out.contextCount = scan.count;
  out.previousId = scan.previousId;
  out.nextId = scan.nextId;
  char screenshotPath[PATH_CAPACITY];
  out.hasScreenshot = out.hasScreenshot &&
                      buildCapturePath(directory, id, "bmp", false, screenshotPath, sizeof(screenshotPath)) &&
                      Storage.exists(screenshotPath);
  return out.hasScreenshot;
}

bool WordInboxStore::openContextText(const std::string_view bookKey, const WordInboxContextInfo& context,
                                     HalFile& outFile) {
  outFile.close();
  if (!isValidBookKeyInternal(bookKey) || !context.hasText || context.textLength == 0) return false;

  char directory[PATH_CAPACITY];
  char path[PATH_CAPACITY];
  if (!buildDirectoryForKey(bookKey, directory, sizeof(directory)) ||
      !buildCapturePath(directory, context.id, "ctx", false, path, sizeof(path)) ||
      !Storage.openFileForRead("WIN", path, outFile) || !outFile.seek(context.textOffset)) {
    outFile.close();
    return false;
  }
  return true;
}

bool WordInboxStore::getScreenshotPath(const std::string_view bookKey, const uint32_t id, char* const output,
                                       const size_t outputSize) {
  char directory[PATH_CAPACITY];
  return output && outputSize > 0 && id > 0 && id <= MAX_CAPTURE_ID &&
         buildDirectoryForKey(bookKey, directory, sizeof(directory)) &&
         buildCapturePath(directory, id, "bmp", false, output, outputSize) && Storage.exists(output);
}

bool WordInboxStore::deleteContext(const std::string_view bookKey, const uint32_t id) {
  char directory[PATH_CAPACITY];
  if (!buildDirectoryForKey(bookKey, directory, sizeof(directory)) || id == 0 || id > MAX_CAPTURE_ID) return false;

  char contextPath[PATH_CAPACITY];
  if (!buildCapturePath(directory, id, "ctx", false, contextPath, sizeof(contextPath)) ||
      !Storage.exists(contextPath) || !Storage.remove(contextPath)) {
    return false;
  }

  char screenshotPath[PATH_CAPACITY];
  if (!buildCapturePath(directory, id, "bmp", false, screenshotPath, sizeof(screenshotPath))) return false;
  if (Storage.exists(screenshotPath) && !Storage.remove(screenshotPath)) {
    LOG_ERR("WIN", "Failed to remove orphaned screenshot: %s", screenshotPath);
    return false;
  }
  return true;
}

bool WordInboxStore::deleteBook(const std::string_view bookKey) {
  char directory[PATH_CAPACITY];
  if (!buildDirectoryForKey(bookKey, directory, sizeof(directory)) || !Storage.exists(directory)) return false;
  return Storage.removeDir(directory);
}

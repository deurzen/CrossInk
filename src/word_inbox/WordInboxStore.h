#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

enum class WordInboxBookType : uint8_t {
  Epub = 1,
  Txt = 2,
  Xtc = 3,
};

enum class WordInboxSaveResult : uint8_t {
  Saved,
  InvalidInput,
  StorageError,
  ScreenshotError,
  HashCollision,
  IdExhausted,
};

struct WordInboxCapture {
  WordInboxBookType bookType = WordInboxBookType::Epub;
  std::string_view bookPath;
  std::string_view title;
  std::string_view author;
  std::string_view chapterTitle;
  int32_t spineIndex = -1;
  uint32_t currentPage = 0;
  uint32_t totalPages = 0;
  uint8_t progressPercent = 0;
  std::string_view text;
  bool textTruncated = false;
  const uint8_t* framebuffer = nullptr;
  int displayWidth = 0;
  int displayHeight = 0;
};

struct WordInboxBookInfo {
  char key[24] = {};
  WordInboxBookType bookType = WordInboxBookType::Epub;
  std::string title;
  std::string author;
  std::string bookPath;
  uint32_t contextCount = 0;
  uint32_t earliestContextId = 0;
  uint32_t latestContextId = 0;
};

struct WordInboxContextInfo {
  uint32_t id = 0;
  uint32_t position = 0;
  uint32_t contextCount = 0;
  uint32_t previousId = 0;
  uint32_t nextId = 0;
  int32_t spineIndex = -1;
  uint32_t currentPage = 0;
  uint32_t totalPages = 0;
  uint8_t progressPercent = 0;
  bool hasText = false;
  bool textTruncated = false;
  bool hasScreenshot = false;
  std::string chapterTitle;
  uint32_t textLength = 0;
  uint32_t textOffset = 0;
};

using WordInboxBookVisitor = bool (*)(void* context, const WordInboxBookInfo& book);

class WordInboxStore {
 public:
  static WordInboxSaveResult save(const WordInboxCapture& capture, uint32_t& outCaptureId);

  static bool isValidBookKey(std::string_view key);
  static bool visitBooks(void* context, WordInboxBookVisitor visitor);
  static bool getContext(std::string_view bookKey, uint32_t id, WordInboxContextInfo& out);
  static bool openContextText(std::string_view bookKey, const WordInboxContextInfo& context, HalFile& outFile);
  static bool getScreenshotPath(std::string_view bookKey, uint32_t id, char* output, size_t outputSize);
  static bool deleteContext(std::string_view bookKey, uint32_t id);
  static bool deleteBook(std::string_view bookKey);
};

#pragma once

#include <cstdint>
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

class WordInboxStore {
 public:
  static WordInboxSaveResult save(const WordInboxCapture& capture, uint32_t& outCaptureId);
};

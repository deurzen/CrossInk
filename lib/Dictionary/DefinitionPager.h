#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "DictionaryPackage.h"

namespace dictionary::definition {

constexpr size_t kPageTextBytes = 4096;
constexpr size_t kMaxPageLines = 64;
constexpr size_t kMaxLineBytes = 384;
constexpr size_t kReadChunkBytes = 256;

struct Cursor {
  uint32_t fieldHeaderOffset = 4;
  uint16_t fieldIndex = 0;
  uint16_t fieldByteOffset = 0;
};

struct Line {
  uint16_t textOffset = 0;
  uint16_t textLength = 0;
  uint8_t fieldType = 0;
  bool fieldStart = false;
};

struct Page {
  char text[kPageTextBytes]{};
  Line lines[kMaxPageLines]{};
  Cursor next{};
  uint16_t textBytesUsed = 0;
  uint8_t lineCount = 0;
  bool hasNext = false;

  std::string_view lineText(uint8_t index) const;
};

struct WidthMeasurer {
  void* context = nullptr;
  int (*measure)(void* context, std::string_view text) = nullptr;
};

enum class PagerError : uint8_t {
  NONE = 0,
  INVALID_INPUT,
  PACKAGE_READ_FAILED,
  CURSOR_INVALID,
  INVALID_UTF8,
  LINE_TOO_WIDE,
};

// Streams and wraps one bounded definition page. The caller owns both this
// workspace and Page on the heap because their fixed buffers exceed the reader
// task's safe stack budget. Entry payloads are never materialized in full.
class Pager {
 public:
  bool load(const DictionaryPackage& package, const EntrySlice& entry, const Cursor& start,
            const WidthMeasurer& measurer, int maxLineWidth, Page& output, PagerError& error);

 private:
  uint8_t chunk_[kReadChunkBytes]{};
  char line_[kMaxLineBytes]{};
  uint32_t chunkStart_ = UINT32_MAX;
  size_t chunkLength_ = 0;

  bool readByte(const DictionaryPackage& package, const EntrySlice& entry, uint32_t relativeOffset, uint8_t& value,
                PagerError& error);
};

static_assert(sizeof(Pager) <= 768, "Definition pager workspace exceeds its transient memory budget");
static_assert(sizeof(Page) <= 4608, "Definition page exceeds its transient memory budget");

const char* pagerErrorName(PagerError error);

}  // namespace dictionary::definition

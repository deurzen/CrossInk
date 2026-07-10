#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "BookLanguageReader.h"

namespace dictionary::page_shortlist {

constexpr size_t kMaxVisibleTokens = 192;
constexpr size_t kVisibleSurfacePoolBytes = 3072;
constexpr size_t kMaxItems = 48;
constexpr size_t kMaxAnalysesPerItem = 8;
constexpr size_t kMaxComponentsPerItem = 8;
constexpr size_t kShortlistSurfacePoolBytes = 2048;
constexpr uint32_t kMaxPageShards = 8;
constexpr uint32_t kMaxScannedCandidates = 1024;

struct Item {
  uint16_t surfaceOffset = 0;
  uint16_t localSurfaceId = UINT16_MAX;
  uint16_t primaryLocalLemmaId = UINT16_MAX;
  uint16_t alternateLocalLemmaId = UINT16_MAX;
  uint8_t surfaceLength = 0;
  uint8_t flags = 0;
  uint8_t analysisCount = 0;
  uint8_t componentCount = 0;
  uint16_t confidence = 0;
  uint16_t visibleOrder = 0;
  uint16_t localLemmaIds[kMaxAnalysesPerItem]{};
};

struct Shortlist {
  Item items[kMaxItems]{};
  char surfacePool[kShortlistSurfacePoolBytes]{};
  uint16_t count = 0;
  uint16_t surfaceBytesUsed = 0;
  bool truncated = false;

  std::string_view surface(uint16_t index) const;
};

enum class GenerateError : uint8_t {
  NONE = 0,
  INVALID_INPUT,
  SHARD_RANGE_INVALID,
  READER_FAILED,
};

// Fixed-capacity collector and intersection workspace. It performs no heap
// allocation; callers should own it on the heap because its bounded arrays are
// intentionally larger than the reader task's safe stack budget.
class Generator {
 public:
  void reset();
  bool addRenderedWord(std::string_view word, bool insertedTrailingHyphen);
  void finishRenderedPage();
  bool generate(const book_language::BookLanguageReader& reader, uint32_t firstShard, uint32_t lastShard,
                Shortlist& output, GenerateError& error);

  size_t visibleTokenCount() const { return tokenCount_; }
  bool visibleTokensTruncated() const { return tokensTruncated_; }

 private:
  struct VisibleToken {
    uint64_t hash = 0;
    uint16_t offset = 0;
    uint8_t length = 0;
    uint16_t order = 0;
  };

  VisibleToken tokens_[kMaxVisibleTokens]{};
  char tokenPool_[kVisibleSurfacePoolBytes]{};
  char pendingHyphenated_[256]{};
  uint16_t tokenCount_ = 0;
  uint16_t tokenBytesUsed_ = 0;
  uint16_t pendingLength_ = 0;
  uint16_t nextOrder_ = 0;
  bool tokensTruncated_ = false;

  bool addToken(std::string_view token);
  bool addWordTokens(std::string_view word, bool joinFirst, bool holdLast);
};

static_assert(sizeof(Generator) <= 6656, "Shortlist generator exceeds its transient memory budget");
static_assert(sizeof(Shortlist) <= 4096, "Shortlist output exceeds its transient memory budget");

const char* generateErrorName(GenerateError error);

}  // namespace dictionary::page_shortlist

#pragma once

#include <DictionaryPackage.h>
#include <LexemeStateStore.h>

#include <cstddef>
#include <cstdint>

namespace dictionary::review {

constexpr size_t kMaxReviewPath = 192;
constexpr size_t kMaxReviewItems = 50;

struct Item {
  uint32_t lexemeId = 0;
  lexeme_state::Status status = lexeme_state::Status::Unseen;
};

struct Page {
  Item items[kMaxReviewItems]{};
  size_t count = 0;
  uint32_t nextLexemeId = 0;
  bool done = false;
};

enum class ReviewError : uint8_t {
  NONE = 0,
  INVALID_INPUT,
  PATH_TOO_LONG,
  DICTIONARY_MISSING,
  DICTIONARY_INVALID,
  BUNDLE_MISMATCH,
  STATE_FAILED,
  LEXEME_FAILED,
};

// HAL-backed cold-path session for WebUI review. Own on the heap: retained
// source paths and WAL state exceed the web task's safe stack budget.
class Session {
 public:
  bool open(const uint8_t (&bundleUuid)[16], ReviewError& error);
  bool readPage(uint32_t firstLexemeId, uint32_t scanLexemeCount, uint8_t* scratch, size_t scratchCapacity, Page& page,
                ReviewError& error);
  bool readHeadword(uint32_t lexemeId, char* output, size_t capacity, size_t& length, uint8_t& partOfSpeech,
                    ReviewError& error);
  bool setStatus(uint32_t lexemeId, lexeme_state::Status status, ReviewError& error);

  uint32_t lexemeCount() const { return package_.metadata().lexemeCount; }
  uint32_t generation() const { return state_.generation(); }

 private:
  struct SourceContext {
    char path[kMaxReviewPath]{};
    uint64_t size = 0;
  };

  SourceContext metaSource_{};
  SourceContext lexemesSource_{};
  SourceContext headwordsSource_{};
  SourceContext entriesSource_{};
  DictionaryPackage package_{};
  lexeme_state::Store state_{};
  bool open_ = false;

  static bool sourceReadAt(void* context, uint32_t offset, void* output, size_t length);
  static bool collectItem(void* context, uint32_t lexemeId, lexeme_state::Status status);
};

const char* reviewErrorName(ReviewError error);

}  // namespace dictionary::review

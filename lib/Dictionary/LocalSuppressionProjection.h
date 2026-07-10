#pragma once

#include <cstddef>
#include <cstdint>

#include "LexemeStateStore.h"
#include "PageShortlist.h"

namespace dictionary::suppression {

constexpr size_t kProjectionHeaderSize = 36;

struct LocalLemmaSource {
  void* context = nullptr;
  uint32_t count = 0;
  bool (*readGlobalLexemeId)(void* context, uint16_t localLemmaId, uint32_t& globalLexemeId) = nullptr;
};

LocalLemmaSource localLemmaSource(book_language::BookLanguageReader& reader);

enum class ProjectionError : uint8_t {
  NONE = 0,
  INVALID_INPUT,
  IO_FAILED,
  FORMAT_INVALID,
  STATE_READ_FAILED,
  LEMMA_READ_FAILED,
};

// Caller-owned local suppression bitset. A 32,768-lemma book needs at most
// 4,096 bytes; no global status array is loaded into RAM.
class Projection {
 public:
  bool loadOrRebuild(const lexeme_state::StorageBackend& storage, const char* bookCachePath,
                     const uint8_t (&bundleUuid)[16], const LocalLemmaSource& lemmas, lexeme_state::Store& state,
                     uint8_t* bitset, size_t bitsetCapacity, ProjectionError& error);
  bool patch(uint16_t localLemmaId, lexeme_state::Status status, uint32_t stateGeneration, ProjectionError& error);
  bool isSuppressed(uint16_t localLemmaId) const;
  void filter(page_shortlist::Shortlist& shortlist) const;

  uint32_t generation() const { return generation_; }
  uint32_t localLemmaCount() const { return localLemmaCount_; }
  size_t byteCount() const { return (localLemmaCount_ + 7U) / 8U; }

 private:
  lexeme_state::StorageBackend storage_{};
  char path_[lexeme_state::kMaxStatePath]{};
  uint8_t* bitset_ = nullptr;
  uint32_t localLemmaCount_ = 0;
  uint32_t generation_ = 0;
  bool open_ = false;

  bool rebuild(const uint8_t (&bundleUuid)[16], const LocalLemmaSource& lemmas, lexeme_state::Store& state,
               ProjectionError& error);
};

const char* projectionErrorName(ProjectionError error);

}  // namespace dictionary::suppression

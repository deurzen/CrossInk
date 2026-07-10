#pragma once

#include <BookLanguageReader.h>
#include <DictionaryPackage.h>
#include <LexemeStateStore.h>
#include <LocalSuppressionProjection.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "DictionaryIoMetrics.h"

namespace dictionary::lookup {

inline constexpr char DICTIONARY_ROOT_PATH[] = "/.crosspoint/dictionaries";
constexpr size_t kMaxLookupPath = 256;

enum class SessionError : uint8_t {
  NONE = 0,
  INVALID_INPUT,
  PATH_TOO_LONG,
  BOOK_ARTIFACT_UNAVAILABLE,
  BOOK_ARTIFACT_INVALID,
  DICTIONARY_MISSING,
  DICTIONARY_INVALID,
  BUNDLE_MISMATCH,
  STATE_FAILED,
  PROJECTION_FAILED,
};

// HAL-backed explicit-lookup session. Random-access callbacks open, seek, and
// close one SD file per bounded operation, preserving the hardware's
// single-reader restriction. Own this object on the heap; its retained paths
// and state handles are too large for the reader task stack.
class Session {
 public:
  bool openReaders(const char* languageArtifactPath, const char* bookCachePath,
                   const std::array<uint8_t, 16>& expectedBundleUuid, SessionError& error);
  bool loadLearningState(uint8_t* suppressionBitset, size_t capacity, SessionError& error);

  bool globalLexemeId(uint16_t localLemmaId, uint32_t& globalLexemeId) const;
  bool setStatus(uint16_t localLemmaId, lexeme_state::Status status, SessionError& error);

  size_t requiredSuppressionBytes() const;
  const book_language::BookLanguageReader& book() const { return book_; }
  const DictionaryPackage& package() const { return package_; }
  const suppression::Projection& projection() const { return projection_; }
  suppression::Projection& projection() { return projection_; }
  uint32_t stateGeneration() const { return state_.generation(); }
  const io_metrics::Counters& sourceIoMetrics() const { return sourceIoMetrics_; }
  const io_metrics::Counters& stateIoMetrics() const { return stateIoMetrics_; }

 private:
  struct SourceContext {
    char path[kMaxLookupPath]{};
    uint64_t size = 0;
    io_metrics::Counters* metrics = nullptr;
    uint8_t sourceToken = 0;
  };

  SourceContext languageSource_{};
  SourceContext metaSource_{};
  SourceContext lexemesSource_{};
  SourceContext headwordsSource_{};
  SourceContext entriesSource_{};
  char cachePath_[kMaxLookupPath]{};
  uint8_t bundleUuid_[16]{};
  book_language::BookLanguageReader book_{};
  DictionaryPackage package_{};
  lexeme_state::Store state_{};
  suppression::Projection projection_{};
  io_metrics::Counters sourceIoMetrics_{};
  io_metrics::Counters stateIoMetrics_{};
  bool readersOpen_ = false;
  bool stateOpen_ = false;

  static bool readAt(void* context, uint32_t offset, void* output, size_t length);
  bool initializeSource(SourceContext& context, const char* path, uint8_t sourceToken);
};

const char* sessionErrorName(SessionError error);

}  // namespace dictionary::lookup

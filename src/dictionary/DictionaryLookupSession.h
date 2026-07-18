#pragma once

#include <BookLanguageReader.h>
#include <ContextualRuntimeFormat.h>
#include <DictionaryPackage.h>
#include <HalStorage.h>
#include <LexemeStateStore.h>
#include <PageShortlist.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "DictionaryIoMetrics.h"
#include "SwitchingFileReader.h"

namespace dictionary::lookup {

inline constexpr char DICTIONARY_ROOT_PATH[] = "/.crosspoint/dictionaries";
inline constexpr char CANONICAL_ROOT_PATH[] = "/.crosspoint/lexicons";
constexpr size_t kMaxLookupPath = 256;

enum class SessionError : uint8_t {
  NONE = 0,
  INVALID_INPUT,
  PATH_TOO_LONG,
  BOOK_ARTIFACT_UNAVAILABLE,
  BOOK_ARTIFACT_INVALID,
  DICTIONARY_MISSING,
  DICTIONARY_INVALID,
  IDENTITY_MISMATCH,
  STATE_FAILED,
  PROJECTION_FAILED,
};

// HAL-backed explicit-lookup session. Random-access callbacks open, seek, and
// close one SD file per bounded operation, preserving the hardware's
// single-reader restriction. Own this object on the heap; its retained paths
// and state handles are too large for the reader task stack.
class Session {
 public:
  Session();

  bool openReaders(const char* languageArtifactPath, const char* bookCachePath,
                   const std::array<uint8_t, 16>& expectedIdentityUuid, SessionError& error);
  bool openLearningState(SessionError& error);
  bool filterShortlist(page_shortlist::Shortlist& shortlist, SessionError& error);
  bool openDefinitionPackage(SessionError& error);

  bool globalLexemeId(uint16_t localLemmaId, uint32_t& globalLexemeId) const;
  bool setStatus(uint16_t localLemmaId, lexeme_state::Status status, SessionError& error);
  void closeSourceFile() { sourceReader_.close(); }

  const book_language::BookLanguageReader& book() const { return book_; }
  const DictionaryPackage& package() const { return package_; }
  const contextual::CanonicalLexiconReader& canonical() const { return canonical_; }
  bool usesCanonicalIdentity() const { return contextualIdentity_; }
  uint32_t runtimeLexemeCount() const { return runtimeLexemeCount_; }
  uint32_t stateGeneration() const { return state_.generation(); }
  const io_metrics::Counters& sourceIoMetrics() const { return sourceIoMetrics_; }
  const io_metrics::Counters& stateIoMetrics() const { return stateIoMetrics_; }

 private:
  struct SourceContext {
    char path[kMaxLookupPath]{};
    uint64_t size = 0;
    io::SwitchingFileReader<HalFile>* reader = nullptr;
    io_metrics::Counters* metrics = nullptr;
    uint8_t sourceToken = 0;
  };

  SourceContext languageSource_{};
  SourceContext metaSource_{};
  SourceContext lexemesSource_{};
  SourceContext headwordsSource_{};
  SourceContext entriesSource_{};
  char cachePath_[kMaxLookupPath]{};
  uint8_t identityUuid_[16]{};
  uint32_t runtimeLexemeCount_ = 0;
  book_language::BookLanguageReader book_{};
  DictionaryPackage package_{};
  contextual::CanonicalLexiconReader canonical_{};
  lexeme_state::Store state_{};
  uint32_t shortlistGlobalIds_[page_shortlist::kMaxItems * page_shortlist::kMaxAnalysesPerItem]{};
  uint16_t shortlistStatusOrder_[page_shortlist::kMaxItems * page_shortlist::kMaxAnalysesPerItem]{};
  io_metrics::Counters sourceIoMetrics_{};
  io_metrics::Counters stateIoMetrics_{};
  io::SwitchingFileReader<HalFile> sourceReader_;
  bool readersOpen_ = false;
  bool stateOpen_ = false;
  bool contextualIdentity_ = false;

  static bool openForRead(void* context, const char* path, HalFile& file);
  static bool readAt(void* context, uint32_t offset, void* output, size_t length);
  bool initializeSource(SourceContext& context, const char* path, uint8_t sourceToken);
  bool setSourcePath(SourceContext& context, const char* path, uint8_t sourceToken);
  bool validateLegacyRuntimeMetadata(SessionError& error);
  bool openCanonicalRuntime(const char* directory, SessionError& error);
};

const char* sessionErrorName(SessionError error);

}  // namespace dictionary::lookup

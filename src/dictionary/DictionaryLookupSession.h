#pragma once

#include <BookLanguageReader.h>
#include <ContextualRuntimeFormat.h>
#include <ContextualSourceCatalog.h>
#include <DefinitionPager.h>
#include <HalStorage.h>
#include <LexemeStateStore.h>
#include <PageShortlist.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "DictionaryIoMetrics.h"
#include "SwitchingFileReader.h"

namespace dictionary::lookup {

inline constexpr char CANONICAL_ROOT_PATH[] = "/.crosspoint/lexicons";
inline constexpr char DEFINITION_SOURCE_ROOT_PATH[] = "/.crosspoint/definition-sources";
constexpr size_t kMaxLookupPath = 256;

enum class SourceDiscoveryStatus : uint8_t {
  NOT_APPLICABLE = 0,
  READY,
  PARTIAL,
  ATTACHMENTS_INVALID,
};

enum class DefinitionIndexStatus : uint8_t {
  MISSING = 0,
  PRESENT,
  SOURCE_UNAVAILABLE,
  RECORD_INVALID,
};

struct DefinitionIndexLookup {
  contextual::DefinitionIndexRecord record{};
  DefinitionIndexStatus status = DefinitionIndexStatus::MISSING;
};

enum class SessionError : uint8_t {
  NONE = 0,
  INVALID_INPUT,
  PATH_TOO_LONG,
  BOOK_ARTIFACT_UNAVAILABLE,
  BOOK_ARTIFACT_INVALID,
  CANONICAL_MISSING,
  CANONICAL_INVALID,
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

  bool openReaders(const char* languageArtifactPath, const std::array<uint8_t, 16>& expectedCanonicalUuid,
                   SessionError& error);
  bool openLearningState(SessionError& error);
  bool filterShortlist(page_shortlist::Shortlist& shortlist, SessionError& error);
  bool readDefinitionIndexes(uint32_t canonicalId,
                             std::array<DefinitionIndexLookup, contextual::kMaxAttachedSources>& output,
                             uint8_t& outputCount, SessionError& error);
  bool contextualEntryReader(uint8_t sourceIndex, definition::EntryReader& output, SessionError& error);

  bool globalLexemeId(uint16_t localLemmaId, uint32_t& globalLexemeId) const;
  bool readCanonicalHeadword(uint16_t localLemmaId, char* output, size_t capacity, size_t& outputLength,
                             SessionError& error);
  bool setItemStatus(const page_shortlist::Item& item, lexeme_state::Status status, SessionError& error);
  bool setStatus(uint16_t localLemmaId, lexeme_state::Status status, SessionError& error);
  void closeSourceFile() { sourceReader_.close(); }

  const book_language::BookLanguageReader& book() const { return book_; }
  const contextual::CanonicalLexiconReader& canonical() const { return canonical_; }
  SourceDiscoveryStatus sourceDiscoveryStatus() const { return definitionSources_.status; }
  uint8_t definitionSourceCount() const { return definitionSources_.catalog.sourceCount; }
  uint8_t skippedDefinitionSourceCount() const { return definitionSources_.catalog.skippedCount; }
  uint32_t attachmentGeneration() const { return definitionSources_.catalog.attachmentGeneration; }
  const contextual::DefinitionMetadata* definitionSource(uint8_t index) const {
    return index < definitionSources_.catalog.sourceCount ? &definitionSources_.catalog.sources[index] : nullptr;
  }
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

  struct DefinitionSources {
    contextual::AttachmentStore attachments{};
    contextual::DefinitionSourceReader reader{};
    SourceContext metaSource{};
    SourceContext indexSource{};
    SourceContext entriesSource{};
    SourceContext retainedIndexSources[contextual::kMaxAttachedSources]{};
    SourceContext retainedEntrySources[contextual::kMaxAttachedSources]{};
    contextual::DefinitionSourceCatalog catalog{};
    SourceDiscoveryStatus status = SourceDiscoveryStatus::NOT_APPLICABLE;
  };
  static_assert(sizeof(DefinitionSources) <= 3584,
                "contextual definition state must add no more than 3.5 KiB to the lookup session");

  SourceContext languageSource_{};
  SourceContext metaSource_{};
  SourceContext lexemesSource_{};
  SourceContext headwordsSource_{};
  uint8_t canonicalUuid_[16]{};
  uint32_t runtimeLexemeCount_ = 0;
  book_language::BookLanguageReader book_{};
  contextual::CanonicalLexiconReader canonical_{};
  DefinitionSources definitionSources_{};
  lexeme_state::Store state_{};
  uint32_t shortlistGlobalIds_[page_shortlist::kMaxItems * page_shortlist::kMaxAnalysesPerItem]{};
  uint16_t shortlistStatusOrder_[page_shortlist::kMaxItems * page_shortlist::kMaxAnalysesPerItem]{};
  io_metrics::Counters sourceIoMetrics_{};
  io_metrics::Counters stateIoMetrics_{};
  io::SwitchingFileReader<HalFile> sourceReader_;
  bool readersOpen_ = false;
  bool stateOpen_ = false;

  static bool openForRead(void* context, const char* path, HalFile& file);
  static bool readAt(void* context, uint32_t offset, void* output, size_t length);
  bool initializeSource(SourceContext& context, const char* path, uint8_t sourceToken);
  bool setSourcePath(SourceContext& context, const char* path, uint8_t sourceToken);
  bool openCanonicalRuntime(const char* directory, SessionError& error);
  void discoverDefinitionSources(const char* canonicalDirectory);
  static bool loadDefinitionMetadata(void* context, const uint8_t (&sourceUuid)[16],
                                     contextual::DefinitionMetadata& output);
  bool prepareDefinitionRuntimeSources(uint8_t sourceIndex);
  static bool readContextualEntryChunk(void* context, const EntrySlice& entry, uint32_t relativeOffset, void* output,
                                       size_t capacity, size_t& bytesRead, RuntimeError& error);
};

const char* sessionErrorName(SessionError error);

}  // namespace dictionary::lookup

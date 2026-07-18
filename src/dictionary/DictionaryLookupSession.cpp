#include "DictionaryLookupSession.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "AttachmentStorage.h"
#include "LanguageStateStorage.h"

namespace dictionary::lookup {
namespace {

bool appendPath(char* output, const size_t capacity, const char* directory, const char* leaf) {
  const int written = std::snprintf(output, capacity, "%s/%s", directory, leaf);
  return written > 0 && static_cast<size_t>(written) < capacity;
}

void formatUuid(const uint8_t* uuid, char (&output)[33]) {
  for (size_t index = 0; index < 16; ++index) std::snprintf(output + index * 2, 3, "%02x", uuid[index]);
}

}  // namespace

Session::Session() : sourceReader_(nullptr, openForRead) {}

bool Session::openForRead(void*, const char* path, HalFile& file) {
  return Storage.openFileForRead("DICT", path, file);
}

// Callback ABI requires void* even though the source descriptor is read-only.
// cppcheck-suppress constParameterPointer
bool Session::readAt(void* context, const uint32_t offset, void* output, const size_t length) {
  const auto& source = *static_cast<const SourceContext*>(context);
  return source.reader &&
         source.reader->readAt(source.path, source.sourceToken, source.size, offset, output, length, source.metrics);
}

bool Session::setSourcePath(SourceContext& context, const char* path, const uint8_t sourceToken) {
  if (!path || path[0] == '\0' || std::strlen(path) >= sizeof(context.path)) return false;
  std::strcpy(context.path, path);
  context.reader = &sourceReader_;
  context.metrics = &sourceIoMetrics_;
  context.sourceToken = sourceToken;
  context.size = 0;
  return true;
}

bool Session::initializeSource(SourceContext& context, const char* path, const uint8_t sourceToken) {
  return setSourcePath(context, path, sourceToken) &&
         sourceReader_.fileSize(path, sourceToken, &sourceIoMetrics_, context.size);
}

bool Session::openCanonicalRuntime(const char* directory, SessionError& error) {
  char path[kMaxLookupPath]{};
  if (!appendPath(path, sizeof(path), directory, "meta.bin") || !initializeSource(metaSource_, path, 2) ||
      !appendPath(path, sizeof(path), directory, "lexemes.bin") || !initializeSource(lexemesSource_, path, 3) ||
      !appendPath(path, sizeof(path), directory, "headwords.bin") || !initializeSource(headwordsSource_, path, 4)) {
    error = SessionError::CANONICAL_MISSING;
    return false;
  }

  contextual::RuntimeFormatError formatError;
  const RandomAccessSource meta{&metaSource_, metaSource_.size, readAt};
  const RandomAccessSource lexemes{&lexemesSource_, lexemesSource_.size, readAt};
  const RandomAccessSource headwords{&headwordsSource_, headwordsSource_.size, readAt};
  if (!canonical_.open(meta, lexemes, headwords, formatError)) {
    error = SessionError::CANONICAL_INVALID;
    return false;
  }
  const auto& metadata = canonical_.metadata();
  if (std::memcmp(metadata.canonicalUuid, canonicalUuid_, sizeof(canonicalUuid_)) != 0) {
    error = SessionError::IDENTITY_MISMATCH;
    return false;
  }
  runtimeLexemeCount_ = metadata.lexemeCount;
  return true;
}

bool Session::loadDefinitionMetadata(void* context, const uint8_t (&sourceUuid)[16],
                                     contextual::DefinitionMetadata& output) {
  auto& session = *static_cast<Session*>(context);
  auto& sources = session.definitionSources_;
  sources.reader = {};
  char uuidHex[33]{};
  formatUuid(sourceUuid, uuidHex);
  char path[kMaxLookupPath]{};
  const auto initialize = [&](SourceContext& source, const char* leaf, const uint8_t token) {
    const int written = std::snprintf(path, sizeof(path), "%s/%s/%s", DEFINITION_SOURCE_ROOT_PATH, uuidHex, leaf);
    return written > 0 && static_cast<size_t>(written) < sizeof(path) && session.initializeSource(source, path, token);
  };
  if (!initialize(sources.metaSource, "meta.bin", 5) || !initialize(sources.indexSource, "entry-index.bin", 6) ||
      !initialize(sources.entriesSource, "entries.bin", 7)) {
    session.sourceReader_.close();
    return false;
  }

  const RandomAccessSource meta{&sources.metaSource, sources.metaSource.size, readAt};
  const RandomAccessSource index{&sources.indexSource, sources.indexSource.size, readAt};
  const RandomAccessSource entries{&sources.entriesSource, sources.entriesSource.size, readAt};
  contextual::RuntimeFormatError formatError;
  if (!sources.reader.open(meta, index, entries, session.canonicalUuid_, session.runtimeLexemeCount_, formatError)) {
    session.sourceReader_.close();
    return false;
  }
  output = sources.reader.metadata();
  return true;
}

bool Session::prepareDefinitionRuntimeSources(const uint8_t sourceIndex) {
  if (sourceIndex >= definitionSources_.catalog.sourceCount) return false;
  const auto& metadata = definitionSources_.catalog.sources[sourceIndex];
  char uuidHex[33]{};
  formatUuid(metadata.sourceUuid, uuidHex);
  char path[kMaxLookupPath]{};
  int written = std::snprintf(path, sizeof(path), "%s/%s/entry-index.bin", DEFINITION_SOURCE_ROOT_PATH, uuidHex);
  auto& indexSource = definitionSources_.retainedIndexSources[sourceIndex];
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(path) ||
      !setSourcePath(indexSource, path, static_cast<uint8_t>(8U + sourceIndex))) {
    return false;
  }
  indexSource.size = metadata.indexFileSize;

  written = std::snprintf(path, sizeof(path), "%s/%s/entries.bin", DEFINITION_SOURCE_ROOT_PATH, uuidHex);
  auto& entrySource = definitionSources_.retainedEntrySources[sourceIndex];
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(path) ||
      !setSourcePath(entrySource, path, static_cast<uint8_t>(11U + sourceIndex))) {
    return false;
  }
  entrySource.size = metadata.entriesFileSize;
  return true;
}

void Session::discoverDefinitionSources(const char* canonicalDirectory) {
  definitionSources_ = {};
  definitionSources_.status = SourceDiscoveryStatus::ATTACHMENTS_INVALID;
  // Attachment recovery uses short-lived HalFile instances. Release the shared
  // package reader first so real SD hardware never has two readers open.
  sourceReader_.close();
  contextual::AttachmentError attachmentError;
  const auto& canonicalUuid = *reinterpret_cast<const uint8_t (*)[16]>(canonicalUuid_);
  if (!definitionSources_.attachments.open(attachment_storage::backend(), canonicalDirectory, canonicalUuid,
                                           attachmentError)) {
    LOG_ERR("DICT", "Failed to open contextual attachments: %s", contextual::attachmentErrorName(attachmentError));
    return;
  }
  contextual::AttachmentRecord attachments;
  if (!definitionSources_.attachments.load(attachments, attachmentError)) {
    LOG_ERR("DICT", "Failed to load contextual attachments: %s", contextual::attachmentErrorName(attachmentError));
    return;
  }

  contextual::SourceCatalogError catalogError;
  if (!contextual::buildDefinitionSourceCatalog(attachments, canonicalUuid, runtimeLexemeCount_, loadDefinitionMetadata,
                                                this, definitionSources_.catalog, catalogError)) {
    LOG_ERR("DICT", "Failed to build definition catalog: %s", contextual::sourceCatalogErrorName(catalogError));
    sourceReader_.close();
    return;
  }
  sourceReader_.close();
  definitionSources_.reader = {};
  for (uint8_t sourceIndex = 0; sourceIndex < definitionSources_.catalog.sourceCount; ++sourceIndex) {
    if (!prepareDefinitionRuntimeSources(sourceIndex)) {
      LOG_ERR("DICT", "Failed to retain definition runtime paths");
      definitionSources_.catalog = {};
      return;
    }
  }
  definitionSources_.status =
      definitionSources_.catalog.skippedCount == 0 ? SourceDiscoveryStatus::READY : SourceDiscoveryStatus::PARTIAL;
}

bool Session::openReaders(const char* languageArtifactPath,
                          const std::array<uint8_t, 16>& expectedCanonicalUuid, SessionError& error) {
  readersOpen_ = false;
  stateOpen_ = false;
  runtimeLexemeCount_ = 0;
  canonical_ = {};
  definitionSources_ = {};
  sourceReader_.close();
  sourceIoMetrics_.reset();
  stateIoMetrics_.reset();
  error = SessionError::NONE;
  if (!languageArtifactPath || languageArtifactPath[0] == '\0') {
    error = SessionError::INVALID_INPUT;
    return false;
  }
  std::memcpy(canonicalUuid_, expectedCanonicalUuid.data(), sizeof(canonicalUuid_));

  if (!initializeSource(languageSource_, languageArtifactPath, 1)) {
    error = std::strlen(languageArtifactPath) >= sizeof(languageSource_.path) ? SessionError::PATH_TOO_LONG
                                                                              : SessionError::BOOK_ARTIFACT_UNAVAILABLE;
    return false;
  }
  book_language::ReaderError readerError = book_language::ReaderError::NONE;
  const book_language::RandomAccessSource languageSource{&languageSource_, languageSource_.size, readAt};
  if (!book_.open(languageSource, readerError)) {
    error = SessionError::BOOK_ARTIFACT_INVALID;
    return false;
  }
  if (!book_language::matchesCanonicalLexicon(book_.header(), canonicalUuid_)) {
    error = SessionError::IDENTITY_MISMATCH;
    return false;
  }

  char uuidHex[33]{};
  formatUuid(canonicalUuid_, uuidHex);
  char directory[kMaxLookupPath]{};
  if (!appendPath(directory, sizeof(directory), CANONICAL_ROOT_PATH, uuidHex)) {
    error = SessionError::PATH_TOO_LONG;
    return false;
  }
  if (!openCanonicalRuntime(directory, error)) return false;
  discoverDefinitionSources(directory);
  readersOpen_ = true;
  return true;
}

bool Session::openLearningState(SessionError& error) {
  stateOpen_ = false;
  // Learning-state callbacks open their own files, so release the package
  // reader first to preserve the hardware's single-reader invariant.
  sourceReader_.close();
  stateIoMetrics_.reset();
  error = SessionError::NONE;
  if (!readersOpen_) {
    error = SessionError::INVALID_INPUT;
    return false;
  }

  lexeme_state::StateError stateError = lexeme_state::StateError::NONE;
  if (!state_.open(language_state_storage::backend(&stateIoMetrics_), language_state_storage::ROOT_PATH, canonicalUuid_,
                   runtimeLexemeCount_, stateError)) {
    error = SessionError::STATE_FAILED;
    return false;
  }
  stateOpen_ = true;
  return true;
}

bool Session::filterShortlist(page_shortlist::Shortlist& shortlist, SessionError& error) {
  error = SessionError::NONE;
  if (!readersOpen_ || !stateOpen_ || shortlist.count > page_shortlist::kMaxItems) {
    error = SessionError::INVALID_INPUT;
    return false;
  }

  size_t referenceCount = 0;
  for (uint16_t itemIndex = 0; itemIndex < shortlist.count; ++itemIndex) {
    const auto& item = shortlist.items[itemIndex];
    const uint8_t identityCount = page_shortlist::learningIdentityCount(item);
    if (identityCount == 0) {
      error = SessionError::BOOK_ARTIFACT_INVALID;
      return false;
    }
    for (uint8_t analysis = 0; analysis < identityCount; ++analysis) {
      const size_t flatIndex = static_cast<size_t>(itemIndex) * page_shortlist::kMaxAnalysesPerItem + analysis;
      if (!globalLexemeId(item.localLemmaIds[analysis], shortlistGlobalIds_[flatIndex])) {
        error = SessionError::BOOK_ARTIFACT_INVALID;
        return false;
      }
      shortlistStatusOrder_[referenceCount++] = static_cast<uint16_t>(flatIndex);
    }
  }
  sourceReader_.close();

  std::sort(shortlistStatusOrder_, shortlistStatusOrder_ + referenceCount,
            [this](const uint16_t left, const uint16_t right) {
              return shortlistGlobalIds_[left] < shortlistGlobalIds_[right];
            });
  bool allSuppressed[page_shortlist::kMaxItems]{};
  std::fill(allSuppressed, allSuppressed + shortlist.count, true);
  for (size_t index = 0; index < referenceCount; ++index) {
    const uint16_t flatIndex = shortlistStatusOrder_[index];
    uint8_t packed = 0;
    lexeme_state::StateError stateError = lexeme_state::StateError::NONE;
    const uint32_t globalId = shortlistGlobalIds_[flatIndex];
    if (!state_.readPackedByte(globalId / 2U, packed, stateError)) {
      error = SessionError::STATE_FAILED;
      return false;
    }
    const uint8_t raw = (globalId & 1U) == 0 ? packed & 0x0FU : packed >> 4U;
    if (raw > static_cast<uint8_t>(lexeme_state::Status::ImplicitlyFamiliar)) {
      error = SessionError::STATE_FAILED;
      return false;
    }
    if (!lexeme_state::isSuppressed(static_cast<lexeme_state::Status>(raw))) {
      allSuppressed[flatIndex / page_shortlist::kMaxAnalysesPerItem] = false;
    }
  }

  uint16_t output = 0;
  for (uint16_t input = 0; input < shortlist.count; ++input) {
    if (!allSuppressed[input]) shortlist.items[output++] = shortlist.items[input];
  }
  shortlist.count = output;
  return true;
}

bool Session::readDefinitionIndexes(const uint32_t canonicalId,
                                    std::array<DefinitionIndexLookup, contextual::kMaxAttachedSources>& output,
                                    uint8_t& outputCount, SessionError& error) {
  output = {};
  outputCount = 0;
  error = SessionError::NONE;
  if (!readersOpen_ || canonicalId >= runtimeLexemeCount_) {
    error = SessionError::INVALID_INPUT;
    return false;
  }

  outputCount = definitionSources_.catalog.sourceCount;
  for (uint8_t sourceIndex = 0; sourceIndex < outputCount; ++sourceIndex) {
    const auto& metadata = definitionSources_.catalog.sources[sourceIndex];
    auto& result = output[sourceIndex];
    auto& sourceContext = definitionSources_.retainedIndexSources[sourceIndex];
    const RandomAccessSource source{&sourceContext, sourceContext.size, readAt};
    contextual::RuntimeFormatError formatError;
    if (!contextual::readDefinitionIndexRecord(source, metadata.canonicalLexemeCount, metadata.entriesFileSize,
                                               canonicalId, result.record, formatError)) {
      result.status = formatError == contextual::RuntimeFormatError::RECORD_INVALID
                          ? DefinitionIndexStatus::RECORD_INVALID
                          : DefinitionIndexStatus::SOURCE_UNAVAILABLE;
      LOG_ERR("DICT", "Definition index read failed for %s: %s", metadata.sourceLabel,
              contextual::runtimeFormatErrorName(formatError));
      continue;
    }
    result.status = result.record.present() ? DefinitionIndexStatus::PRESENT : DefinitionIndexStatus::MISSING;
  }
  return true;
}

bool Session::readContextualEntryChunk(void* context, const EntrySlice& entry, const uint32_t relativeOffset,
                                       void* output, const size_t capacity, size_t& bytesRead, RuntimeError& error) {
  bytesRead = 0;
  error = RuntimeError::NONE;
  auto& source = *static_cast<SourceContext*>(context);
  if (!source.reader || entry.length == 0 || entry.length > kMaxEntryBytes ||
      static_cast<uint64_t>(entry.offset) + entry.length > source.size || relativeOffset > entry.length) {
    error = RuntimeError::ENTRY_RANGE_INVALID;
    return false;
  }
  if (capacity == 0 || relativeOffset == entry.length) return true;
  if (!output) {
    error = RuntimeError::OUTPUT_BUFFER_TOO_SMALL;
    return false;
  }
  bytesRead = std::min<size_t>(capacity, entry.length - relativeOffset);
  if (!source.reader->readAt(source.path, source.sourceToken, source.size, entry.offset + relativeOffset, output,
                             bytesRead, source.metrics)) {
    bytesRead = 0;
    error = RuntimeError::ENTRY_READ_FAILED;
    return false;
  }
  return true;
}

bool Session::contextualEntryReader(const uint8_t sourceIndex, definition::EntryReader& output, SessionError& error) {
  output = {};
  error = SessionError::NONE;
  if (!readersOpen_ || sourceIndex >= definitionSources_.catalog.sourceCount) {
    error = SessionError::INVALID_INPUT;
    return false;
  }
  output = {&definitionSources_.retainedEntrySources[sourceIndex], readContextualEntryChunk};
  return true;
}

bool Session::globalLexemeId(const uint16_t localLemmaId, uint32_t& globalLexemeId) const {
  book_language::ReaderError error = book_language::ReaderError::NONE;
  return readersOpen_ && book_.readGlobalLexemeId(localLemmaId, globalLexemeId, error) &&
         globalLexemeId < runtimeLexemeCount_;
}

bool Session::readCanonicalHeadword(const uint16_t localLemmaId, char* output, const size_t capacity,
                                    size_t& outputLength, SessionError& error) {
  outputLength = 0;
  error = SessionError::NONE;
  uint32_t canonicalId = 0;
  if (!readersOpen_ || output == nullptr || capacity == 0 || !globalLexemeId(localLemmaId, canonicalId)) {
    error = SessionError::INVALID_INPUT;
    return false;
  }

  contextual::CanonicalLexemeRecord lexeme;
  contextual::RuntimeFormatError formatError = contextual::RuntimeFormatError::NONE;
  if (!canonical_.readLexeme(canonicalId, lexeme, formatError) ||
      !canonical_.readHeadword(lexeme, output, capacity, outputLength, formatError)) {
    LOG_ERR("DICT", "Canonical headword read failed: %s", contextual::runtimeFormatErrorName(formatError));
    error = SessionError::CANONICAL_INVALID;
    return false;
  }
  output[outputLength] = '\0';
  return true;
}

bool Session::setItemStatus(const page_shortlist::Item& item, const lexeme_state::Status status, SessionError& error) {
  const uint8_t identityCount = page_shortlist::learningIdentityCount(item);
  if (identityCount == 0) {
    error = SessionError::BOOK_ARTIFACT_INVALID;
    return false;
  }
  for (uint8_t analysis = 0; analysis < identityCount; ++analysis) {
    if (!setStatus(item.localLemmaIds[analysis], status, error)) return false;
  }
  return true;
}

bool Session::setStatus(const uint16_t localLemmaId, const lexeme_state::Status status, SessionError& error) {
  error = SessionError::NONE;
  if (!stateOpen_) {
    error = SessionError::STATE_FAILED;
    return false;
  }
  uint32_t globalId = 0;
  if (!globalLexemeId(localLemmaId, globalId)) {
    error = SessionError::BOOK_ARTIFACT_INVALID;
    return false;
  }
  // globalLexemeId() may leave language.bin open. Close it before the state
  // backend performs WAL/status operations through separate handles.
  sourceReader_.close();
  lexeme_state::StateError stateError = lexeme_state::StateError::NONE;
  if (!state_.set(globalId, status, stateError)) {
    error = SessionError::STATE_FAILED;
    return false;
  }
  return true;
}

const char* sessionErrorName(const SessionError error) {
  switch (error) {
    case SessionError::NONE:
      return "none";
    case SessionError::INVALID_INPUT:
      return "invalid input";
    case SessionError::PATH_TOO_LONG:
      return "path too long";
    case SessionError::BOOK_ARTIFACT_UNAVAILABLE:
      return "book artifact unavailable";
    case SessionError::BOOK_ARTIFACT_INVALID:
      return "book artifact invalid";
    case SessionError::CANONICAL_MISSING:
      return "canonical lexicon missing";
    case SessionError::CANONICAL_INVALID:
      return "canonical lexicon invalid";
    case SessionError::IDENTITY_MISMATCH:
      return "identity mismatch";
    case SessionError::STATE_FAILED:
      return "state failed";
    case SessionError::PROJECTION_FAILED:
      return "projection failed";
  }
  return "unknown";
}

}  // namespace dictionary::lookup

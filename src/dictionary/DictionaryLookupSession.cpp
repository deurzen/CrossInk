#include "DictionaryLookupSession.h"

#include <HalStorage.h>

#include <cstdio>
#include <cstring>

#include "LanguageStateStorage.h"

namespace dictionary::lookup {
namespace {

bool appendPath(char* output, const size_t capacity, const char* directory, const char* leaf) {
  const int written = std::snprintf(output, capacity, "%s/%s", directory, leaf);
  return written > 0 && static_cast<size_t>(written) < capacity;
}

}  // namespace

// Callback ABI requires void* even though the source descriptor is read-only.
// cppcheck-suppress constParameterPointer
bool Session::readAt(void* context, const uint32_t offset, void* output, const size_t length) {
  const auto& source = *static_cast<const SourceContext*>(context);
  if (static_cast<uint64_t>(offset) + length > source.size) return false;
  HalFile file;
  return Storage.openFileForRead("DICT", source.path, file) && file.seek(offset) &&
         file.read(output, length) == static_cast<int>(length);
}

bool Session::initializeSource(SourceContext& context, const char* path) {
  if (!path || path[0] == '\0' || std::strlen(path) >= sizeof(context.path)) return false;
  std::strcpy(context.path, path);
  HalFile file;
  if (!Storage.openFileForRead("DICT", path, file)) return false;
  context.size = file.fileSize64();
  return true;
}

bool Session::openReaders(const char* languageArtifactPath, const char* bookCachePath,
                          const std::array<uint8_t, 16>& expectedBundleUuid, SessionError& error) {
  readersOpen_ = false;
  stateOpen_ = false;
  error = SessionError::NONE;
  if (!languageArtifactPath || !bookCachePath || languageArtifactPath[0] == '\0' || bookCachePath[0] == '\0') {
    error = SessionError::INVALID_INPUT;
    return false;
  }
  if (std::strlen(bookCachePath) >= sizeof(cachePath_)) {
    error = SessionError::PATH_TOO_LONG;
    return false;
  }
  std::strcpy(cachePath_, bookCachePath);
  std::memcpy(bundleUuid_, expectedBundleUuid.data(), sizeof(bundleUuid_));

  if (!initializeSource(languageSource_, languageArtifactPath)) {
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
  if (std::memcmp(book_.header().dictionaryBundleUuid, bundleUuid_, sizeof(bundleUuid_)) != 0) {
    error = SessionError::BUNDLE_MISMATCH;
    return false;
  }

  char uuidHex[33]{};
  for (size_t index = 0; index < sizeof(bundleUuid_); ++index) {
    std::snprintf(uuidHex + index * 2, 3, "%02x", bundleUuid_[index]);
  }
  char directory[kMaxLookupPath]{};
  char path[kMaxLookupPath]{};
  if (!appendPath(directory, sizeof(directory), DICTIONARY_ROOT_PATH, uuidHex)) {
    error = SessionError::PATH_TOO_LONG;
    return false;
  }

  const struct {
    const char* leaf;
    SourceContext* source;
  } files[] = {{"meta.bin", &metaSource_},
               {"lexemes.bin", &lexemesSource_},
               {"headwords.bin", &headwordsSource_},
               {"entries.bin", &entriesSource_}};
  for (const auto& file : files) {
    if (!appendPath(path, sizeof(path), directory, file.leaf)) {
      error = SessionError::PATH_TOO_LONG;
      return false;
    }
    if (!initializeSource(*file.source, path)) {
      error = SessionError::DICTIONARY_MISSING;
      return false;
    }
  }

  PackageError packageError = PackageError::NONE;
  const RandomAccessSource meta{&metaSource_, metaSource_.size, readAt};
  const RandomAccessSource lexemes{&lexemesSource_, lexemesSource_.size, readAt};
  const RandomAccessSource headwords{&headwordsSource_, headwordsSource_.size, readAt};
  const RandomAccessSource entries{&entriesSource_, entriesSource_.size, readAt};
  if (!package_.open(meta, lexemes, headwords, entries, packageError)) {
    error = SessionError::DICTIONARY_INVALID;
    return false;
  }
  if (std::memcmp(package_.metadata().dictionaryBundleUuid, bundleUuid_, sizeof(bundleUuid_)) != 0) {
    error = SessionError::BUNDLE_MISMATCH;
    return false;
  }

  readersOpen_ = true;
  return true;
}

size_t Session::requiredSuppressionBytes() const {
  if (!readersOpen_) return 0;
  return (book_.header().localLemmaCount + 7U) / 8U;
}

bool Session::loadLearningState(uint8_t* suppressionBitset, const size_t capacity, SessionError& error) {
  stateOpen_ = false;
  error = SessionError::NONE;
  if (!readersOpen_ || !suppressionBitset || capacity < requiredSuppressionBytes()) {
    error = SessionError::INVALID_INPUT;
    return false;
  }

  lexeme_state::StateError stateError = lexeme_state::StateError::NONE;
  if (!state_.open(language_state_storage::backend(), language_state_storage::ROOT_PATH, bundleUuid_,
                   package_.metadata().lexemeCount, stateError)) {
    error = SessionError::STATE_FAILED;
    return false;
  }

  suppression::ProjectionError projectionError = suppression::ProjectionError::NONE;
  if (!projection_.loadOrRebuild(language_state_storage::backend(), cachePath_, bundleUuid_,
                                 suppression::localLemmaSource(book_), state_, suppressionBitset, capacity,
                                 projectionError)) {
    error = SessionError::PROJECTION_FAILED;
    return false;
  }
  stateOpen_ = true;
  return true;
}

bool Session::globalLexemeId(const uint16_t localLemmaId, uint32_t& globalLexemeId) const {
  book_language::ReaderError error = book_language::ReaderError::NONE;
  return readersOpen_ && book_.readGlobalLexemeId(localLemmaId, globalLexemeId, error);
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
  lexeme_state::StateError stateError = lexeme_state::StateError::NONE;
  if (!state_.set(globalId, status, stateError)) {
    error = SessionError::STATE_FAILED;
    return false;
  }
  suppression::ProjectionError projectionError = suppression::ProjectionError::NONE;
  if (!projection_.patch(localLemmaId, status, state_.generation(), projectionError)) {
    error = SessionError::PROJECTION_FAILED;
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
    case SessionError::DICTIONARY_MISSING:
      return "dictionary missing";
    case SessionError::DICTIONARY_INVALID:
      return "dictionary invalid";
    case SessionError::BUNDLE_MISMATCH:
      return "bundle mismatch";
    case SessionError::STATE_FAILED:
      return "state failed";
    case SessionError::PROJECTION_FAILED:
      return "projection failed";
  }
  return "unknown";
}

}  // namespace dictionary::lookup

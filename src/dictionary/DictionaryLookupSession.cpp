#include "DictionaryLookupSession.h"

#include <HalStorage.h>

#include <algorithm>
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

bool Session::initializeSource(SourceContext& context, const char* path, const uint8_t sourceToken) {
  if (!path || path[0] == '\0' || std::strlen(path) >= sizeof(context.path)) return false;
  std::strcpy(context.path, path);
  context.reader = &sourceReader_;
  context.metrics = &sourceIoMetrics_;
  context.sourceToken = sourceToken;
  return sourceReader_.fileSize(path, sourceToken, &sourceIoMetrics_, context.size);
}

bool Session::openReaders(const char* languageArtifactPath, const char* bookCachePath,
                          const std::array<uint8_t, 16>& expectedBundleUuid, SessionError& error) {
  readersOpen_ = false;
  stateOpen_ = false;
  sourceReader_.close();
  sourceIoMetrics_.reset();
  stateIoMetrics_.reset();
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
  uint8_t sourceToken = 2;
  for (const auto& file : files) {
    if (!appendPath(path, sizeof(path), directory, file.leaf)) {
      error = SessionError::PATH_TOO_LONG;
      return false;
    }
    if (!initializeSource(*file.source, path, sourceToken++)) {
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
  if (!state_.open(language_state_storage::backend(&stateIoMetrics_), language_state_storage::ROOT_PATH, bundleUuid_,
                   package_.metadata().lexemeCount, stateError)) {
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
    if (item.analysisCount == 0 || item.analysisCount > page_shortlist::kMaxAnalysesPerItem) {
      error = SessionError::BOOK_ARTIFACT_INVALID;
      return false;
    }
    for (uint8_t analysis = 0; analysis < item.analysisCount; ++analysis) {
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

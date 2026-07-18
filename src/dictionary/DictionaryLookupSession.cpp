#include "DictionaryLookupSession.h"

#include <Crc32.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "LanguageStateStorage.h"

namespace dictionary::lookup {
namespace {

uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

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

bool Session::validateLegacyRuntimeMetadata(SessionError& error) {
  uint8_t data[kDictionaryMetaSize]{};
  if (metaSource_.size != sizeof(data) || !readAt(&metaSource_, 0, data, sizeof(data)) ||
      std::memcmp(data, "CXDM", 4) != 0 || readU16(data + 4) != kDictionaryPackageVersion ||
      readU16(data + 6) != kDictionaryMetaSize || updateCrc32(0, data, 76) != readU32(data + 76) ||
      std::memcmp(data + 12, identityUuid_, sizeof(identityUuid_)) != 0) {
    error = SessionError::DICTIONARY_INVALID;
    return false;
  }
  runtimeLexemeCount_ = readU32(data + 44);
  if (runtimeLexemeCount_ == 0 || runtimeLexemeCount_ > kMaxLexemeCount || readU16(data + 48) != kLexemeRecordSize) {
    error = SessionError::DICTIONARY_INVALID;
    return false;
  }
  return true;
}

bool Session::openCanonicalRuntime(const char* directory, SessionError& error) {
  char path[kMaxLookupPath]{};
  if (!appendPath(path, sizeof(path), directory, "meta.bin") || !initializeSource(metaSource_, path, 2) ||
      !appendPath(path, sizeof(path), directory, "lexemes.bin") || !initializeSource(lexemesSource_, path, 3) ||
      !appendPath(path, sizeof(path), directory, "headwords.bin") || !initializeSource(headwordsSource_, path, 4)) {
    error = SessionError::DICTIONARY_MISSING;
    return false;
  }

  contextual::RuntimeFormatError formatError;
  const RandomAccessSource meta{&metaSource_, metaSource_.size, readAt};
  const RandomAccessSource lexemes{&lexemesSource_, lexemesSource_.size, readAt};
  const RandomAccessSource headwords{&headwordsSource_, headwordsSource_.size, readAt};
  if (!canonical_.open(meta, lexemes, headwords, formatError)) {
    error = SessionError::DICTIONARY_INVALID;
    return false;
  }
  const auto& metadata = canonical_.metadata();
  if (std::memcmp(metadata.canonicalUuid, identityUuid_, sizeof(identityUuid_)) != 0) {
    error = SessionError::IDENTITY_MISMATCH;
    return false;
  }
  runtimeLexemeCount_ = metadata.lexemeCount;
  return true;
}

bool Session::openReaders(const char* languageArtifactPath, const char* bookCachePath,
                          const std::array<uint8_t, 16>& expectedIdentityUuid, SessionError& error) {
  readersOpen_ = false;
  stateOpen_ = false;
  runtimeLexemeCount_ = 0;
  package_ = {};
  canonical_ = {};
  contextualIdentity_ = false;
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
  std::memcpy(identityUuid_, expectedIdentityUuid.data(), sizeof(identityUuid_));

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
  if (!book_language::matchesIdentity(book_.header(), identityUuid_)) {
    error = SessionError::IDENTITY_MISMATCH;
    return false;
  }

  char uuidHex[33]{};
  for (size_t index = 0; index < sizeof(identityUuid_); ++index) {
    std::snprintf(uuidHex + index * 2, 3, "%02x", identityUuid_[index]);
  }
  char directory[kMaxLookupPath]{};
  char path[kMaxLookupPath]{};
  contextualIdentity_ = book_.header().usesCanonicalIdentity();
  const char* packageRoot = contextualIdentity_ ? CANONICAL_ROOT_PATH : DICTIONARY_ROOT_PATH;
  if (!appendPath(directory, sizeof(directory), packageRoot, uuidHex)) {
    error = SessionError::PATH_TOO_LONG;
    return false;
  }
  if (contextualIdentity_) {
    if (!openCanonicalRuntime(directory, error)) return false;
    readersOpen_ = true;
    return true;
  }

  if (!appendPath(path, sizeof(path), directory, "meta.bin") || !initializeSource(metaSource_, path, 2)) {
    error = SessionError::DICTIONARY_MISSING;
    return false;
  }
  if (!validateLegacyRuntimeMetadata(error)) return false;

  const struct {
    const char* leaf;
    SourceContext* source;
    uint8_t token;
  } deferred[] = {{"lexemes.bin", &lexemesSource_, 3},
                  {"headwords.bin", &headwordsSource_, 4},
                  {"entries.bin", &entriesSource_, 5}};
  const bool sourcesReady = std::all_of(std::begin(deferred), std::end(deferred), [&](const auto& file) {
    return appendPath(path, sizeof(path), directory, file.leaf) && setSourcePath(*file.source, path, file.token);
  });
  if (!sourcesReady) {
    error = SessionError::PATH_TOO_LONG;
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
  if (!state_.open(language_state_storage::backend(&stateIoMetrics_), language_state_storage::ROOT_PATH, identityUuid_,
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

bool Session::openDefinitionPackage(SessionError& error) {
  error = SessionError::NONE;
  if (!readersOpen_) {
    error = SessionError::INVALID_INPUT;
    return false;
  }
  if (contextualIdentity_) {
    // C25/C26 open attached definition sources; v4 must never be interpreted
    // as a legacy package sharing the same UUID.
    error = SessionError::DICTIONARY_MISSING;
    return false;
  }
  if (package_.isOpen()) return true;
  if (!sourceReader_.fileSize(lexemesSource_.path, 3, &sourceIoMetrics_, lexemesSource_.size) ||
      !sourceReader_.fileSize(headwordsSource_.path, 4, &sourceIoMetrics_, headwordsSource_.size) ||
      !sourceReader_.fileSize(entriesSource_.path, 5, &sourceIoMetrics_, entriesSource_.size)) {
    error = SessionError::DICTIONARY_MISSING;
    return false;
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
  return true;
}

bool Session::globalLexemeId(const uint16_t localLemmaId, uint32_t& globalLexemeId) const {
  book_language::ReaderError error = book_language::ReaderError::NONE;
  return readersOpen_ && book_.readGlobalLexemeId(localLemmaId, globalLexemeId, error) &&
         globalLexemeId < runtimeLexemeCount_;
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

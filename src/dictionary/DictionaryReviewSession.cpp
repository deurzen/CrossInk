#include "DictionaryReviewSession.h"

#include <HalStorage.h>

#include <cstdio>
#include <cstring>

#include "DictionaryStorage.h"
#include "LanguageStateStorage.h"

namespace dictionary::review {
namespace {

bool appendPath(char* output, const size_t capacity, const char* directory, const char* leaf) {
  const int written = std::snprintf(output, capacity, "%s/%s", directory, leaf);
  return written > 0 && static_cast<size_t>(written) < capacity;
}

}  // namespace

// cppcheck-suppress constParameterPointer
bool Session::sourceReadAt(void* context, const uint32_t offset, void* output, const size_t length) {
  const auto& source = *static_cast<const SourceContext*>(context);
  if (static_cast<uint64_t>(offset) + length > source.size) return false;
  HalFile file;
  return Storage.openFileForRead("DRV", source.path, file) && file.seek(offset) &&
         file.read(output, length) == static_cast<int>(length);
}

bool Session::open(const uint8_t (&bundleUuid)[16], ReviewError& error) {
  open_ = false;
  error = ReviewError::NONE;
  char uuidHex[33]{};
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t index = 0; index < sizeof(bundleUuid); ++index) {
    uuidHex[index * 2] = HEX_DIGITS[bundleUuid[index] >> 4U];
    uuidHex[index * 2 + 1] = HEX_DIGITS[bundleUuid[index] & 0x0FU];
  }
  char directory[kMaxReviewPath]{};
  if (!appendPath(directory, sizeof(directory), storage::ROOT_PATH, uuidHex)) {
    error = ReviewError::PATH_TOO_LONG;
    return false;
  }

  const auto initialize = [&](SourceContext& source, const char* leaf) {
    if (!appendPath(source.path, sizeof(source.path), directory, leaf)) return false;
    HalFile sourceFile;
    if (!Storage.openFileForRead("DRV", source.path, sourceFile)) return false;
    source.size = sourceFile.fileSize64();
    return true;
  };
  if (!initialize(metaSource_, "meta.bin") || !initialize(lexemesSource_, "lexemes.bin") ||
      !initialize(headwordsSource_, "headwords.bin") || !initialize(entriesSource_, "entries.bin")) {
    error = ReviewError::DICTIONARY_MISSING;
    return false;
  }

  PackageError packageError;
  const RandomAccessSource meta{&metaSource_, metaSource_.size, sourceReadAt};
  const RandomAccessSource lexemes{&lexemesSource_, lexemesSource_.size, sourceReadAt};
  const RandomAccessSource headwords{&headwordsSource_, headwordsSource_.size, sourceReadAt};
  const RandomAccessSource entries{&entriesSource_, entriesSource_.size, sourceReadAt};
  if (!package_.open(meta, lexemes, headwords, entries, packageError)) {
    error = ReviewError::DICTIONARY_INVALID;
    return false;
  }
  if (std::memcmp(package_.metadata().dictionaryBundleUuid, bundleUuid, sizeof(bundleUuid)) != 0) {
    error = ReviewError::BUNDLE_MISMATCH;
    return false;
  }
  lexeme_state::StateError stateError;
  if (!state_.open(language_state_storage::backend(), language_state_storage::ROOT_PATH, bundleUuid,
                   package_.metadata().lexemeCount, stateError)) {
    error = ReviewError::STATE_FAILED;
    return false;
  }
  open_ = true;
  return true;
}

bool Session::collectItem(void* context, const uint32_t lexemeId, const lexeme_state::Status status) {
  auto& page = *static_cast<Page*>(context);
  if (page.count >= kMaxReviewItems) return false;
  page.items[page.count++] = {lexemeId, status};
  return page.count < kMaxReviewItems;
}

bool Session::readPage(const uint32_t firstLexemeId, const uint32_t scanLexemeCount, uint8_t* scratch,
                       const size_t scratchCapacity, Page& page, ReviewError& error) {
  page.count = 0;
  page.nextLexemeId = firstLexemeId;
  page.done = false;
  error = ReviewError::NONE;
  if (!open_) {
    error = ReviewError::INVALID_INPUT;
    return false;
  }
  lexeme_state::StateError stateError;
  if (!state_.visitNonUnseen(firstLexemeId, scanLexemeCount, scratch, scratchCapacity, &page, collectItem,
                             page.nextLexemeId, stateError)) {
    error = ReviewError::STATE_FAILED;
    return false;
  }
  page.done = page.nextLexemeId >= package_.metadata().lexemeCount;
  return true;
}

bool Session::readHeadword(const uint32_t lexemeId, char* output, const size_t capacity, size_t& length,
                           uint8_t& partOfSpeech, ReviewError& error) {
  length = 0;
  partOfSpeech = 0;
  error = ReviewError::NONE;
  LexemeRecord lexeme;
  PackageError packageError;
  if (!open_ || !package_.readLexeme(lexemeId, lexeme, packageError) ||
      !package_.readHeadword(lexeme, output, capacity, length, packageError)) {
    error = ReviewError::LEXEME_FAILED;
    return false;
  }
  partOfSpeech = lexeme.partOfSpeech;
  return true;
}

bool Session::setStatus(const uint32_t lexemeId, const lexeme_state::Status status, ReviewError& error) {
  error = ReviewError::NONE;
  if (!open_) {
    error = ReviewError::INVALID_INPUT;
    return false;
  }
  lexeme_state::StateError stateError;
  if (!state_.set(lexemeId, status, stateError)) {
    error = ReviewError::STATE_FAILED;
    return false;
  }
  return true;
}

const char* reviewErrorName(const ReviewError error) {
  switch (error) {
    case ReviewError::NONE:
      return "none";
    case ReviewError::INVALID_INPUT:
      return "invalid-input";
    case ReviewError::PATH_TOO_LONG:
      return "path-too-long";
    case ReviewError::DICTIONARY_MISSING:
      return "dictionary-missing";
    case ReviewError::DICTIONARY_INVALID:
      return "dictionary-invalid";
    case ReviewError::BUNDLE_MISMATCH:
      return "bundle-mismatch";
    case ReviewError::STATE_FAILED:
      return "state-failed";
    case ReviewError::LEXEME_FAILED:
      return "lexeme-failed";
  }
  return "unknown";
}

}  // namespace dictionary::review

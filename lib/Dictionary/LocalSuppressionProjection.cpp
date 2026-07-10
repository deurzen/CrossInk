#include "LocalSuppressionProjection.h"

#include <cstdio>
#include <cstring>

namespace dictionary::suppression {
namespace {
constexpr char kMagic[] = "CXSP";
constexpr uint16_t kVersion = 1;
constexpr uint32_t kGenerationOffset = 28;

void writeU16(uint8_t* data, const uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
}
void writeU32(uint8_t* data, const uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8U);
  data[2] = static_cast<uint8_t>(value >> 16U);
  data[3] = static_cast<uint8_t>(value >> 24U);
}
uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8U);
}
uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

bool backendValid(const lexeme_state::StorageBackend& storage) {
  return storage.exists && storage.remove && storage.rename && storage.readAt && storage.writeAtSynced &&
         storage.createFileSynced && storage.fileSize;
}

bool readReaderLemma(void* context, const uint16_t localLemmaId, uint32_t& globalLexemeId) {
  auto& reader = *static_cast<book_language::BookLanguageReader*>(context);
  book_language::ReaderError error;
  return reader.readGlobalLexemeId(localLemmaId, globalLexemeId, error);
}

}  // namespace

LocalLemmaSource localLemmaSource(book_language::BookLanguageReader& reader) {
  return {&reader, reader.header().localLemmaCount, readReaderLemma};
}

bool Projection::loadOrRebuild(const lexeme_state::StorageBackend& storage, const char* bookCachePath,
                               const uint8_t (&bundleUuid)[16], const LocalLemmaSource& lemmas,
                               lexeme_state::Store& state, uint8_t* bitset, const size_t bitsetCapacity,
                               ProjectionError& error) {
  open_ = false;
  error = ProjectionError::NONE;
  const size_t requiredBytes = (lemmas.count + 7U) / 8U;
  if (!backendValid(storage) || !bookCachePath || bookCachePath[0] == '\0' ||
      lemmas.count > book_language::kMaxLocalLemmaCount || (lemmas.count > 0 && !lemmas.readGlobalLexemeId) ||
      requiredBytes > bitsetCapacity || (requiredBytes > 0 && !bitset) || state.lexemeCount() == 0) {
    error = ProjectionError::INVALID_INPUT;
    return false;
  }
  const int written = std::snprintf(path_, sizeof(path_), "%s/dictionary-suppress.bin", bookCachePath);
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(path_)) {
    error = ProjectionError::INVALID_INPUT;
    return false;
  }
  storage_ = storage;
  bitset_ = bitset;
  localLemmaCount_ = lemmas.count;
  generation_ = state.generation();
  open_ = true;

  bool valid = false;
  if (storage_.exists(storage_.context, path_)) {
    uint8_t header[kProjectionHeaderSize]{};
    valid = storage_.fileSize(storage_.context, path_) == kProjectionHeaderSize + byteCount() &&
            storage_.readAt(storage_.context, path_, 0, header, sizeof(header)) &&
            std::memcmp(header, kMagic, 4) == 0 && readU16(header + 4) == kVersion &&
            readU16(header + 6) == kProjectionHeaderSize && std::memcmp(header + 8, bundleUuid, 16) == 0 &&
            readU32(header + 24) == localLemmaCount_ && readU32(header + 28) == state.generation() &&
            readU32(header + 32) == byteCount() &&
            (byteCount() == 0 || storage_.readAt(storage_.context, path_, kProjectionHeaderSize, bitset_, byteCount()));
    if (valid && localLemmaCount_ % 8U != 0) {
      const uint8_t validMask = static_cast<uint8_t>((1U << (localLemmaCount_ % 8U)) - 1U);
      valid = (bitset_[byteCount() - 1] & ~validMask) == 0;
    }
  }
  if (valid) return true;
  return rebuild(bundleUuid, lemmas, state, error);
}

bool Projection::rebuild(const uint8_t (&bundleUuid)[16], const LocalLemmaSource& lemmas, lexeme_state::Store& state,
                         ProjectionError& error) {
  if (byteCount() > 0) std::memset(bitset_, 0, byteCount());
  uint32_t cachedPackedIndex = UINT32_MAX;
  uint8_t cachedPacked = 0;
  for (uint32_t localId = 0; localId < localLemmaCount_; ++localId) {
    uint32_t globalId = 0;
    if (!lemmas.readGlobalLexemeId(lemmas.context, static_cast<uint16_t>(localId), globalId) ||
        globalId >= state.lexemeCount()) {
      error = ProjectionError::LEMMA_READ_FAILED;
      return false;
    }
    const uint32_t packedIndex = globalId / 2U;
    if (packedIndex != cachedPackedIndex) {
      lexeme_state::StateError stateError;
      if (!state.readPackedByte(packedIndex, cachedPacked, stateError)) {
        error = ProjectionError::STATE_READ_FAILED;
        return false;
      }
      cachedPackedIndex = packedIndex;
    }
    const uint8_t rawStatus = (globalId & 1U) == 0 ? cachedPacked & 0x0FU : cachedPacked >> 4U;
    if (rawStatus > static_cast<uint8_t>(lexeme_state::Status::ImplicitlyFamiliar)) {
      error = ProjectionError::STATE_READ_FAILED;
      return false;
    }
    if (lexeme_state::isSuppressed(static_cast<lexeme_state::Status>(rawStatus))) {
      bitset_[localId / 8U] |= static_cast<uint8_t>(1U << (localId % 8U));
    }
  }

  uint8_t header[kProjectionHeaderSize]{};
  std::memcpy(header, kMagic, 4);
  writeU16(header + 4, kVersion);
  writeU16(header + 6, kProjectionHeaderSize);
  std::memcpy(header + 8, bundleUuid, 16);
  writeU32(header + 24, localLemmaCount_);
  writeU32(header + 28, state.generation());
  writeU32(header + 32, byteCount());
  char temporary[lexeme_state::kMaxStatePath]{};
  const int written = std::snprintf(temporary, sizeof(temporary), "%s.tmp", path_);
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(temporary) ||
      !storage_.createFileSynced(storage_.context, temporary, header, sizeof(header),
                                 kProjectionHeaderSize + byteCount()) ||
      (byteCount() > 0 &&
       !storage_.writeAtSynced(storage_.context, temporary, kProjectionHeaderSize, bitset_, byteCount())) ||
      !storage_.rename(storage_.context, temporary, path_)) {
    storage_.remove(storage_.context, temporary);
    error = ProjectionError::IO_FAILED;
    return false;
  }
  generation_ = state.generation();
  return true;
}

bool Projection::patch(const uint16_t localLemmaId, const lexeme_state::Status status, const uint32_t stateGeneration,
                       ProjectionError& error) {
  error = ProjectionError::NONE;
  if (!open_ || localLemmaId >= localLemmaCount_ || generation_ == UINT32_MAX || stateGeneration != generation_ + 1U ||
      static_cast<uint8_t>(status) > static_cast<uint8_t>(lexeme_state::Status::ImplicitlyFamiliar)) {
    error = ProjectionError::INVALID_INPUT;
    return false;
  }
  const uint32_t byteIndex = localLemmaId / 8U;
  const uint8_t mask = static_cast<uint8_t>(1U << (localLemmaId % 8U));
  if (lexeme_state::isSuppressed(status)) {
    bitset_[byteIndex] |= mask;
  } else {
    bitset_[byteIndex] &= static_cast<uint8_t>(~mask);
  }
  if (!storage_.writeAtSynced(storage_.context, path_, kProjectionHeaderSize + byteIndex, bitset_ + byteIndex, 1)) {
    error = ProjectionError::IO_FAILED;
    return false;
  }
  uint8_t generationBytes[4]{};
  writeU32(generationBytes, stateGeneration);
  if (!storage_.writeAtSynced(storage_.context, path_, kGenerationOffset, generationBytes, sizeof(generationBytes))) {
    error = ProjectionError::IO_FAILED;
    return false;
  }
  generation_ = stateGeneration;
  return true;
}

bool Projection::isSuppressed(const uint16_t localLemmaId) const {
  return open_ && localLemmaId < localLemmaCount_ &&
         (bitset_[localLemmaId / 8U] & static_cast<uint8_t>(1U << (localLemmaId % 8U))) != 0;
}

void Projection::filter(page_shortlist::Shortlist& shortlist) const {
  uint16_t outputIndex = 0;
  for (uint16_t inputIndex = 0; inputIndex < shortlist.count; ++inputIndex) {
    const auto& item = shortlist.items[inputIndex];
    bool allSuppressed = item.analysisCount > 0;
    for (uint8_t analysis = 0; analysis < item.analysisCount; ++analysis) {
      if (!isSuppressed(item.localLemmaIds[analysis])) {
        allSuppressed = false;
        break;
      }
    }
    if (!allSuppressed) shortlist.items[outputIndex++] = item;
  }
  shortlist.count = outputIndex;
}

const char* projectionErrorName(const ProjectionError error) {
  switch (error) {
    case ProjectionError::NONE:
      return "none";
    case ProjectionError::INVALID_INPUT:
      return "invalid input";
    case ProjectionError::IO_FAILED:
      return "io failed";
    case ProjectionError::FORMAT_INVALID:
      return "format invalid";
    case ProjectionError::STATE_READ_FAILED:
      return "state read failed";
    case ProjectionError::LEMMA_READ_FAILED:
      return "lemma read failed";
  }
  return "unknown";
}

}  // namespace dictionary::suppression

#include "PageShortlist.h"

#include <algorithm>
#include <cstring>

namespace dictionary::page_shortlist {
namespace {

struct Codepoint {
  uint32_t value = 0;
  uint8_t length = 1;
};

Codepoint decodeUtf8(const std::string_view text, const size_t offset) {
  const auto first = static_cast<uint8_t>(text[offset]);
  if (first < 0x80) return {first, 1};
  if ((first & 0xE0) == 0xC0 && offset + 1 < text.size()) {
    return {static_cast<uint32_t>((first & 0x1F) << 6U) | (static_cast<uint8_t>(text[offset + 1]) & 0x3F), 2};
  }
  if ((first & 0xF0) == 0xE0 && offset + 2 < text.size()) {
    return {static_cast<uint32_t>((first & 0x0F) << 12U) |
                static_cast<uint32_t>((static_cast<uint8_t>(text[offset + 1]) & 0x3F) << 6U) |
                (static_cast<uint8_t>(text[offset + 2]) & 0x3F),
            3};
  }
  if ((first & 0xF8) == 0xF0 && offset + 3 < text.size()) {
    return {static_cast<uint32_t>((first & 0x07) << 18U) |
                static_cast<uint32_t>((static_cast<uint8_t>(text[offset + 1]) & 0x3F) << 12U) |
                static_cast<uint32_t>((static_cast<uint8_t>(text[offset + 2]) & 0x3F) << 6U) |
                (static_cast<uint8_t>(text[offset + 3]) & 0x3F),
            4};
  }
  return {first, 1};
}

bool isMark(const uint32_t cp) { return cp >= 0x0300 && cp <= 0x036F; }

bool isLetter(const uint32_t cp) {
  if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return true;
  // Compiler input is NFC. These ranges cover German and other Latin letters;
  // unsupported scripts simply fail to intersect the German artifact.
  return (cp >= 0x00C0 && cp <= 0x02AF) || (cp >= 0x1E00 && cp <= 0x1EFF);
}

bool isJoiner(const uint32_t cp) { return cp == '-' || cp == '\'' || cp == 0x2019 || cp == 0x2011; }

bool lemmasOverlap(const Item& item, const uint16_t* const incoming, const uint8_t incomingCount) {
  for (uint8_t existingIndex = 0; existingIndex < item.analysisCount; ++existingIndex) {
    for (uint8_t incomingIndex = 0; incomingIndex < incomingCount; ++incomingIndex) {
      if (item.localLemmaIds[existingIndex] == incoming[incomingIndex]) return true;
    }
  }
  return false;
}

}  // namespace

std::string_view Shortlist::surface(const uint16_t index) const {
  if (index >= count) return {};
  return std::string_view(surfacePool + items[index].surfaceOffset, items[index].surfaceLength);
}

void Generator::reset() {
  tokenCount_ = 0;
  tokenBytesUsed_ = 0;
  pendingLength_ = 0;
  nextOrder_ = 0;
  tokensTruncated_ = false;
}

bool Generator::addToken(const std::string_view token) {
  if (token.empty() || token.size() > UINT8_MAX) {
    if (token.size() > UINT8_MAX) tokensTruncated_ = true;
    return token.empty();
  }
  const uint64_t hash = book_language::fnv1a64(token);
  for (uint16_t index = 0; index < tokenCount_; ++index) {
    const auto& existing = tokens_[index];
    if (existing.hash == hash && existing.length == token.size() &&
        std::memcmp(tokenPool_ + existing.offset, token.data(), token.size()) == 0) {
      ++nextOrder_;
      return true;
    }
  }
  if (tokenCount_ >= kMaxVisibleTokens || token.size() > kVisibleSurfacePoolBytes - tokenBytesUsed_) {
    tokensTruncated_ = true;
    return false;
  }

  std::memcpy(tokenPool_ + tokenBytesUsed_, token.data(), token.size());
  tokens_[tokenCount_++] = VisibleToken{hash, tokenBytesUsed_, static_cast<uint8_t>(token.size()), nextOrder_++};
  tokenBytesUsed_ = static_cast<uint16_t>(tokenBytesUsed_ + token.size());
  return true;
}

bool Generator::addWordTokens(const std::string_view word, const bool joinFirst, const bool holdLast) {
  struct Slice {
    uint16_t offset;
    uint16_t length;
  };
  constexpr size_t kMaxSlicesPerRenderedWord = 32;
  Slice slices[kMaxSlicesPerRenderedWord]{};
  size_t sliceCount = 0;

  for (size_t cursor = 0; cursor < word.size();) {
    Codepoint cp = decodeUtf8(word, cursor);
    if (!isLetter(cp.value)) {
      cursor += cp.length;
      continue;
    }
    const size_t start = cursor;
    cursor += cp.length;
    while (cursor < word.size()) {
      cp = decodeUtf8(word, cursor);
      if (isLetter(cp.value) || isMark(cp.value)) {
        cursor += cp.length;
        continue;
      }
      if (isJoiner(cp.value)) {
        const size_t afterJoiner = cursor + cp.length;
        if (afterJoiner < word.size() && isLetter(decodeUtf8(word, afterJoiner).value)) {
          cursor = afterJoiner;
          continue;
        }
      }
      break;
    }
    if (sliceCount >= kMaxSlicesPerRenderedWord || cursor - start > UINT16_MAX) {
      tokensTruncated_ = true;
      return false;
    }
    slices[sliceCount++] = {static_cast<uint16_t>(start), static_cast<uint16_t>(cursor - start)};
  }

  if (sliceCount == 0) return true;
  size_t firstSlice = 0;
  if (joinFirst && pendingLength_ > 0) {
    const auto first = word.substr(slices[0].offset, slices[0].length);
    if (first.size() > static_cast<size_t>(UINT8_MAX - pendingLength_)) {
      pendingLength_ = 0;
      tokensTruncated_ = true;
      return false;
    }
    std::memcpy(pendingHyphenated_ + pendingLength_, first.data(), first.size());
    pendingLength_ = static_cast<uint16_t>(pendingLength_ + first.size());
    firstSlice = 1;
    if (!(holdLast && sliceCount == 1) && !addToken(std::string_view(pendingHyphenated_, pendingLength_))) {
      pendingLength_ = 0;
      return false;
    }
    if (!(holdLast && sliceCount == 1)) pendingLength_ = 0;
  }

  for (size_t index = firstSlice; index < sliceCount; ++index) {
    const auto token = word.substr(slices[index].offset, slices[index].length);
    if (holdLast && index + 1 == sliceCount) {
      if (token.size() > UINT8_MAX) {
        tokensTruncated_ = true;
        return false;
      }
      std::memcpy(pendingHyphenated_, token.data(), token.size());
      pendingLength_ = token.size();
    } else if (!addToken(token)) {
      return false;
    }
  }
  return true;
}

bool Generator::addRenderedWord(std::string_view word, const bool insertedTrailingHyphen) {
  if (insertedTrailingHyphen && !word.empty() && word.back() == '-') word.remove_suffix(1);
  const bool joinFirst = pendingLength_ > 0;
  return addWordTokens(word, joinFirst, insertedTrailingHyphen);
}

void Generator::finishRenderedPage() {
  if (pendingLength_ > 0) {
    addToken(std::string_view(pendingHyphenated_, pendingLength_));
    pendingLength_ = 0;
  }
}

bool Generator::generate(const book_language::BookLanguageReader& reader, const uint32_t firstShard,
                         const uint32_t lastShard, Shortlist& output, GenerateError& error) const {
  output = {};
  output.truncated = tokensTruncated_;
  error = GenerateError::NONE;
  if (!reader.isOpen() || tokenCount_ == 0) {
    error = GenerateError::INVALID_INPUT;
    return false;
  }
  if (firstShard > lastShard || lastShard >= reader.header().shardCount) {
    error = GenerateError::SHARD_RANGE_INVALID;
    return false;
  }

  const uint64_t requestedShardCount = static_cast<uint64_t>(lastShard) - firstShard + 1U;
  const uint32_t shardsToScan = static_cast<uint32_t>(std::min<uint64_t>(requestedShardCount, kMaxPageShards));
  if (requestedShardCount > kMaxPageShards) output.truncated = true;
  uint32_t candidatesScanned = 0;
  book_language::ReaderError readerError = book_language::ReaderError::NONE;

  for (uint32_t shardOffset = 0; shardOffset < shardsToScan; ++shardOffset) {
    book_language::ShardDirectoryRecord shard;
    if (!reader.readShard(firstShard + shardOffset, shard, readerError)) {
      error = GenerateError::READER_FAILED;
      return false;
    }
    uint64_t previousHash = 0;
    bool havePreviousHash = false;
    for (uint16_t candidateIndex = 0; candidateIndex < shard.recordCount; ++candidateIndex) {
      if (candidatesScanned++ >= kMaxScannedCandidates) {
        output.truncated = true;
        return true;
      }
      book_language::ShardCandidate candidate;
      if (!reader.readCandidate(shard, candidateIndex, candidate, readerError) ||
          (havePreviousHash && candidate.surfaceHash < previousHash)) {
        error = GenerateError::READER_FAILED;
        return false;
      }
      previousHash = candidate.surfaceHash;
      havePreviousHash = true;

      for (uint16_t tokenIndex = 0; tokenIndex < tokenCount_; ++tokenIndex) {
        const auto& token = tokens_[tokenIndex];
        if (token.hash != candidate.surfaceHash || token.length != candidate.surfaceByteLength) continue;
        const std::string_view visibleSurface(tokenPool_ + token.offset, token.length);
        book_language::SurfaceRecord surface;
        bool equal = false;
        if (!reader.readSurface(candidate.localSurfaceId, surface, readerError) ||
            !reader.surfaceEquals(surface, visibleSurface, equal, readerError)) {
          error = GenerateError::READER_FAILED;
          return false;
        }
        if (!equal) continue;

        if (surface.analysisCount > kMaxAnalysesPerItem || surface.componentCount > kMaxComponentsPerItem) {
          output.truncated = true;
          break;
        }
        // Compound-only matches currently have no whole-word lexeme to open.
        // Do not expose them as actionable definitions: the browser's bounded
        // splitter can otherwise produce false positives such as foreign words
        // assembled from unrelated short dictionary forms.
        if (surface.analysisCount == 0) break;
        uint16_t analysisIds[kMaxAnalysesPerItem]{};
        for (uint8_t analysisIndex = 0; analysisIndex < surface.analysisCount; ++analysisIndex) {
          if (!reader.readSurfaceAnalysis(surface, analysisIndex, analysisIds[analysisIndex], readerError)) {
            error = GenerateError::READER_FAILED;
            return false;
          }
        }
        for (uint8_t componentIndex = 0; componentIndex < surface.componentCount; ++componentIndex) {
          uint16_t localLemmaId = UINT16_MAX;
          if (!reader.readSurfaceComponent(surface, componentIndex, localLemmaId, readerError)) {
            error = GenerateError::READER_FAILED;
            return false;
          }
        }
        if ((surface.analysisCount > 0 ? analysisIds[0] : UINT16_MAX) != candidate.primaryLocalLemmaId ||
            (surface.analysisCount > 1 ? analysisIds[1] : UINT16_MAX) != candidate.alternateLocalLemmaId) {
          error = GenerateError::READER_FAILED;
          return false;
        }

        bool duplicate = false;
        for (uint16_t itemIndex = 0; itemIndex < output.count; ++itemIndex) {
          duplicate = output.items[itemIndex].localSurfaceId == candidate.localSurfaceId ||
                      lemmasOverlap(output.items[itemIndex], analysisIds, surface.analysisCount);
          if (duplicate) break;
        }
        if (duplicate) break;
        if (output.count >= kMaxItems || visibleSurface.size() > kShortlistSurfacePoolBytes - output.surfaceBytesUsed) {
          output.truncated = true;
          return true;
        }

        const uint16_t poolOffset = output.surfaceBytesUsed;
        std::memcpy(output.surfacePool + poolOffset, visibleSurface.data(), visibleSurface.size());
        output.surfaceBytesUsed = static_cast<uint16_t>(output.surfaceBytesUsed + visibleSurface.size());
        Item item;
        item.surfaceOffset = poolOffset;
        item.localSurfaceId = candidate.localSurfaceId;
        item.primaryLocalLemmaId = candidate.primaryLocalLemmaId;
        item.alternateLocalLemmaId = candidate.alternateLocalLemmaId;
        item.surfaceLength = static_cast<uint8_t>(visibleSurface.size());
        item.flags = candidate.flags;
        item.analysisCount = surface.analysisCount;
        item.componentCount = surface.componentCount;
        item.confidence = surface.confidence;
        item.visibleOrder = token.order;
        std::copy(analysisIds, analysisIds + surface.analysisCount, item.localLemmaIds);
        uint16_t insertAt = output.count;
        while (insertAt > 0 && output.items[insertAt - 1].visibleOrder > item.visibleOrder) {
          output.items[insertAt] = output.items[insertAt - 1];
          --insertAt;
        }
        output.items[insertAt] = item;
        ++output.count;
        break;
      }
    }
  }
  return true;
}

const char* generateErrorName(const GenerateError error) {
  switch (error) {
    case GenerateError::NONE:
      return "none";
    case GenerateError::INVALID_INPUT:
      return "invalid input";
    case GenerateError::SHARD_RANGE_INVALID:
      return "shard range invalid";
    case GenerateError::READER_FAILED:
      return "reader failed";
  }
  return "unknown";
}

}  // namespace dictionary::page_shortlist

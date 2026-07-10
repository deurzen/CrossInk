#include "VisiblePageWords.h"

#include <Epub/Page.h>

#include <cstdint>

namespace VisiblePageWords {
namespace {

bool hasVisibleText(const std::string_view text) {
  for (size_t index = 0; index < text.size();) {
    const auto first = static_cast<uint8_t>(text[index]);
    if (first == ' ' || first == '\t' || first == '\r' || first == '\n') {
      ++index;
      continue;
    }
    if (first == 0xC2 && index + 1 < text.size() && static_cast<uint8_t>(text[index + 1]) == 0xA0) {
      index += 2;
      continue;
    }
    if (first == 0xE2 && index + 2 < text.size() && static_cast<uint8_t>(text[index + 1]) == 0x80) {
      const auto third = static_cast<uint8_t>(text[index + 2]);
      if (third == 0x83 || third == 0xAF) {
        index += 3;
        continue;
      }
    }
    return true;
  }
  return false;
}

enum class VisitLineResult : uint8_t { Continue, CallbackStopped, LimitReached };

VisitLineResult visitLine(const TextBlock& block, const size_t maxWords, const Callbacks& callbacks, Result& result) {
  int lastVisibleWord = -1;
  for (uint16_t index = 0; index < block.wordCount(); ++index) {
    const std::string_view text(block.wordText(index), block.wordTextLen(index));
    if (hasVisibleText(text)) {
      lastVisibleWord = index;
    }
  }

  if (lastVisibleWord < 0) return VisitLineResult::Continue;

  for (uint16_t index = 0; index < block.wordCount(); ++index) {
    const std::string_view text(block.wordText(index), block.wordTextLen(index));
    if (!hasVisibleText(text)) continue;
    if (result.wordsVisited >= maxWords) {
      result.limitReached = true;
      return VisitLineResult::LimitReached;
    }

    const Word word{text, block.wordEndsWithInsertedHyphen(index)};
    ++result.wordsVisited;
    if (callbacks.onWord && !callbacks.onWord(callbacks.context, word)) {
      return VisitLineResult::CallbackStopped;
    }
  }

  const bool joinsNextLine = block.wordEndsWithInsertedHyphen(static_cast<uint16_t>(lastVisibleWord));
  if (callbacks.onLineEnd && !callbacks.onLineEnd(callbacks.context, joinsNextLine)) {
    return VisitLineResult::CallbackStopped;
  }
  return VisitLineResult::Continue;
}

VisitLineResult visitTable(const PageTableFragment& table, const size_t maxWords, const Callbacks& callbacks,
                           Result& result) {
  for (const auto& row : table.getRows()) {
    for (const auto& cell : row.cells) {
      for (const auto& line : cell.lines) {
        if (!line) continue;
        const auto lineResult = visitLine(*line, maxWords, callbacks, result);
        if (lineResult != VisitLineResult::Continue) return lineResult;
      }
    }
  }
  return VisitLineResult::Continue;
}

}  // namespace

Result visitEpubPage(const Page& page, const size_t maxWords, const Callbacks& callbacks) {
  Result result;
  for (const auto& element : page.elements) {
    if (!element) continue;

    VisitLineResult elementResult = VisitLineResult::Continue;
    switch (element->getTag()) {
      case TAG_PageLine: {
        const auto& line = static_cast<const PageLine&>(*element);
        if (line.getBlock()) {
          elementResult = visitLine(*line.getBlock(), maxWords, callbacks, result);
        }
        break;
      }
      case TAG_PageTableFragment:
        elementResult = visitTable(static_cast<const PageTableFragment&>(*element), maxWords, callbacks, result);
        break;
      case TAG_PageImage:
      case TAG_PageHorizontalRule:
        break;
    }

    if (elementResult != VisitLineResult::Continue) return result;
  }

  result.completed = true;
  return result;
}

}  // namespace VisiblePageWords

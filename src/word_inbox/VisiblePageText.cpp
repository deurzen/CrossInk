#include "VisiblePageText.h"

#include <Epub/Page.h>

#include <cstdint>
#include <string_view>

namespace VisiblePageText {
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

bool appendTextBlock(const TextBlock& block, VisibleTextBuffer& output) {
  int lastVisibleWord = -1;
  for (uint16_t index = 0; index < block.wordCount(); ++index) {
    const std::string_view token(block.wordText(index), block.wordTextLen(index));
    if (hasVisibleText(token)) {
      lastVisibleWord = index;
    }
  }

  for (uint16_t index = 0; index < block.wordCount(); ++index) {
    const std::string_view token(block.wordText(index), block.wordTextLen(index));
    if (!hasVisibleText(token)) continue;
    output.appendWord(token, false, block.wordEndsWithInsertedHyphen(index));
  }

  return lastVisibleWord >= 0 && block.wordEndsWithInsertedHyphen(static_cast<uint16_t>(lastVisibleWord));
}

void appendTable(const PageTableFragment& table, VisibleTextBuffer& output) {
  for (const auto& row : table.getRows()) {
    for (const auto& cell : row.cells) {
      for (const auto& line : cell.lines) {
        if (!line) continue;
        const bool joinsNextLine = appendTextBlock(*line, output);
        output.finishLine(joinsNextLine);
      }
    }
  }
}

}  // namespace

VisibleTextBuffer::Result fromEpubPage(const Page& page, char* const buffer, const size_t capacity) {
  VisibleTextBuffer output(buffer, capacity);
  for (const auto& element : page.elements) {
    if (!element) continue;

    switch (element->getTag()) {
      case TAG_PageLine: {
        const auto& line = static_cast<const PageLine&>(*element);
        if (line.getBlock()) {
          const bool joinsNextLine = appendTextBlock(*line.getBlock(), output);
          output.finishLine(joinsNextLine);
        }
        break;
      }
      case TAG_PageTableFragment:
        appendTable(static_cast<const PageTableFragment&>(*element), output);
        break;
      case TAG_PageImage:
      case TAG_PageHorizontalRule:
        break;
    }
  }
  return output.result();
}

VisibleTextBuffer::Result fromTxtLines(const std::vector<std::string>& lines, char* const buffer,
                                       const size_t capacity) {
  VisibleTextBuffer output(buffer, capacity);
  for (const auto& line : lines) {
    output.appendLine(line);
  }
  return output.result();
}

}  // namespace VisiblePageText

#include "VisiblePageText.h"

#include <Epub/Page.h>

#include "util/VisiblePageWords.h"

namespace VisiblePageText {
namespace {

// One-byte words require a separator after the first, so this cap can fill but
// cannot prematurely truncate the 8 KiB output buffer.
constexpr size_t MAX_VISITED_EPUB_WORDS = (MAX_TEXT_BYTES + 1) / 2;

bool appendWord(void* const context, const VisiblePageWords::Word& word) {
  auto& output = *static_cast<VisibleTextBuffer*>(context);
  output.appendWord(word.text, false, word.insertedTrailingHyphen);
  return !output.result().truncated;
}

bool finishLine(void* const context, const bool joinsNextLine) {
  auto& output = *static_cast<VisibleTextBuffer*>(context);
  output.finishLine(joinsNextLine);
  return !output.result().truncated;
}

}  // namespace

VisibleTextBuffer::Result fromEpubPage(const Page& page, char* const buffer, const size_t capacity) {
  VisibleTextBuffer output(buffer, capacity);
  const VisiblePageWords::Callbacks callbacks{&output, appendWord, finishLine};
  VisiblePageWords::visitEpubPage(page, MAX_VISITED_EPUB_WORDS, callbacks);
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

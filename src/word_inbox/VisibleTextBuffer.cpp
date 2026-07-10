#include "VisibleTextBuffer.h"

#include <algorithm>
#include <cstdint>

namespace {

bool isAsciiWhitespace(const uint8_t value) { return value == ' ' || value == '\t' || value == '\r' || value == '\n'; }

size_t utf8WhitespaceLengthAt(const std::string_view text, const size_t index) {
  const auto first = static_cast<uint8_t>(text[index]);
  if (first == 0xC2 && index + 1 < text.size() && static_cast<uint8_t>(text[index + 1]) == 0xA0) {
    return 2;  // Non-breaking space.
  }
  if (first == 0xE2 && index + 2 < text.size() && static_cast<uint8_t>(text[index + 1]) == 0x80) {
    const auto third = static_cast<uint8_t>(text[index + 2]);
    if (third == 0x83 || third == 0xAF) {
      return 3;  // Em space or narrow non-breaking space.
    }
  }
  return 0;
}

size_t whitespaceLengthAt(const std::string_view text, const size_t index) {
  return isAsciiWhitespace(static_cast<uint8_t>(text[index])) ? 1 : utf8WhitespaceLengthAt(text, index);
}

std::string_view trimWhitespace(std::string_view text) {
  size_t begin = 0;
  while (begin < text.size()) {
    const size_t whitespaceLength = whitespaceLengthAt(text, begin);
    if (whitespaceLength == 0) break;
    begin += whitespaceLength;
  }

  size_t end = text.size();
  while (end > begin) {
    size_t candidate = end - 1;
    while (candidate > begin && (static_cast<uint8_t>(text[candidate]) & 0xC0) == 0x80) {
      --candidate;
    }
    const size_t whitespaceLength = whitespaceLengthAt(text, candidate);
    if (whitespaceLength == 0 || candidate + whitespaceLength != end) break;
    end = candidate;
  }
  return text.substr(begin, end - begin);
}

size_t utf8SequenceLength(const uint8_t lead) {
  if ((lead & 0x80) == 0) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;
}

}  // namespace

VisibleTextBuffer::VisibleTextBuffer(char* const buffer, const size_t capacity) : buffer(buffer), capacity(capacity) {
  if (buffer && capacity > 0) {
    buffer[0] = '\0';
  }
}

bool VisibleTextBuffer::appendByte(const char value) {
  if (!buffer || capacity == 0 || length + 1 >= capacity) {
    truncated = true;
    return false;
  }
  buffer[length++] = value;
  buffer[length] = '\0';
  return true;
}

bool VisibleTextBuffer::appendUtf8(const std::string_view text) {
  for (size_t index = 0; index < text.size();) {
    size_t sequenceLength = utf8SequenceLength(static_cast<uint8_t>(text[index]));
    if (index + sequenceLength > text.size()) {
      sequenceLength = 1;  // Preserve malformed input without reading past it.
    }
    if (!buffer || capacity == 0 || sequenceLength >= capacity - length) {
      truncated = true;
      return false;
    }
    for (size_t byte = 0; byte < sequenceLength; ++byte) {
      buffer[length++] = text[index + byte];
    }
    buffer[length] = '\0';
    index += sequenceLength;
  }
  return true;
}

void VisibleTextBuffer::appendPendingSeparator(const bool attachedToPrevious) {
  if (!hasText) return;

  if (joinNextToken) {
    joinNextToken = false;
    pendingLineBreak = false;
    return;
  }
  if (pendingLineBreak) {
    appendByte('\n');
    pendingLineBreak = false;
  } else if (!attachedToPrevious && length > 0 && buffer[length - 1] != ' ' && buffer[length - 1] != '\n') {
    appendByte(' ');
  }
}

void VisibleTextBuffer::appendWord(std::string_view word, const bool attachedToPrevious,
                                   const bool insertedTrailingHyphen) {
  if (truncated) return;

  word = trimWhitespace(word);
  if (insertedTrailingHyphen && !word.empty() && word.back() == '-') {
    word.remove_suffix(1);
    word = trimWhitespace(word);
  }
  if (word.empty()) return;

  appendPendingSeparator(attachedToPrevious);
  if (truncated) return;

  bool pendingSpace = false;
  for (size_t index = 0; index < word.size();) {
    const size_t whitespaceLength = whitespaceLengthAt(word, index);
    if (whitespaceLength > 0) {
      pendingSpace = length > 0 && buffer[length - 1] != ' ' && buffer[length - 1] != '\n';
      index += whitespaceLength;
      continue;
    }

    if (pendingSpace) {
      if (!appendByte(' ')) return;
      pendingSpace = false;
    }

    const size_t sequenceLength = std::min(utf8SequenceLength(static_cast<uint8_t>(word[index])), word.size() - index);
    if (!appendUtf8(word.substr(index, sequenceLength))) return;
    index += sequenceLength;
  }

  lineHasText = true;
  hasText = true;
}

void VisibleTextBuffer::finishLine(const bool joinNextLine) {
  if (!lineHasText || truncated) return;
  pendingLineBreak = !joinNextLine;
  joinNextToken = joinNextLine;
  lineHasText = false;
}

void VisibleTextBuffer::appendLine(std::string_view line) {
  if (truncated) return;

  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.remove_suffix(1);
  }
  if (renderedLineCount > 0 && !appendByte('\n')) return;
  renderedLineCount++;

  if (!line.empty() && appendUtf8(line)) {
    hasText = true;
  }
}

VisibleTextBuffer::Result VisibleTextBuffer::result() const { return Result{buffer ? buffer : "", length, truncated}; }

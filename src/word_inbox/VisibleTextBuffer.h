#pragma once

#include <cstddef>
#include <string_view>

class VisibleTextBuffer {
 public:
  struct Result {
    const char* text = nullptr;
    size_t length = 0;
    bool truncated = false;
  };

  VisibleTextBuffer(char* buffer, size_t capacity);

  // Appends one rendered token. Callers set attachedToPrevious when the token
  // visually touches the previous token (for example, separate punctuation).
  void appendWord(std::string_view word, bool attachedToPrevious = false, bool insertedTrailingHyphen = false);
  void finishLine(bool joinNextLine = false);

  // TXT/Markdown pages already retain complete rendered lines.
  void appendLine(std::string_view line);

  [[nodiscard]] Result result() const;

 private:
  char* buffer;
  size_t capacity;
  size_t length = 0;
  bool truncated = false;
  bool lineHasText = false;
  bool hasText = false;
  bool pendingLineBreak = false;
  bool joinNextToken = false;
  size_t renderedLineCount = 0;

  bool appendByte(char value);
  bool appendUtf8(std::string_view text);
  void appendPendingSeparator(bool attachedToPrevious);
};

#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "VisibleTextBuffer.h"

TEST(VisibleTextBuffer, JoinsRenderedWordsAndLines) {
  std::array<char, 128> storage{};
  VisibleTextBuffer text(storage.data(), storage.size());

  text.appendWord("Das");
  text.appendWord(
      "Gespr\xC3\xA4"
      "ch");
  text.appendWord(".", true);
  text.finishLine();
  text.appendWord("Weiter");
  text.finishLine();

  const auto result = text.result();
  EXPECT_EQ(std::string_view(result.text, result.length),
            "Das Gespr\xC3\xA4"
            "ch.\nWeiter");
  EXPECT_FALSE(result.truncated);
}

TEST(VisibleTextBuffer, NormalizesWhitespaceInsideWords) {
  std::array<char, 128> storage{};
  VisibleTextBuffer text(storage.data(), storage.size());

  text.appendWord("  eins\t\xC2\xA0zwei\xE2\x80\xAF drei  ");

  const auto result = text.result();
  EXPECT_EQ(std::string_view(result.text, result.length), "eins zwei drei");
}

TEST(VisibleTextBuffer, RemovesLayoutHyphenAndJoinsNextLine) {
  std::array<char, 128> storage{};
  VisibleTextBuffer text(storage.data(), storage.size());

  text.appendWord("unbe-");
  text.finishLine(true);
  text.appendWord("kannt");
  text.finishLine();

  const auto result = text.result();
  EXPECT_EQ(std::string_view(result.text, result.length), "unbe-kannt");

  std::array<char, 128> insertedStorage{};
  VisibleTextBuffer inserted(insertedStorage.data(), insertedStorage.size());
  inserted.appendWord("unbe-", false, true);
  inserted.finishLine(true);
  inserted.appendWord("kannt");

  const auto insertedResult = inserted.result();
  EXPECT_EQ(std::string_view(insertedResult.text, insertedResult.length), "unbekannt");
}

TEST(VisibleTextBuffer, PreservesTxtLinesWithoutTrailingNewline) {
  std::array<char, 128> storage{};
  VisibleTextBuffer text(storage.data(), storage.size());

  text.appendLine("erste Zeile\r\n");
  text.appendLine("");
  text.appendLine("dritte Zeile");

  const auto result = text.result();
  EXPECT_EQ(std::string_view(result.text, result.length), "erste Zeile\n\ndritte Zeile");
}

TEST(VisibleTextBuffer, TruncatesOnlyAtUtf8Boundary) {
  std::array<char, 6> storage{};  // Four data bytes plus room for one UTF-8 character and NUL is not enough.
  VisibleTextBuffer text(storage.data(), storage.size());

  text.appendWord("abc\xC3\xA4z");

  const auto result = text.result();
  EXPECT_EQ(std::string_view(result.text, result.length), "abc\xC3\xA4");
  EXPECT_TRUE(result.truncated);
  EXPECT_EQ(storage[result.length], '\0');
}

TEST(VisibleTextBuffer, HandlesUnavailableOutputBuffer) {
  VisibleTextBuffer text(nullptr, 0);
  text.appendWord("text");

  const auto result = text.result();
  EXPECT_TRUE(result.truncated);
  EXPECT_EQ(result.length, 0U);
  EXPECT_STREQ(result.text, "");
}

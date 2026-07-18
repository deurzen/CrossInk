#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>

#include "DefinitionPager.h"
#include "GrammarPresentation.h"

namespace {
using dictionary::grammar_presentation::Label;

constexpr const char* kEnglishLabels[] = {
    "Other",
    "Noun",
    "Verb",
    "Adjective",
    "Adverb",
    "Pronoun",
    "Determiner",
    "Adposition",
    "Conjunction",
    "Numeral",
    "Particle",
    "Interjection",
    "Proper noun",
    "Phrase",
    "Abbreviation",
    "Other",
    "Nominative",
    "Accusative",
    "Dative",
    "Genitive",
    "Positive",
    "Comparative",
    "Superlative",
    "Masculine",
    "Feminine",
    "Neuter",
    "Indicative",
    "Subjunctive",
    "Imperative",
    "Singular",
    "Plural",
    "First person",
    "Second person",
    "Third person",
    "Present",
    "Past",
    "Perfect",
    "Finite",
    "Infinitive",
    "Participle",
    "First person singular",
    "First person plural",
    "Second person singular",
    "Second person plural",
    "Third person singular",
    "Third person plural",
};
static_assert(sizeof(kEnglishLabels) / sizeof(kEnglishLabels[0]) == static_cast<size_t>(Label::Count));

const char* englishLabel(void*, const Label label) { return kEnglishLabels[static_cast<size_t>(label)]; }

const char* germanLabel(void*, const Label label) {
  switch (label) {
    case Label::PosVerb:
      return "Verb";
    case Label::TensePast:
      return "Präteritum";
    case Label::MoodIndicative:
      return "Indikativ";
    case Label::ThirdSingular:
      return "3. Person Singular";
    default:
      return englishLabel(nullptr, label);
  }
}

int byteWidth(void*, const std::string_view text) { return static_cast<int>(text.size()); }

std::string grammar(const uint8_t pos, const uint32_t descriptor, const int maxWidth = 512,
                    const dictionary::grammar_presentation::LabelProvider labels = {nullptr, englishLabel}) {
  std::array<char, 192> output{};
  size_t length = 0;
  EXPECT_TRUE(dictionary::grammar_presentation::formatGrammarLine(pos, descriptor, labels, {nullptr, byteWidth},
                                                                  maxWidth, output.data(), output.size(), length));
  return std::string(output.data(), length);
}

}  // namespace

TEST(GrammarPresentation, FormatsPrimaryPosAwareOrderInEnglishAndGerman) {
  EXPECT_EQ(grammar(2, 0x0000DA80U), "Verb · Past · Indicative · Third person singular");
  EXPECT_EQ(grammar(2, 0x0000DA80U, 512, {nullptr, germanLabel}), "Verb · Präteritum · Indikativ · 3. Person Singular");
  EXPECT_EQ(grammar(1, 0), "Noun");
  EXPECT_EQ(grammar(2, 0x00010000U), "Verb · Infinitive");
  EXPECT_EQ(grammar(2, 0x00018000U), "Verb · Participle");
  EXPECT_EQ(grammar(1, 0x00000263U), "Noun · Dative · Neuter · Singular");
  EXPECT_EQ(grammar(3, 0x00000408U), "Adjective · Plural · Positive");
  EXPECT_EQ(grammar(5, 0x00001C01U), "Pronoun · Nominative · Third person plural");
}

TEST(GrammarPresentation, AppliesFrozenComponentDropOrder) {
  constexpr uint32_t adjective = 0x0000022AU;
  const std::string withoutPositive = "Adjective · Accusative · Masculine · Singular";
  EXPECT_EQ(grammar(3, adjective, withoutPositive.size()), withoutPositive);

  const std::string finiteWithoutIndicative = "Verb · Past · Third person singular";
  EXPECT_EQ(grammar(2, 0x0000DA80U, finiteWithoutIndicative.size()), finiteWithoutIndicative);
}

TEST(GrammarPresentation, MapsEveryLabelAndRejectsMalformedDescriptor) {
  for (size_t index = 0; index < static_cast<size_t>(Label::Count); ++index) {
    EXPECT_STRNE(englishLabel(nullptr, static_cast<Label>(index)), "");
  }
  std::array<char, 192> output{};
  size_t length = 0;
  EXPECT_FALSE(dictionary::grammar_presentation::formatGrammarLine(
      2, 0x00020000U, {nullptr, englishLabel}, {nullptr, byteWidth}, 512, output.data(), output.size(), length));
}

TEST(GrammarPresentation, KeepsAnalysisAndSourceBoundariesInsideOnePackedLine) {
  dictionary::definition::Line line;
  line.analysisStart = true;
  line.sourceStart = true;
  line.analysisIndex = 7;
  line.sourceIndex = 2;
  EXPECT_EQ(sizeof(line), 6U);
  EXPECT_TRUE(line.analysisStart);
  EXPECT_TRUE(line.sourceStart);
  EXPECT_EQ(line.analysisIndex, 7);
  EXPECT_EQ(line.sourceIndex, 2);
  EXPECT_EQ(dictionary::definition::contentLineLimit(10, 5, 2, 2), 6U);
  EXPECT_EQ(dictionary::definition::contentLineLimit(10, 6, 2, 2), 0U);
}

TEST(GrammarPresentation, FitsDefinitionTitleWithoutSplittingUtf8) {
  const std::string title = "knipste · außergewöhnlichlangeslemma";
  std::array<char, 32> output{};
  size_t length = 0;
  ASSERT_TRUE(
      dictionary::grammar_presentation::fitText(title, {nullptr, byteWidth}, 24, output.data(), output.size(), length));
  EXPECT_EQ(std::string_view(output.data(), length), "knipste · außergew…");
  EXPECT_LE(length, 24U);
  EXPECT_EQ(output[length], '\0');
}

TEST(GrammarPresentation, TruncatesAlternativeHeadwordWithoutSplittingUtf8) {
  const std::string headword = "ääääääääääääääääääää";
  std::array<char, 64> output{};
  size_t length = 0;
  ASSERT_TRUE(dictionary::grammar_presentation::formatAnalysisLabel(
      headword, 1, {nullptr, englishLabel}, {nullptr, byteWidth}, 30, output.data(), output.size(), length));
  const std::string rendered(output.data(), length);
  EXPECT_LE(rendered.size(), 30U);
  EXPECT_EQ(rendered.substr(rendered.size() - std::strlen(" · Noun")), " · Noun");
  EXPECT_NE(rendered.find("…"), std::string::npos);
  for (size_t index = 0; index < rendered.size();) {
    const uint8_t first = static_cast<uint8_t>(rendered[index]);
    const size_t bytes = first < 0x80 ? 1 : (first & 0xE0) == 0xC0 ? 2 : 3;
    ASSERT_LE(index + bytes, rendered.size());
    index += bytes;
  }
}

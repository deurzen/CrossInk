#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dictionary::grammar_presentation {

enum class Label : uint8_t {
  PosUnknown = 0,
  PosNoun,
  PosVerb,
  PosAdjective,
  PosAdverb,
  PosPronoun,
  PosDeterminer,
  PosAdposition,
  PosConjunction,
  PosNumeral,
  PosParticle,
  PosInterjection,
  PosProperNoun,
  PosPhrase,
  PosAbbreviation,
  PosOther,
  CaseNominative,
  CaseAccusative,
  CaseDative,
  CaseGenitive,
  DegreePositive,
  DegreeComparative,
  DegreeSuperlative,
  GenderMasculine,
  GenderFeminine,
  GenderNeuter,
  MoodIndicative,
  MoodSubjunctive,
  MoodImperative,
  NumberSingular,
  NumberPlural,
  PersonFirst,
  PersonSecond,
  PersonThird,
  TensePresent,
  TensePast,
  TensePerfect,
  VerbFormFinite,
  VerbFormInfinitive,
  VerbFormParticiple,
  FirstSingular,
  FirstPlural,
  SecondSingular,
  SecondPlural,
  ThirdSingular,
  ThirdPlural,
  Count,
};

struct LabelProvider {
  void* context = nullptr;
  const char* (*get)(void* context, Label label) = nullptr;
};

struct TextMeasurer {
  void* context = nullptr;
  int (*measure)(void* context, std::string_view text) = nullptr;
};

// Formats translated primary POS/grammar into caller-owned storage. Components
// are omitted in the frozen fallback order until both byte and pixel limits fit.
bool formatGrammarLine(uint8_t canonicalPos, uint32_t descriptor, const LabelProvider& labels,
                       const TextMeasurer& measurer, int maxWidth, char* output, size_t capacity, size_t& outputLength);

// Formats "headword · POS", truncating only the headword at UTF-8 boundaries.
bool formatAnalysisLabel(std::string_view headword, uint8_t canonicalPos, const LabelProvider& labels,
                         const TextMeasurer& measurer, int maxWidth, char* output, size_t capacity,
                         size_t& outputLength);

}  // namespace dictionary::grammar_presentation

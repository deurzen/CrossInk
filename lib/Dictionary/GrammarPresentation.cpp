#include "GrammarPresentation.h"

#include <algorithm>
#include <cstring>

#include "BookLanguageFormat.h"

namespace dictionary::grammar_presentation {
namespace {

constexpr char kSeparator[] = " \xC2\xB7 ";
constexpr char kEllipsis[] = "\xE2\x80\xA6";
constexpr size_t kMaxComponents = 9;

struct Component {
  Label label = Label::PosUnknown;
  uint8_t dropPriority = 0;
  bool visible = true;
};

constexpr Label kPosLabels[] = {
    Label::PosUnknown,     Label::PosNoun,    Label::PosVerb,         Label::PosAdjective,
    Label::PosAdverb,      Label::PosPronoun, Label::PosDeterminer,   Label::PosAdposition,
    Label::PosConjunction, Label::PosNumeral, Label::PosParticle,     Label::PosInterjection,
    Label::PosProperNoun,  Label::PosPhrase,  Label::PosAbbreviation, Label::PosOther,
};
static_assert(sizeof(kPosLabels) / sizeof(kPosLabels[0]) == 16);

bool isContinuation(const char value) { return (static_cast<uint8_t>(value) & 0xC0U) == 0x80U; }

size_t safePrefix(const std::string_view text, size_t length) {
  length = std::min(length, text.size());
  while (length > 0 && length < text.size() && isContinuation(text[length])) --length;
  return length;
}

size_t removeLastCodepoint(const std::string_view text, const size_t length) {
  if (length == 0) return 0;
  size_t start = length - 1;
  while (start > 0 && isContinuation(text[start])) --start;
  return start;
}

bool append(char* output, const size_t capacity, size_t& length, const std::string_view text) {
  if (text.size() > capacity - 1U - length) return false;
  std::memcpy(output + length, text.data(), text.size());
  length += text.size();
  output[length] = '\0';
  return true;
}

const char* translated(const LabelProvider& provider, const Label label) {
  return provider.get ? provider.get(provider.context, label) : nullptr;
}

bool buildLine(Component* components, const size_t count, const LabelProvider& labels, char* output,
               const size_t capacity, size_t& outputLength) {
  outputLength = 0;
  if (!output || capacity == 0) return false;
  output[0] = '\0';
  bool first = true;
  for (size_t index = 0; index < count; ++index) {
    if (!components[index].visible) continue;
    const char* text = translated(labels, components[index].label);
    if (!text || text[0] == '\0') return false;
    if (!first && !append(output, capacity, outputLength, kSeparator)) return false;
    if (!append(output, capacity, outputLength, text)) return false;
    first = false;
  }
  return !first;
}

bool fits(const TextMeasurer& measurer, const char* output, const size_t length, const int maxWidth) {
  return measurer.measure && maxWidth > 0 &&
         measurer.measure(measurer.context, std::string_view(output, length)) <= maxWidth;
}

Label personNumberLabel(const uint32_t person, const uint32_t number) {
  constexpr Label values[3][2] = {{Label::FirstSingular, Label::FirstPlural},
                                  {Label::SecondSingular, Label::SecondPlural},
                                  {Label::ThirdSingular, Label::ThirdPlural}};
  return values[person - 1U][number - 1U];
}

Label fieldLabel(const Label first, const uint32_t code) {
  return static_cast<Label>(static_cast<uint8_t>(first) + code - 1U);
}

bool fitSingleLabel(const std::string_view text, const TextMeasurer& measurer, const int maxWidth, char* output,
                    const size_t capacity, size_t& outputLength) {
  if (!output || capacity < sizeof(kEllipsis)) return false;
  outputLength = 0;
  if (append(output, capacity, outputLength, text) && fits(measurer, output, outputLength, maxWidth)) return true;

  size_t prefix = safePrefix(text, std::min(text.size(), capacity - sizeof(kEllipsis)));
  while (true) {
    outputLength = 0;
    if (!append(output, capacity, outputLength, text.substr(0, prefix)) ||
        !append(output, capacity, outputLength, kEllipsis)) {
      return false;
    }
    if (fits(measurer, output, outputLength, maxWidth)) return true;
    if (prefix == 0) return false;
    prefix = removeLastCodepoint(text, prefix);
  }
}

}  // namespace

bool formatGrammarLine(const uint8_t canonicalPos, const uint32_t descriptor, const LabelProvider& labels,
                       const TextMeasurer& measurer, const int maxWidth, char* output, const size_t capacity,
                       size_t& outputLength) {
  outputLength = 0;
  if (canonicalPos >= sizeof(kPosLabels) / sizeof(kPosLabels[0]) ||
      !book_language::isGrammarDescriptorValid(descriptor) || !labels.get || !measurer.measure || !output ||
      capacity == 0 || maxWidth <= 0) {
    return false;
  }

  const uint32_t caseCode = descriptor & 0x7U;
  const uint32_t degree = (descriptor >> 3U) & 0x3U;
  const uint32_t gender = (descriptor >> 5U) & 0x3U;
  const uint32_t mood = (descriptor >> 7U) & 0x3U;
  const uint32_t number = (descriptor >> 9U) & 0x3U;
  const uint32_t person = (descriptor >> 11U) & 0x3U;
  const uint32_t tense = (descriptor >> 13U) & 0x3U;
  const uint32_t verbForm = (descriptor >> 15U) & 0x3U;

  Component components[kMaxComponents]{};
  size_t count = 0;
  const auto add = [&](const Label label, const uint8_t dropPriority) {
    if (count < kMaxComponents) components[count++] = {label, dropPriority, true};
  };
  const auto addCase = [&]() {
    if (caseCode != 0) add(fieldLabel(Label::CaseNominative, caseCode), 4);
  };
  const auto addGender = [&]() {
    if (gender != 0) add(fieldLabel(Label::GenderMasculine, gender), 2);
  };
  const auto addDegree = [&]() {
    if (degree != 0) add(fieldLabel(Label::DegreePositive, degree), degree == 1 ? 1 : 7);
  };
  const auto addMood = [&]() {
    if (mood != 0) add(fieldLabel(Label::MoodIndicative, mood), mood == 1 ? 3 : 8);
  };
  const auto addTense = [&]() {
    if (tense != 0) add(fieldLabel(Label::TensePresent, tense), 9);
  };
  const auto addPersonNumber = [&]() {
    if (person != 0 && number != 0) {
      add(personNumberLabel(person, number), 10);
    } else if (number != 0) {
      add(fieldLabel(Label::NumberSingular, number), 5);
    } else if (person != 0) {
      add(fieldLabel(Label::PersonFirst, person), 6);
    }
  };
  const auto addVerbForm = [&]() {
    if (verbForm != 0) add(fieldLabel(Label::VerbFormFinite, verbForm), 11);
  };

  add(kPosLabels[canonicalPos], 0);
  if (canonicalPos == 2 && verbForm == 1) {
    addTense();
    addMood();
    addPersonNumber();
    if (tense == 0 && mood == 0 && person == 0 && number == 0) addVerbForm();
  } else if (canonicalPos == 2 && verbForm == 2) {
    addVerbForm();
  } else if (canonicalPos == 2 && verbForm == 3) {
    addVerbForm();
    addTense();
    addCase();
    addGender();
    if (number != 0) add(fieldLabel(Label::NumberSingular, number), 5);
    addDegree();
  } else if (canonicalPos == 1 || canonicalPos == 12) {
    addCase();
    addGender();
    if (number != 0) add(fieldLabel(Label::NumberSingular, number), 5);
  } else if (canonicalPos == 3) {
    addCase();
    addGender();
    if (number != 0) add(fieldLabel(Label::NumberSingular, number), 5);
    addDegree();
  } else if (canonicalPos == 5 || canonicalPos == 6 || canonicalPos == 9) {
    addCase();
    addGender();
    addPersonNumber();
  } else if (canonicalPos == 4) {
    addDegree();
  } else {
    addCase();
    addGender();
    addPersonNumber();
    addTense();
    addMood();
    addVerbForm();
    addDegree();
  }

  if (buildLine(components, count, labels, output, capacity, outputLength) &&
      fits(measurer, output, outputLength, maxWidth)) {
    return true;
  }
  for (uint8_t priority = 1; priority <= 11; ++priority) {
    bool changed = false;
    for (size_t index = 1; index < count; ++index) {
      if (components[index].visible && components[index].dropPriority == priority) {
        components[index].visible = false;
        changed = true;
      }
    }
    if (changed && buildLine(components, count, labels, output, capacity, outputLength) &&
        fits(measurer, output, outputLength, maxWidth)) {
      return true;
    }
  }

  const char* pos = translated(labels, kPosLabels[canonicalPos]);
  return pos && fitSingleLabel(pos, measurer, maxWidth, output, capacity, outputLength);
}

bool formatAnalysisLabel(const std::string_view headword, const uint8_t canonicalPos, const LabelProvider& labels,
                         const TextMeasurer& measurer, const int maxWidth, char* output, const size_t capacity,
                         size_t& outputLength) {
  outputLength = 0;
  if (headword.empty() || canonicalPos >= sizeof(kPosLabels) / sizeof(kPosLabels[0]) || !labels.get ||
      !measurer.measure || !output || capacity == 0 || maxWidth <= 0) {
    return false;
  }
  const char* posValue = translated(labels, kPosLabels[canonicalPos]);
  if (!posValue || posValue[0] == '\0') return false;
  const std::string_view pos(posValue);

  const auto build = [&](const size_t prefix, const bool ellipsized) {
    outputLength = 0;
    return append(output, capacity, outputLength, headword.substr(0, prefix)) &&
           (!ellipsized || append(output, capacity, outputLength, kEllipsis)) &&
           append(output, capacity, outputLength, kSeparator) && append(output, capacity, outputLength, pos);
  };
  if (build(headword.size(), false) && fits(measurer, output, outputLength, maxWidth)) return true;

  const size_t suffixBytes = sizeof(kEllipsis) - 1U + sizeof(kSeparator) - 1U + pos.size();
  size_t prefix = capacity > suffixBytes ? safePrefix(headword, capacity - 1U - suffixBytes) : 0;
  while (true) {
    if (build(prefix, true) && fits(measurer, output, outputLength, maxWidth)) return true;
    if (prefix == 0) break;
    prefix = removeLastCodepoint(headword, prefix);
  }
  outputLength = 0;
  return append(output, capacity, outputLength, pos) && fits(measurer, output, outputLength, maxWidth);
}

}  // namespace dictionary::grammar_presentation

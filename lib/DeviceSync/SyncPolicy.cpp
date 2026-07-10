#include "SyncPolicy.h"

#include <cstring>
#include <iterator>

namespace DeviceSync {
namespace {

size_t categoryIndex(const Category category) { return static_cast<size_t>(category); }

char foldAscii(const char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool patternIsSafe(const char* pattern) {
  if (pattern == nullptr || pattern[0] != '/') return false;
  const size_t length = strnlen(pattern, MAX_POLICY_PATTERN_BYTES);
  if (length == 0 || length >= MAX_POLICY_PATTERN_BYTES || (length > 1 && pattern[length - 1] == '/')) return false;
  if (length == 1) return true;

  size_t segmentStart = 1;
  for (size_t i = 1; i <= length; ++i) {
    const char value = pattern[i];
    if (value == '\\' || (value != '\0' && (static_cast<uint8_t>(value) < 0x20 || value == 0x7F))) return false;
    if (value != '/' && value != '\0') continue;
    if (i == segmentStart) return false;
    const size_t segmentLength = i - segmentStart;
    if ((segmentLength == 1 && pattern[segmentStart] == '.') ||
        (segmentLength == 2 && pattern[segmentStart] == '.' && pattern[segmentStart + 1] == '.')) {
      return false;
    }
    segmentStart = i + 1;
  }
  return true;
}

// Keep the bounded 192-byte DP workspace out of the caller's directory-scan frame.
__attribute__((noinline)) bool globMatches(const char* pattern, const char* path) {
  const size_t patternLength = std::strlen(pattern);
  const size_t pathLength = std::strlen(path);
  if (patternLength >= 3 && std::strcmp(pattern + patternLength - 3, "/**") == 0 && pathLength == patternLength - 3) {
    bool samePrefix = true;
    for (size_t i = 0; i < pathLength; ++i) {
      samePrefix = samePrefix && foldAscii(pattern[i]) == foldAscii(path[i]);
    }
    if (samePrefix) return true;
  }

  bool previous[MAX_POLICY_PATTERN_BYTES] = {};
  bool current[MAX_POLICY_PATTERN_BYTES] = {};
  previous[0] = true;

  for (size_t j = 1; j <= patternLength; ++j) {
    previous[j] = pattern[j - 1] == '*' && previous[j - 1];
  }

  for (size_t i = 1; i <= pathLength; ++i) {
    current[0] = false;
    const char pathChar = path[i - 1];
    for (size_t j = 1; j <= patternLength; ++j) {
      const char patternChar = pattern[j - 1];
      if (patternChar == '*') {
        const bool crossesSlash = (j >= 2 && pattern[j - 2] == '*') || (j < patternLength && pattern[j] == '*');
        current[j] = current[j - 1] || ((crossesSlash || pathChar != '/') && previous[j]);
      } else if (patternChar == '?') {
        current[j] = pathChar != '/' && previous[j - 1];
      } else {
        current[j] = foldAscii(patternChar) == foldAscii(pathChar) && previous[j - 1];
      }
    }
    std::memcpy(previous, current, (patternLength + 1) * sizeof(previous[0]));
    std::memset(current, 0, (patternLength + 1) * sizeof(current[0]));
  }
  return previous[patternLength];
}

}  // namespace

SyncPolicy::SyncPolicy() { reset(); }

void SyncPolicy::reset() {
  directions_.fill(Direction::Disabled);
  clearPathRules();
  mirrorDeletions_ = false;
}

void SyncPolicy::setDefaults() {
  reset();
  setDirection(Category::BookContent, Direction::Bidirectional);
  setDirection(Category::EpubProgress, Direction::Bidirectional);
  setDirection(Category::XtcTxtProgress, Direction::Bidirectional);
  setDirection(Category::Bookmarks, Direction::Bidirectional);
  setDirection(Category::Clippings, Direction::Bidirectional);
  setDirection(Category::PortableBookSettings, Direction::Bidirectional);
  setDirection(Category::FinishedState, Direction::Bidirectional);

  static constexpr const char* DEFAULT_RULES[] = {
      "/**",
      "/.crosspoint/**",
      "/.fonts/**",
      "/fonts/**",
      "/.sleep/**",
      "/sleep/**",
      "/.crossink-stats-backup/**",
      "/.device-sync-*",
      "/**/.device-sync-*",
  };
  addPathRule(PathRuleAction::Include, DEFAULT_RULES[0]);
  for (size_t i = 1; i < std::size(DEFAULT_RULES); ++i) {
    addPathRule(PathRuleAction::Exclude, DEFAULT_RULES[i]);
  }
}

Direction SyncPolicy::direction(const Category category) const {
  const size_t index = categoryIndex(category);
  return index < directions_.size() ? directions_[index] : Direction::Disabled;
}

bool SyncPolicy::setDirection(const Category category, const Direction directionValue) {
  const size_t index = categoryIndex(category);
  if (index >= directions_.size() || directionValue > Direction::Bidirectional) return false;
  directions_[index] = directionValue;
  return true;
}

void SyncPolicy::clearPathRules() {
  for (auto& rule : rules_) {
    rule.action = PathRuleAction::Exclude;
    rule.pattern[0] = '\0';
  }
  ruleCount_ = 0;
}

bool SyncPolicy::addPathRule(const PathRuleAction action, const char* pattern) {
  if (ruleCount_ >= rules_.size() || action > PathRuleAction::Include || !isValidPathPattern(pattern)) return false;
  PathRule& rule = rules_[ruleCount_++];
  rule.action = action;
  std::strncpy(rule.pattern, pattern, sizeof(rule.pattern) - 1);
  rule.pattern[sizeof(rule.pattern) - 1] = '\0';
  return true;
}

bool SyncPolicy::isValidPathPattern(const char* pattern) { return patternIsSafe(pattern); }

bool SyncPolicy::isNormalizedAbsolutePath(const char* path) {
  if (path == nullptr || path[0] != '/') return false;
  const size_t length = strnlen(path, MAX_SYNC_PATH_BYTES + 1);
  if (length == 0 || length > MAX_SYNC_PATH_BYTES || (length > 1 && path[length - 1] == '/')) return false;
  if (length == 1) return true;

  size_t segmentStart = 1;
  for (size_t i = 1; i <= length; ++i) {
    const char value = path[i];
    if (value == '\\' || (value != '\0' && (static_cast<uint8_t>(value) < 0x20 || value == 0x7F))) return false;
    if (value != '/' && value != '\0') continue;
    if (i == segmentStart) return false;
    const size_t segmentLength = i - segmentStart;
    if ((segmentLength == 1 && path[segmentStart] == '.') ||
        (segmentLength == 2 && path[segmentStart] == '.' && path[segmentStart + 1] == '.')) {
      return false;
    }
    segmentStart = i + 1;
  }
  return true;
}

bool SyncPolicy::pathAllowed(const char* normalizedPath) const {
  if (!isNormalizedAbsolutePath(normalizedPath)) return false;
  bool allowed = false;
  for (size_t i = 0; i < ruleCount_; ++i) {
    if (globMatches(rules_[i].pattern, normalizedPath)) {
      allowed = rules_[i].action == PathRuleAction::Include;
    }
  }
  return allowed;
}

bool SyncPolicy::allowsOutbound(const Category category, const char* normalizedPath) const {
  return DeviceSync::allowsOutbound(direction(category)) && (normalizedPath == nullptr || pathAllowed(normalizedPath));
}

bool SyncPolicy::allowsInbound(const Category category, const char* normalizedPath) const {
  return DeviceSync::allowsInbound(direction(category)) && (normalizedPath == nullptr || pathAllowed(normalizedPath));
}

bool SyncPolicy::operationAllowed(const SyncPolicy& sender, const SyncPolicy& receiver, const Category category,
                                  const char* normalizedPath) {
  return sender.allowsOutbound(category, normalizedPath) && receiver.allowsInbound(category, normalizedPath);
}

}  // namespace DeviceSync

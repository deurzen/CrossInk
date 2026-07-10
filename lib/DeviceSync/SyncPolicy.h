#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "DeviceSyncTypes.h"

namespace DeviceSync {

constexpr size_t MAX_POLICY_RULES = 12;
constexpr size_t MAX_POLICY_PATTERN_BYTES = 80;
constexpr size_t MAX_SYNC_PATH_BYTES = 512;

enum class PathRuleAction : uint8_t {
  Exclude,
  Include,
};

struct PathRule {
  PathRuleAction action = PathRuleAction::Exclude;
  char pattern[MAX_POLICY_PATTERN_BYTES] = {};
};

// Fixed at roughly 1 KiB; keep it in activity-owned storage rather than a task stack.
class SyncPolicy {
 public:
  SyncPolicy();

  void reset();
  void setDefaults();

  Direction direction(Category category) const;
  bool setDirection(Category category, Direction direction);

  void clearPathRules();
  bool addPathRule(PathRuleAction action, const char* pattern);
  size_t pathRuleCount() const { return ruleCount_; }
  const PathRule* pathRule(size_t index) const { return index < ruleCount_ ? &rules_[index] : nullptr; }

  bool mirrorDeletions() const { return mirrorDeletions_; }
  void setMirrorDeletions(const bool enabled) { mirrorDeletions_ = enabled; }

  bool pathAllowed(const char* normalizedPath) const;
  bool allowsOutbound(Category category, const char* normalizedPath = nullptr) const;
  bool allowsInbound(Category category, const char* normalizedPath = nullptr) const;

  static bool operationAllowed(const SyncPolicy& sender, const SyncPolicy& receiver, Category category,
                               const char* normalizedPath = nullptr);
  static bool isNormalizedAbsolutePath(const char* path);

 private:
  std::array<Direction, CATEGORY_COUNT> directions_{};
  std::array<PathRule, MAX_POLICY_RULES> rules_{};
  uint8_t ruleCount_ = 0;
  bool mirrorDeletions_ = false;
};

}  // namespace DeviceSync

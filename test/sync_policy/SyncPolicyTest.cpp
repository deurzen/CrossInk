#include <SyncPolicy.h>
#include <gtest/gtest.h>

namespace {
using namespace DeviceSync;

TEST(SyncPolicyTest, DefaultsEnablePortableReadingCategoriesOnly) {
  SyncPolicy policy;
  policy.setDefaults();

  EXPECT_EQ(policy.direction(Category::BookContent), Direction::Bidirectional);
  EXPECT_EQ(policy.direction(Category::EpubProgress), Direction::Bidirectional);
  EXPECT_EQ(policy.direction(Category::Bookmarks), Direction::Bidirectional);
  EXPECT_EQ(policy.direction(Category::Clippings), Direction::Bidirectional);
  EXPECT_EQ(policy.direction(Category::PortableBookSettings), Direction::Bidirectional);
  EXPECT_EQ(policy.direction(Category::FinishedState), Direction::Bidirectional);

  EXPECT_EQ(policy.direction(Category::ReadingStats), Direction::Disabled);
  EXPECT_EQ(policy.direction(Category::RecentBooks), Direction::Disabled);
  EXPECT_EQ(policy.direction(Category::GlobalReaderPreferences), Direction::Disabled);
  EXPECT_EQ(policy.direction(Category::OpdsDefinitions), Direction::Disabled);
  EXPECT_EQ(policy.direction(Category::FontPackages), Direction::Disabled);
  EXPECT_EQ(policy.direction(Category::SleepImages), Direction::Disabled);
  EXPECT_EQ(policy.direction(Category::Screenshots), Direction::Disabled);
}

TEST(SyncPolicyTest, DefaultsExcludePrivateAndInternalPaths) {
  SyncPolicy policy;
  policy.setDefaults();

  EXPECT_TRUE(policy.pathAllowed("/Books/Novel.epub"));
  EXPECT_TRUE(policy.pathAllowed("/Novel.epub"));
  EXPECT_FALSE(policy.pathAllowed("/.crosspoint"));
  EXPECT_FALSE(policy.pathAllowed("/.crosspoint/state.json"));
  EXPECT_FALSE(policy.pathAllowed("/.fonts"));
  EXPECT_FALSE(policy.pathAllowed("/.fonts/reader.cpfont"));
  EXPECT_FALSE(policy.pathAllowed("/fonts/reader.cpfont"));
  EXPECT_FALSE(policy.pathAllowed("/.sleep/cover.png"));
  EXPECT_FALSE(policy.pathAllowed("/sleep/cover.png"));
  EXPECT_FALSE(policy.pathAllowed("/.crossink-stats-backup/stats.bin"));
  EXPECT_FALSE(policy.pathAllowed("/.device-sync-item.part"));
  EXPECT_FALSE(policy.pathAllowed("/Books/.device-sync-item.part"));
}

TEST(SyncPolicyTest, LaterMatchingRuleOverridesEarlierRule) {
  SyncPolicy policy;
  policy.setDefaults();
  ASSERT_TRUE(policy.addPathRule(PathRuleAction::Include, "/sleep/shared/**"));

  EXPECT_FALSE(policy.pathAllowed("/sleep/private/cover.png"));
  EXPECT_TRUE(policy.pathAllowed("/sleep/shared/cover.png"));
}

TEST(SyncPolicyTest, SingleStarDoesNotCrossDirectoriesAndDoubleStarDoes) {
  SyncPolicy policy;
  policy.clearPathRules();
  ASSERT_TRUE(policy.addPathRule(PathRuleAction::Include, "/Books/*.epub"));

  EXPECT_TRUE(policy.pathAllowed("/Books/Novel.epub"));
  EXPECT_FALSE(policy.pathAllowed("/Books/Series/Novel.epub"));

  ASSERT_TRUE(policy.addPathRule(PathRuleAction::Include, "/Books/**"));
  EXPECT_TRUE(policy.pathAllowed("/Books/Series/Novel.epub"));
}

TEST(SyncPolicyTest, PathMatchingUsesFatStyleAsciiCaseFolding) {
  SyncPolicy policy;
  policy.clearPathRules();
  ASSERT_TRUE(policy.addPathRule(PathRuleAction::Include, "/Books/*.EPUB"));

  EXPECT_TRUE(policy.pathAllowed("/books/novel.epub"));
}

TEST(SyncPolicyTest, RejectsUnsafeOrNonCanonicalPathsAndRules) {
  EXPECT_FALSE(SyncPolicy::isNormalizedAbsolutePath(nullptr));
  EXPECT_FALSE(SyncPolicy::isNormalizedAbsolutePath("Books/Novel.epub"));
  EXPECT_FALSE(SyncPolicy::isNormalizedAbsolutePath("/Books//Novel.epub"));
  EXPECT_FALSE(SyncPolicy::isNormalizedAbsolutePath("/Books/../secret"));
  EXPECT_FALSE(SyncPolicy::isNormalizedAbsolutePath("/Books/./Novel.epub"));
  EXPECT_FALSE(SyncPolicy::isNormalizedAbsolutePath("/Books\\Novel.epub"));
  EXPECT_FALSE(SyncPolicy::isNormalizedAbsolutePath("/Books/Novel.epub/"));
  EXPECT_TRUE(SyncPolicy::isNormalizedAbsolutePath("/Books/Novel.epub"));

  SyncPolicy policy;
  EXPECT_FALSE(policy.addPathRule(PathRuleAction::Include, "relative/**"));
  EXPECT_FALSE(policy.addPathRule(PathRuleAction::Include, "/Books/../**"));
  EXPECT_FALSE(policy.addPathRule(PathRuleAction::Include, "/Books//**"));
}

TEST(SyncPolicyTest, EnforcesBothPeersDirectionAndPathPolicy) {
  SyncPolicy sender;
  sender.setDefaults();
  SyncPolicy receiver;
  receiver.setDefaults();
  ASSERT_TRUE(sender.setDirection(Category::BookContent, Direction::SendOnly));
  ASSERT_TRUE(receiver.setDirection(Category::BookContent, Direction::ReceiveOnly));

  EXPECT_TRUE(SyncPolicy::operationAllowed(sender, receiver, Category::BookContent, "/Books/Novel.epub"));
  EXPECT_FALSE(SyncPolicy::operationAllowed(sender, receiver, Category::BookContent, "/.crosspoint/cache.bin"));

  ASSERT_TRUE(receiver.setDirection(Category::BookContent, Direction::SendOnly));
  EXPECT_FALSE(SyncPolicy::operationAllowed(sender, receiver, Category::BookContent, "/Books/Novel.epub"));
}

TEST(SyncPolicyTest, RejectsInvalidCategoryAndDirectionValues) {
  SyncPolicy policy;
  EXPECT_FALSE(policy.setDirection(Category::Count, Direction::Bidirectional));
  EXPECT_FALSE(policy.setDirection(Category::BookContent, static_cast<Direction>(0xFF)));
  EXPECT_EQ(policy.direction(Category::Count), Direction::Disabled);
}

TEST(SyncPolicyTest, BoundsRuleStorage) {
  SyncPolicy policy;
  policy.clearPathRules();
  for (size_t i = 0; i < MAX_POLICY_RULES; ++i) {
    ASSERT_TRUE(policy.addPathRule(PathRuleAction::Include, "/**"));
  }
  EXPECT_FALSE(policy.addPathRule(PathRuleAction::Include, "/Books/**"));
  EXPECT_EQ(policy.pathRuleCount(), MAX_POLICY_RULES);
  EXPECT_NE(policy.pathRule(MAX_POLICY_RULES - 1), nullptr);
  EXPECT_EQ(policy.pathRule(MAX_POLICY_RULES), nullptr);
}

}  // namespace

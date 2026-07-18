#include <gtest/gtest.h>

#include <cstdint>

#include "DictionaryNavigation.h"

namespace {
using namespace dictionary::navigation;

constexpr uint8_t bit(const Action action) { return static_cast<uint8_t>(1U << static_cast<uint8_t>(action)); }

struct Releases {
  uint8_t mask = 0;
  uint8_t queried = 0;
};

bool released(void* context, const Action action) {
  auto& state = *static_cast<Releases*>(context);
  state.queried |= bit(action);
  return (state.mask & bit(action)) != 0;
}

}  // namespace

TEST(DictionaryNavigation, EnforcesOneActionPriority) {
  Releases all{0xFEU, 0};
  EXPECT_EQ(firstReleased(&all, released), Action::Back);
  EXPECT_EQ(all.queried, bit(Action::Back));

  Releases confirmAndDown{static_cast<uint8_t>(bit(Action::Confirm) | bit(Action::Down)), 0};
  EXPECT_EQ(firstReleased(&confirmAndDown, released), Action::Confirm);
  EXPECT_EQ(confirmAndDown.queried, static_cast<uint8_t>(bit(Action::Back) | bit(Action::Confirm)));

  Releases down{bit(Action::Down), 0};
  EXPECT_EQ(firstReleased(&down, released), Action::Down);
  EXPECT_EQ(down.queried, 0x7EU);
  EXPECT_EQ(firstReleased(nullptr, nullptr), Action::None);
}

TEST(DictionaryNavigation, MapsEveryActionInEveryMode) {
  EXPECT_EQ(effectFor(Mode::Shortlist, Action::Back, false), Effect::Finish);
  EXPECT_EQ(effectFor(Mode::Shortlist, Action::Confirm, false), Effect::OpenDefinition);
  for (const Action action : {Action::Left, Action::Right, Action::Up, Action::Down}) {
    EXPECT_EQ(effectFor(Mode::Shortlist, action, false), Effect::MoveShortlist);
  }

  EXPECT_EQ(effectFor(Mode::Definition, Action::Back, false), Effect::ReturnToShortlist);
  EXPECT_EQ(effectFor(Mode::Definition, Action::Confirm, false), Effect::OpenStatus);
  EXPECT_EQ(effectFor(Mode::Definition, Action::Confirm, true), Effect::None);
  EXPECT_EQ(effectFor(Mode::Definition, Action::Left, false), Effect::PreviousPage);
  EXPECT_EQ(effectFor(Mode::Definition, Action::Right, false), Effect::NextPage);
  EXPECT_EQ(effectFor(Mode::Definition, Action::Up, true), Effect::PreviousWord);
  EXPECT_EQ(effectFor(Mode::Definition, Action::Down, true), Effect::NextWord);

  EXPECT_EQ(effectFor(Mode::Status, Action::Back, false), Effect::CancelStatus);
  EXPECT_EQ(effectFor(Mode::Status, Action::Confirm, false), Effect::SaveStatus);
  for (const Action action : {Action::Left, Action::Right, Action::Up, Action::Down}) {
    EXPECT_EQ(effectFor(Mode::Status, action, false), Effect::MoveStatus);
  }
  EXPECT_EQ(effectFor(Mode::Status, Action::None, false), Effect::None);
}

TEST(DictionaryNavigation, MovesShortlistAndStatusWithFrozenWrapRules) {
  EXPECT_EQ(moveShortlistSelection(0, 3, 2, Action::Left), 2);
  EXPECT_EQ(moveShortlistSelection(2, 3, 2, Action::Right), 0);
  EXPECT_EQ(moveShortlistSelection(2, 6, 2, Action::Up), 0);
  EXPECT_EQ(moveShortlistSelection(5, 6, 2, Action::Down), 5);
  EXPECT_EQ(moveShortlistSelection(1, 3, 2, Action::Confirm), 1);

  EXPECT_EQ(moveStatusSelection(0, Action::Left), 2);
  EXPECT_EQ(moveStatusSelection(0, Action::Up), 2);
  EXPECT_EQ(moveStatusSelection(2, Action::Right), 0);
  EXPECT_EQ(moveStatusSelection(2, Action::Down), 0);
  EXPECT_EQ(moveStatusSelection(1, Action::Confirm), 1);
}

TEST(DictionaryNavigation, KeepsDefinitionPagingIndependentOfWordMovement) {
  EXPECT_EQ(definitionPageMove(false, 0, true, Action::Left), PageMove::None);
  EXPECT_EQ(definitionPageMove(false, 1, true, Action::Left), PageMove::Previous);
  EXPECT_EQ(definitionPageMove(false, 1, true, Action::Right), PageMove::Next);
  EXPECT_EQ(definitionPageMove(false, 1, false, Action::Right), PageMove::None);
  EXPECT_EQ(definitionPageMove(true, 1, true, Action::Left), PageMove::None);
  EXPECT_EQ(definitionPageMove(true, 1, true, Action::Right), PageMove::None);
  EXPECT_EQ(definitionPageMove(false, 1, true, Action::Down), PageMove::None);
}

TEST(DictionaryNavigation, NavigatesThreeWordsAndWrapsAtBothEdges) {
  EXPECT_EQ(moveWord(1, 3, Action::Up).selection, 0);
  EXPECT_EQ(moveWord(1, 3, Action::Down).selection, 2);
  EXPECT_EQ(moveWord(0, 3, Action::Up).selection, 2);
  EXPECT_EQ(moveWord(2, 3, Action::Down).selection, 0);
  EXPECT_TRUE(moveWord(2, 3, Action::Down).changed);

  const WordMove oneUp = moveWord(0, 1, Action::Up);
  const WordMove oneDown = moveWord(0, 1, Action::Down);
  EXPECT_FALSE(oneUp.changed);
  EXPECT_FALSE(oneDown.changed);
  EXPECT_EQ(oneUp.selection, 0);
  EXPECT_EQ(oneDown.selection, 0);
}

TEST(DictionaryNavigation, PreservesDirectionAfterCurrentItemRemoval) {
  EXPECT_EQ(moveAfterCurrentRemoval(1, 2, Action::Down).selection, 1);  // [A,B,C] saved B -> C
  EXPECT_EQ(moveAfterCurrentRemoval(1, 2, Action::Up).selection, 0);    // [A,B,C] saved B -> A
  EXPECT_EQ(moveAfterCurrentRemoval(0, 2, Action::Down).selection, 0);  // saved A -> B
  EXPECT_EQ(moveAfterCurrentRemoval(0, 2, Action::Up).selection, 1);    // saved A -> C
  EXPECT_EQ(moveAfterCurrentRemoval(2, 2, Action::Down).selection, 0);  // saved C -> A
  EXPECT_EQ(moveAfterCurrentRemoval(2, 2, Action::Up).selection, 1);    // saved C -> B

  const WordMove only = moveAfterCurrentRemoval(0, 0, Action::Down);
  EXPECT_TRUE(only.finish);
  EXPECT_FALSE(only.changed);
}

#include "DictionaryNavigation.h"

#include <algorithm>

namespace dictionary::navigation {
namespace {

bool isPreviousWord(const Action action) { return action == Action::Up; }
bool isNextWord(const Action action) { return action == Action::Down; }

}  // namespace

Effect effectFor(const Mode mode, const Action action, const bool definitionFailed) {
  if (action == Action::None) return Effect::None;
  if (action == Action::Back) {
    if (mode == Mode::Shortlist) return Effect::Finish;
    return mode == Mode::Status ? Effect::CancelStatus : Effect::ReturnToShortlist;
  }
  if (mode == Mode::Shortlist) {
    return action == Action::Confirm ? Effect::OpenDefinition : Effect::MoveShortlist;
  }
  if (mode == Mode::Status) {
    return action == Action::Confirm ? Effect::SaveStatus : Effect::MoveStatus;
  }
  switch (action) {
    case Action::Confirm:
      return definitionFailed ? Effect::None : Effect::OpenStatus;
    case Action::Left:
      return Effect::PreviousPage;
    case Action::Right:
      return Effect::NextPage;
    case Action::Up:
      return Effect::PreviousWord;
    case Action::Down:
      return Effect::NextWord;
    case Action::None:
    case Action::Back:
      return Effect::None;
  }
  return Effect::None;
}

Action firstReleased(void* context, const ReleasedReader reader) {
  if (!reader) return Action::None;
  constexpr Action priority[] = {Action::Back, Action::Confirm, Action::Left, Action::Right, Action::Up, Action::Down};
  for (const Action action : priority) {
    if (reader(context, action)) return action;
  }
  return Action::None;
}

uint16_t moveShortlistSelection(const uint16_t current, const uint16_t count, const uint16_t rowsPerPage,
                                const Action action) {
  if (count == 0 || current >= count) return current;
  switch (action) {
    case Action::Left:
      return current == 0 ? static_cast<uint16_t>(count - 1) : static_cast<uint16_t>(current - 1);
    case Action::Right:
      return static_cast<uint16_t>((current + 1U) % count);
    case Action::Up:
      return current > rowsPerPage ? static_cast<uint16_t>(current - rowsPerPage) : 0;
    case Action::Down:
      return static_cast<uint16_t>(std::min<uint32_t>(count - 1U, current + rowsPerPage));
    case Action::None:
    case Action::Back:
    case Action::Confirm:
      return current;
  }
  return current;
}

uint8_t moveStatusSelection(const uint8_t current, const Action action) {
  constexpr uint8_t count = 3;
  if (current >= count) return current;
  if (action == Action::Left || action == Action::Up) {
    return current == 0 ? count - 1U : current - 1U;
  }
  if (action == Action::Right || action == Action::Down) {
    return static_cast<uint8_t>((current + 1U) % count);
  }
  return current;
}

PageMove definitionPageMove(const bool failed, const uint32_t pageIndex, const bool hasNext, const Action action) {
  if (failed) return PageMove::None;
  if (action == Action::Left && pageIndex > 0) return PageMove::Previous;
  if (action == Action::Right && hasNext) return PageMove::Next;
  return PageMove::None;
}

WordMove moveWord(const uint16_t current, const uint16_t count, const Action action) {
  WordMove result{current, false, false};
  if ((!isPreviousWord(action) && !isNextWord(action)) || current >= count || count <= 1) return result;
  result.changed = true;
  if (isNextWord(action)) {
    result.selection = static_cast<uint16_t>((current + 1U) % count);
  } else {
    result.selection = current == 0 ? static_cast<uint16_t>(count - 1) : static_cast<uint16_t>(current - 1);
  }
  return result;
}

WordMove moveAfterCurrentRemoval(const uint16_t oldIndex, const uint16_t filteredCount, const Action action) {
  WordMove result{oldIndex, false, filteredCount == 0};
  if (result.finish || (!isPreviousWord(action) && !isNextWord(action))) return result;
  result.changed = true;
  if (isNextWord(action)) {
    result.selection = oldIndex < filteredCount ? oldIndex : 0;
  } else {
    result.selection = oldIndex == 0 ? static_cast<uint16_t>(filteredCount - 1) : static_cast<uint16_t>(oldIndex - 1);
  }
  return result;
}

}  // namespace dictionary::navigation

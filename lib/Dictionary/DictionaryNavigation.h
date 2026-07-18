#pragma once

#include <cstddef>
#include <cstdint>

namespace dictionary::navigation {

enum class Action : uint8_t { None = 0, Back, Confirm, Left, Right, Up, Down };

using ReleasedReader = bool (*)(void* context, Action action);

enum class Mode : uint8_t { Shortlist = 0, Definition, Status };
enum class Effect : uint8_t {
  None = 0,
  Finish,
  OpenDefinition,
  ReturnToShortlist,
  CancelStatus,
  SaveStatus,
  MoveShortlist,
  MoveStatus,
  OpenStatus,
  PreviousPage,
  NextPage,
  PreviousWord,
  NextWord,
};

Effect effectFor(Mode mode, Action action, bool definitionFailed);

// Reads in the frozen Back, Confirm, Left, Right, Up, Down priority order and
// stops after the first release so lower-priority actions remain untouched.
Action firstReleased(void* context, ReleasedReader reader);

uint16_t moveShortlistSelection(uint16_t current, uint16_t count, uint16_t rowsPerPage, Action action);
uint8_t moveStatusSelection(uint8_t current, Action action);

enum class PageMove : uint8_t { None = 0, Previous, Next };
PageMove definitionPageMove(bool failed, uint32_t pageIndex, bool hasNext, Action action);

struct WordMove {
  uint16_t selection = 0;
  bool changed = false;
  bool finish = false;
};

WordMove moveWord(uint16_t current, uint16_t count, Action action);
WordMove moveAfterCurrentRemoval(uint16_t oldIndex, uint16_t filteredCount, Action action);

}  // namespace dictionary::navigation

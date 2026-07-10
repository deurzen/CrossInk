#include "DictionaryActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kSideMargin = 20;
constexpr int kHeaderY = 15;
constexpr int kListTop = 58;
constexpr int kRowHeight = 34;
constexpr int kDefinitionLineGap = 3;
constexpr int kBottomReserved = 48;

const char* statusLabel(const uint8_t index) {
  switch (index) {
    case 0:
      return tr(STR_LEARNING);
    case 1:
      return tr(STR_KNOWN);
    case 2:
      return tr(STR_IGNORE);
    default:
      return "";
  }
}

dictionary::lexeme_state::Status statusValue(const uint8_t index) {
  switch (index) {
    case 0:
      return dictionary::lexeme_state::Status::Learning;
    case 1:
      return dictionary::lexeme_state::Status::Known;
    case 2:
      return dictionary::lexeme_state::Status::Ignored;
    default:
      return dictionary::lexeme_state::Status::Unseen;
  }
}

}  // namespace

DictionaryActivity::DictionaryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       std::unique_ptr<dictionary::lookup::Session> session,
                                       std::unique_ptr<dictionary::page_shortlist::Shortlist> shortlist,
                                       std::unique_ptr<uint8_t[]> suppressionBitset)
    : Activity("Dictionary", renderer, mappedInput),
      session_(std::move(session)),
      shortlist_(std::move(shortlist)),
      suppressionBitset_(std::move(suppressionBitset)) {}

void DictionaryActivity::onEnter() {
  Activity::onEnter();
  if (!session_ || !shortlist_ || shortlist_->count == 0) {
    LOG_ERR("DICT", "Dictionary activity entered without a shortlist");
    finish();
    return;
  }
  requestUpdate();
}

void DictionaryActivity::onExit() {
  definitionPage_.reset();
  pager_.reset();
  shortlist_.reset();
  session_.reset();
  suppressionBitset_.reset();
  Activity::onExit();
}

void DictionaryActivity::contentMargins(int& top, int& right, int& bottom, int& left) const {
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
}

int DictionaryActivity::shortlistRowsPerPage() const {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  contentMargins(top, right, bottom, left);
  (void)right;
  (void)left;
  return std::max(1, (renderer.getScreenHeight() - bottom - (top + kListTop) - kBottomReserved) / kRowHeight);
}

int DictionaryActivity::definitionContentWidth() const {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  contentMargins(top, right, bottom, left);
  (void)top;
  (void)bottom;
  return std::max(1, renderer.getScreenWidth() - left - right - 2 * kSideMargin);
}

uint16_t DictionaryActivity::selectedLocalLemmaId() const {
  if (!shortlist_ || selected_ >= shortlist_->count) return UINT16_MAX;
  const auto& item = shortlist_->items[selected_];
  if (analysisIndex_ >= item.analysisCount) return UINT16_MAX;
  return item.localLemmaIds[analysisIndex_];
}

int DictionaryActivity::measureDefinitionText(void* context, const std::string_view text) {
  auto& activity = *static_cast<DictionaryActivity*>(context);
  if (text.size() >= sizeof(activity.lineScratch_)) return INT_MAX;
  std::memcpy(activity.lineScratch_, text.data(), text.size());
  activity.lineScratch_[text.size()] = '\0';
  return activity.renderer.getTextAdvanceX(UI_10_FONT_ID, activity.lineScratch_, EpdFontFamily::REGULAR);
}

bool DictionaryActivity::openDefinition() {
  definitionFailed_ = false;
  statusSaved_ = false;
  const uint16_t localLemmaId = selectedLocalLemmaId();
  uint32_t globalLexemeId = 0;
  dictionary::PackageError packageError = dictionary::PackageError::NONE;
  size_t headwordLength = 0;
  if (localLemmaId == UINT16_MAX || !session_->globalLexemeId(localLemmaId, globalLexemeId) ||
      !session_->package().readLexeme(globalLexemeId, lexeme_, packageError) ||
      !session_->package().readHeadword(lexeme_, headword_, sizeof(headword_), headwordLength, packageError) ||
      !session_->package().getEntrySlice(lexeme_, entry_, packageError)) {
    LOG_ERR("DICT", "Definition lookup failed: %s", dictionary::packageErrorName(packageError));
    definitionFailed_ = true;
    mode_ = Mode::Definition;
    requestUpdate();
    return false;
  }

  if (!pager_) {
    // 644-byte wrapping workspace is retained and reused in definition mode;
    // stack/static storage would exceed the reader task budget or add BSS.
    pager_ = makeUniqueNoThrow<dictionary::definition::Pager>();
  }
  if (!definitionPage_) {
    // One 4.4 KB rendered definition page is retained; the full entry can be
    // 1 MiB and is therefore streamed instead of allocated.
    definitionPage_ = makeUniqueNoThrow<dictionary::definition::Page>();
  }
  if (!pager_ || !definitionPage_) {
    LOG_ERR("DICT", "OOM: definition pager/page (%u bytes)",
            static_cast<unsigned>(sizeof(dictionary::definition::Pager) + sizeof(dictionary::definition::Page)));
    definitionFailed_ = true;
    mode_ = Mode::Definition;
    requestUpdate();
    return false;
  }

  definitionPageStart_ = {};
  definitionPageIndex_ = 0;
  mode_ = Mode::Definition;
  return loadDefinitionPage(definitionPageStart_, 0);
}

bool DictionaryActivity::loadDefinitionPage(const dictionary::definition::Cursor& start, const uint32_t pageIndex) {
  if (!pager_ || !definitionPage_) return false;
  dictionary::definition::PagerError error = dictionary::definition::PagerError::NONE;
  const dictionary::definition::WidthMeasurer measurer{this, measureDefinitionText};
  const int lineStep = renderer.getLineHeight(UI_10_FONT_ID) + kDefinitionLineGap;
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  contentMargins(top, right, bottom, left);
  (void)right;
  (void)left;
  const size_t visibleLines = static_cast<size_t>(
      std::max(1, (renderer.getScreenHeight() - bottom - (top + kListTop) - kBottomReserved) / std::max(1, lineStep)));
  if (!pager_->load(session_->package(), entry_, start, measurer, definitionContentWidth(),
                    std::min(visibleLines, dictionary::definition::kMaxPageLines), *definitionPage_, error)) {
    LOG_ERR("DICT", "Definition page failed: %s", dictionary::definition::pagerErrorName(error));
    definitionFailed_ = true;
    requestUpdate();
    return false;
  }
  definitionFailed_ = false;
  definitionPageStart_ = start;
  definitionPageIndex_ = pageIndex;
  requestUpdate();
  return true;
}

void DictionaryActivity::changeDefinitionPage(const int delta) {
  if (definitionFailed_ || !definitionPage_) return;
  if (delta > 0 && definitionPage_->hasNext) {
    loadDefinitionPage(definitionPage_->next, definitionPageIndex_ + 1);
    return;
  }
  if (delta >= 0 || definitionPageIndex_ == 0) return;

  // Backward navigation replays bounded pages from the entry start. This keeps
  // memory independent of definition length; only explicit reverse navigation
  // pays the additional sequential SD reads.
  const uint32_t targetPage = definitionPageIndex_ - 1;
  dictionary::definition::Cursor cursor{};
  for (uint32_t page = 0; page <= targetPage; ++page) {
    if (!loadDefinitionPage(cursor, page)) return;
    if (page < targetPage) {
      if (!definitionPage_->hasNext) return;
      cursor = definitionPage_->next;
    }
  }
}

void DictionaryActivity::changeAnalysis(const int delta) {
  if (!shortlist_ || selected_ >= shortlist_->count) return;
  const uint8_t count = shortlist_->items[selected_].analysisCount;
  if (count < 2) return;
  analysisIndex_ = static_cast<uint8_t>((analysisIndex_ + count + delta) % count);
  openDefinition();
}

void DictionaryActivity::saveSelectedStatus() {
  dictionary::lookup::SessionError error = dictionary::lookup::SessionError::NONE;
  if (!session_->setStatus(selectedLocalLemmaId(), statusValue(statusSelection_), error)) {
    LOG_ERR("DICT", "Status update failed: %s", dictionary::lookup::sessionErrorName(error));
    statusSaved_ = false;
    mode_ = Mode::Definition;
    definitionFailed_ = true;
    requestUpdate();
    return;
  }
  statusSaved_ = true;
  mode_ = Mode::Definition;
  requestUpdate();
}

void DictionaryActivity::returnToShortlist() {
  session_->projection().filter(*shortlist_);
  if (shortlist_->count == 0) {
    finish();
    return;
  }
  if (selected_ >= shortlist_->count) selected_ = static_cast<uint16_t>(shortlist_->count - 1);
  analysisIndex_ = 0;
  mode_ = Mode::Shortlist;
  requestUpdate();
}

void DictionaryActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mode_ == Mode::Shortlist) {
      finish();
    } else if (mode_ == Mode::Status) {
      mode_ = Mode::Definition;
      requestUpdate();
    } else {
      returnToShortlist();
    }
    return;
  }

  if (mode_ == Mode::Shortlist) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openDefinition();
      return;
    }
    const bool previous = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                          mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                      mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (previous) {
      selected_ = selected_ == 0 ? static_cast<uint16_t>(shortlist_->count - 1) : selected_ - 1;
      analysisIndex_ = 0;
      requestUpdate();
    } else if (next) {
      selected_ = static_cast<uint16_t>((selected_ + 1) % shortlist_->count);
      analysisIndex_ = 0;
      requestUpdate();
    }
    return;
  }

  if (mode_ == Mode::Status) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      saveSelectedStatus();
    } else {
      const bool previous = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
      const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Right);
      if (previous) {
        statusSelection_ = statusSelection_ == 0 ? 2 : statusSelection_ - 1;
        requestUpdate();
      } else if (next) {
        statusSelection_ = static_cast<uint8_t>((statusSelection_ + 1) % 3);
        requestUpdate();
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !definitionFailed_) {
    mode_ = Mode::Status;
    statusSelection_ = 0;
    requestUpdate();
    return;
  }
  const bool previous = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                    mappedInput.wasReleased(MappedInputManager::Button::Right);
  if (previous) {
    if (mappedInput.getHeldTime() >= ReaderUtils::SKIP_HOLD_MS) {
      changeAnalysis(-1);
    } else {
      changeDefinitionPage(-1);
    }
  } else if (next) {
    if (mappedInput.getHeldTime() >= ReaderUtils::SKIP_HOLD_MS) {
      changeAnalysis(1);
    } else {
      changeDefinitionPage(1);
    }
  }
}

void DictionaryActivity::renderShortlist() {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  contentMargins(top, right, bottom, left);
  (void)bottom;
  char header[48]{};
  std::snprintf(header, sizeof(header), "%s · %u/%u", tr(STR_UNKNOWN_WORDS), static_cast<unsigned>(selected_ + 1),
                static_cast<unsigned>(shortlist_->count));
  renderer.drawText(UI_12_FONT_ID, left + kSideMargin, top + kHeaderY, header, true, EpdFontFamily::BOLD);

  const int rows = shortlistRowsPerPage();
  const int pageStart = (selected_ / rows) * rows;
  for (int row = 0; row < rows; ++row) {
    const int index = pageStart + row;
    if (index >= shortlist_->count) break;
    const int y = top + kListTop + row * kRowHeight;
    const bool selected = index == selected_;
    if (selected) renderer.fillRect(left, y, renderer.getScreenWidth() - left - right, kRowHeight, true);
    const std::string_view surface = shortlist_->surface(index);
    const size_t length = std::min(surface.size(), sizeof(lineScratch_) - 1);
    std::memcpy(lineScratch_, surface.data(), length);
    lineScratch_[length] = '\0';
    renderer.drawText(UI_10_FONT_ID, left + kSideMargin, y + 5, lineScratch_, !selected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}

void DictionaryActivity::renderDefinition() {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  contentMargins(top, right, bottom, left);
  char header[160]{};
  uint8_t analyses = 1;
  if (shortlist_ && selected_ < shortlist_->count) analyses = shortlist_->items[selected_].analysisCount;
  if (analyses > 1) {
    std::snprintf(header, sizeof(header), "%s · %u/%u", headword_, static_cast<unsigned>(analysisIndex_ + 1),
                  static_cast<unsigned>(analyses));
  } else {
    std::snprintf(header, sizeof(header), "%s", headword_);
  }
  renderer.drawText(UI_12_FONT_ID, left + kSideMargin, top + kHeaderY, header, true, EpdFontFamily::BOLD);

  if (definitionFailed_) {
    renderer.drawText(UI_10_FONT_ID, left + kSideMargin, top + kListTop, tr(STR_DICTIONARY_LOOKUP_FAILED));
  } else if (definitionPage_) {
    int y = top + kListTop;
    const int lineStep = renderer.getLineHeight(UI_10_FONT_ID) + kDefinitionLineGap;
    for (uint8_t index = 0; index < definitionPage_->lineCount; ++index) {
      const auto text = definitionPage_->lineText(index);
      const size_t length = std::min(text.size(), sizeof(lineScratch_) - 1);
      std::memcpy(lineScratch_, text.data(), length);
      lineScratch_[length] = '\0';
      const bool bold = definitionPage_->lines[index].fieldType == 2;
      renderer.drawText(UI_10_FONT_ID, left + kSideMargin, y, lineScratch_, true,
                        bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      y += lineStep;
      if (y >= renderer.getScreenHeight() - bottom - kBottomReserved) break;
    }
    char pageLabel[16]{};
    std::snprintf(pageLabel, sizeof(pageLabel), "%u%s", definitionPageIndex_ + 1, definitionPage_->hasNext ? "+" : "");
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - right - kSideMargin - 25,
                      renderer.getScreenHeight() - bottom - kBottomReserved, pageLabel);
  }

  if (statusSaved_) {
    renderer.drawText(SMALL_FONT_ID, left + kSideMargin, renderer.getScreenHeight() - bottom - kBottomReserved,
                      tr(STR_WORD_STATUS_SAVED));
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DISPLAY_STATUS), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}

void DictionaryActivity::renderStatus() {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  contentMargins(top, right, bottom, left);
  (void)bottom;
  renderer.drawText(UI_12_FONT_ID, left + kSideMargin, top + kHeaderY, tr(STR_SET_WORD_STATUS), true,
                    EpdFontFamily::BOLD);
  for (uint8_t index = 0; index < 3; ++index) {
    const int y = top + kListTop + index * (kRowHeight + 4);
    const bool selected = index == statusSelection_;
    if (selected) renderer.fillRect(left, y, renderer.getScreenWidth() - left - right, kRowHeight, true);
    renderer.drawText(UI_10_FONT_ID, left + kSideMargin, y + 5, statusLabel(index), !selected);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}

void DictionaryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (mode_) {
    case Mode::Shortlist:
      renderShortlist();
      break;
    case Mode::Definition:
      renderDefinition();
      break;
    case Mode::Status:
      renderStatus();
      break;
  }
  renderer.displayBuffer();
}

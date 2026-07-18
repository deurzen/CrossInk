#include "DictionaryActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kSideMargin = 20;
constexpr int kHeaderY = 15;
constexpr int kListTop = 58;
constexpr int kRowHeight = 34;
constexpr int kDefinitionLineGap = 3;
constexpr int kDefinitionMeaningGap = 4;
constexpr int kAnalysisDividerWidth = 72;
constexpr int kAnalysisDividerHeight = 16;
constexpr int kSourceDividerHeight = 22;
constexpr int kSourceDividerGap = 8;
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

const char* modeName(const uint8_t mode) {
  switch (mode) {
    case 0:
      return "shortlist";
    case 1:
      return "definition";
    case 2:
      return "status";
    default:
      return "unknown";
  }
}

bool entryCursorAtStart(const dictionary::definition::Cursor& cursor) {
  return cursor.fieldHeaderOffset == 4 && cursor.fieldIndex == 0 && cursor.fieldByteOffset == 0;
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
                                       const unsigned long lookupStartedAt)
    : Activity("Dictionary", renderer, mappedInput),
      session_(std::move(session)),
      shortlist_(std::move(shortlist)),
      lookupStartedAt_(lookupStartedAt) {}

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
  LOG_INF("DICT", "Activity resources released: total=%lu ms free=%u maxAlloc=%u stackHwm=%lu",
          millis() - lookupStartedAt_, ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          static_cast<unsigned long>(dictionary::io_metrics::currentTaskStackHighWaterBytes()));
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

const char* DictionaryActivity::definitionFailureMessage() const {
  switch (definitionFailure_) {
    case DefinitionFailure::AttachmentsInvalid:
      return tr(STR_DICTIONARY_ATTACHMENTS_INVALID);
    case DefinitionFailure::NoCompatibleSources:
      return tr(STR_NO_COMPATIBLE_DEFINITION_SOURCES);
    case DefinitionFailure::NoDefinition:
      return tr(STR_NO_ATTACHED_SOURCE_DEFINITION);
    case DefinitionFailure::SourceCorrupt:
      return tr(STR_DEFINITION_SOURCE_INVALID);
    case DefinitionFailure::None:
    case DefinitionFailure::General:
      return tr(STR_DICTIONARY_LOOKUP_FAILED);
  }
  return tr(STR_DICTIONARY_LOOKUP_FAILED);
}

int DictionaryActivity::measureDefinitionText(void* context, const std::string_view text) {
  auto& activity = *static_cast<DictionaryActivity*>(context);
  if (text.size() >= sizeof(activity.lineScratch_)) return INT_MAX;
  std::memcpy(activity.lineScratch_, text.data(), text.size());
  activity.lineScratch_[text.size()] = '\0';
  return activity.renderer.getTextAdvanceX(UI_10_FONT_ID, activity.lineScratch_, EpdFontFamily::REGULAR);
}

bool DictionaryActivity::openDefinition() {
  const unsigned long startedAt = millis();
  definitionFailed_ = false;
  contextualSourceWarning_ = false;
  definitionFailure_ = DefinitionFailure::None;
  statusSaved_ = false;
  canonicalHeadword_[0] = '\0';
  if (!session_ || !shortlist_ || selected_ >= shortlist_->count) {
    LOG_ERR("DICT", "Definition selection is invalid");
    definitionFailed_ = true;
    definitionFailure_ = DefinitionFailure::General;
    mode_ = Mode::Definition;
    requestUpdate();
    return false;
  }
  const auto ioBefore = session_->sourceIoMetrics();
  if (session_->sourceDiscoveryStatus() == dictionary::lookup::SourceDiscoveryStatus::ATTACHMENTS_INVALID) {
    definitionFailed_ = true;
    definitionFailure_ = DefinitionFailure::AttachmentsInvalid;
    mode_ = Mode::Definition;
    requestUpdate();
    return false;
  }
  if (session_->definitionSourceCount() == 0) {
    definitionFailed_ = true;
    definitionFailure_ = DefinitionFailure::NoCompatibleSources;
    mode_ = Mode::Definition;
    requestUpdate();
    return false;
  }
  contextualSourceWarning_ = session_->sourceDiscoveryStatus() == dictionary::lookup::SourceDiscoveryStatus::PARTIAL;

  const std::string_view surface = shortlist_->surface(selected_);
  const size_t headwordLength = std::min(surface.size(), sizeof(headword_) - 1);
  std::memcpy(headword_, surface.data(), headwordLength);
  headword_[headwordLength] = '\0';

  size_t canonicalHeadwordLength = 0;
  dictionary::lookup::SessionError headwordError = dictionary::lookup::SessionError::NONE;
  if (!session_->readCanonicalHeadword(shortlist_->items[selected_].localLemmaIds[0], canonicalHeadword_,
                                       sizeof(canonicalHeadword_), canonicalHeadwordLength, headwordError)) {
    LOG_ERR("DICT", "Canonical lemma unavailable: %s", dictionary::lookup::sessionErrorName(headwordError));
    canonicalHeadword_[0] = '\0';
  }

  if (!pager_) {
    // 644-byte wrapping workspace is retained and reused in definition mode;
    // stack/static storage would exceed the reader task budget or add BSS.
    pager_ = makeUniqueNoThrow<dictionary::definition::Pager>();
  }
  if (!definitionPage_) {
    // One 4.4 KB rendered definition page is retained; every analysis is
    // streamed into it sequentially instead of materializing full entries.
    definitionPage_ = makeUniqueNoThrow<dictionary::definition::Page>();
  }
  if (!pager_ || !definitionPage_) {
    LOG_ERR("DICT", "OOM: definition pager/page (%u bytes)",
            static_cast<unsigned>(sizeof(dictionary::definition::Pager) + sizeof(dictionary::definition::Page)));
    definitionFailed_ = true;
    definitionFailure_ = DefinitionFailure::General;
    mode_ = Mode::Definition;
    requestUpdate();
    return false;
  }

  definitionPageStart_ = {};
  definitionPageNext_ = {};
  contextualIndexes_ = {};
  contextualIndexCount_ = 0;
  contextualIndexAnalysis_ = UINT8_MAX;
  definitionPageIndex_ = 0;
  mode_ = Mode::Definition;
  const bool loaded = loadDefinitionPage(definitionPageStart_, 0);
  const auto io = dictionary::io_metrics::difference(session_->sourceIoMetrics(), ioBefore);
  LOG_INF("DICT",
          "Definition prepared: %lu ms opens=%lu switches=%lu seeks=%lu reads=%lu bytes=%llu free=%u "
          "maxAlloc=%u stackHwm=%lu",
          millis() - startedAt, static_cast<unsigned long>(io.openAttempts),
          static_cast<unsigned long>(io.sourceSwitches), static_cast<unsigned long>(io.seekAttempts),
          static_cast<unsigned long>(io.readCalls), static_cast<unsigned long long>(io.bytesRead), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap(), static_cast<unsigned long>(dictionary::io_metrics::currentTaskStackHighWaterBytes()));
  return loaded;
}

bool DictionaryActivity::loadContextualIndexes(const uint8_t analysisIndex) {
  if (contextualIndexAnalysis_ == analysisIndex) return true;
  if (!shortlist_ || selected_ >= shortlist_->count || analysisIndex >= shortlist_->items[selected_].analysisCount) {
    return false;
  }
  uint32_t canonicalId = 0;
  dictionary::lookup::SessionError error = dictionary::lookup::SessionError::NONE;
  if (!session_->globalLexemeId(shortlist_->items[selected_].localLemmaIds[analysisIndex], canonicalId) ||
      !session_->readDefinitionIndexes(canonicalId, contextualIndexes_, contextualIndexCount_, error)) {
    LOG_ERR("DICT", "Contextual index lookup failed: %s", dictionary::lookup::sessionErrorName(error));
    return false;
  }
  for (uint8_t sourceIndex = 0; sourceIndex < contextualIndexCount_; ++sourceIndex) {
    const auto status = contextualIndexes_[sourceIndex].status;
    if (status == dictionary::lookup::DefinitionIndexStatus::SOURCE_UNAVAILABLE ||
        status == dictionary::lookup::DefinitionIndexStatus::RECORD_INVALID) {
      contextualSourceWarning_ = true;
    }
  }
  contextualIndexAnalysis_ = analysisIndex;
  return true;
}

bool DictionaryActivity::loadContextualDefinitionPage(const DefinitionCursor& start, const uint32_t pageIndex,
                                                      const dictionary::definition::WidthMeasurer& measurer,
                                                      const size_t maxLines) {
  const uint8_t analysisCount = shortlist_->items[selected_].analysisCount;
  DefinitionCursor cursor = start;
  definitionPageNext_ = start;
  uint8_t appendedAnalysis = UINT8_MAX;
  size_t dividerSlots = 0;
  bool firstEntry = true;
  *definitionPage_ = {};

  while (cursor.analysisIndex < analysisCount && definitionPage_->lineCount < maxLines) {
    if (!loadContextualIndexes(cursor.analysisIndex)) {
      definitionFailed_ = true;
      requestUpdate();
      return false;
    }
    if (cursor.sourceIndex >= contextualIndexCount_) {
      ++cursor.analysisIndex;
      cursor.sourceIndex = 0;
      cursor.entry = {};
      definitionPageNext_ = cursor;
      continue;
    }

    const uint8_t sourceIndex = cursor.sourceIndex;
    const auto& index = contextualIndexes_[sourceIndex];
    if (index.status != dictionary::lookup::DefinitionIndexStatus::PRESENT) {
      ++cursor.sourceIndex;
      cursor.entry = {};
      definitionPageNext_ = cursor;
      continue;
    }

    dictionary::definition::EntryReader reader;
    dictionary::lookup::SessionError sessionError = dictionary::lookup::SessionError::NONE;
    if (!session_->contextualEntryReader(sourceIndex, reader, sessionError)) {
      LOG_ERR("DICT", "Contextual entry source failed: %s", dictionary::lookup::sessionErrorName(sessionError));
      ++cursor.sourceIndex;
      cursor.entry = {};
      definitionPageNext_ = cursor;
      continue;
    }
    const dictionary::EntrySlice slice{index.record.entryOffset, index.record.entryLength};
    const uint8_t oldLineCount = definitionPage_->lineCount;
    const uint16_t oldTextBytes = definitionPage_->textBytesUsed;
    const bool startsSource = entryCursorAtStart(cursor.entry);
    const bool startsAnalysis = !firstEntry && appendedAnalysis != cursor.analysisIndex;
    const size_t pendingDividers = (startsSource ? 1U : 0U) + (startsAnalysis ? 1U : 0U);
    const size_t contentLineLimit =
        dictionary::definition::contentLineLimit(maxLines, definitionPage_->lineCount, dividerSlots, pendingDividers);
    if (contentLineLimit == 0) {
      definitionPageNext_ = cursor;
      break;
    }
    dictionary::definition::PagerError pagerError = dictionary::definition::PagerError::NONE;
    const bool loaded = firstEntry ? pager_->load(reader, slice, cursor.entry, measurer, definitionContentWidth(),
                                                  contentLineLimit, *definitionPage_, pagerError)
                                   : pager_->append(reader, slice, cursor.entry, measurer, definitionContentWidth(),
                                                    contentLineLimit, *definitionPage_, pagerError);
    if (!loaded) {
      definitionPage_->lineCount = oldLineCount;
      definitionPage_->textBytesUsed = oldTextBytes;
      definitionPage_->hasNext = false;
      LOG_ERR("DICT", "Skipping contextual definition source %u: %s", static_cast<unsigned>(sourceIndex),
              dictionary::definition::pagerErrorName(pagerError));
      contextualSourceWarning_ = true;
      ++cursor.sourceIndex;
      cursor.entry = {};
      definitionPageNext_ = cursor;
      continue;
    }

    // Pager mutates the referenced page; cppcheck does not model that callback path.
    // cppcheck-suppress knownConditionTrueFalse
    if (definitionPage_->lineCount > oldLineCount) {
      auto& firstLine = definitionPage_->lines[oldLineCount];
      firstLine.analysisStart = startsAnalysis;
      firstLine.sourceStart = startsSource;
      firstLine.sourceIndex = sourceIndex;
      dividerSlots += pendingDividers;
      appendedAnalysis = cursor.analysisIndex;
      firstEntry = false;
    }
    if (definitionPage_->hasNext) {
      definitionPageNext_ = {definitionPage_->next, cursor.analysisIndex, sourceIndex};
      break;
    }
    ++cursor.sourceIndex;
    cursor.entry = {};
    definitionPageNext_ = cursor;
  }

  definitionPage_->hasNext = definitionPageNext_.analysisIndex < analysisCount;
  definitionFailed_ = definitionPage_->lineCount == 0;
  definitionFailure_ = definitionFailed_ ? (contextualSourceWarning_ ? DefinitionFailure::SourceCorrupt
                                                                     : DefinitionFailure::NoDefinition)
                                         : DefinitionFailure::None;
  definitionPageStart_ = start;
  definitionPageIndex_ = pageIndex;
  requestUpdate();
  return !definitionFailed_;
}

bool DictionaryActivity::loadDefinitionPage(const DefinitionCursor& start, const uint32_t pageIndex) {
  if (!pager_ || !definitionPage_ || !shortlist_ || selected_ >= shortlist_->count) return false;
  const dictionary::definition::WidthMeasurer measurer{this, measureDefinitionText};
  const int lineStep = renderer.getLineHeight(UI_10_FONT_ID) + kDefinitionLineGap;
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  contentMargins(top, right, bottom, left);
  (void)right;
  (void)left;
  const int availableHeight = std::max(1, renderer.getScreenHeight() - bottom - (top + kListTop) - kBottomReserved);
  const int visualRowHeight =
      std::max({lineStep + kDefinitionMeaningGap, kSourceDividerHeight, kAnalysisDividerHeight});
  const size_t visibleLines = static_cast<size_t>(std::max(1, availableHeight / std::max(1, visualRowHeight)));
  const size_t maxLines = std::min(visibleLines, dictionary::definition::kMaxPageLines);
  return loadContextualDefinitionPage(start, pageIndex, measurer, maxLines);
}

void DictionaryActivity::changeDefinitionPage(const int delta) {
  if (definitionFailed_ || !definitionPage_) return;
  if (delta > 0 && definitionPage_->hasNext) {
    loadDefinitionPage(definitionPageNext_, definitionPageIndex_ + 1);
    return;
  }
  if (delta >= 0 || definitionPageIndex_ == 0) return;

  // Backward navigation replays bounded pages from the entry start. This keeps
  // memory independent of definition length; only explicit reverse navigation
  // pays the additional sequential SD reads.
  const uint32_t targetPage = definitionPageIndex_ - 1;
  DefinitionCursor cursor{};
  for (uint32_t page = 0; page <= targetPage; ++page) {
    if (!loadDefinitionPage(cursor, page)) return;
    if (page < targetPage) {
      if (!definitionPage_->hasNext) return;
      cursor = definitionPageNext_;
    }
  }
}

void DictionaryActivity::saveSelectedStatus() {
  dictionary::lookup::SessionError error = dictionary::lookup::SessionError::NONE;
  const auto& item = shortlist_->items[selected_];
  if (!session_->setItemStatus(item, statusValue(statusSelection_), error)) {
    LOG_ERR("DICT", "Status update failed: %s", dictionary::lookup::sessionErrorName(error));
    statusSaved_ = false;
    mode_ = Mode::Definition;
    definitionFailed_ = true;
    requestUpdate();
    return;
  }
  LOG_INF("DICT", "Status saved: value=%u generation=%lu free=%u maxAlloc=%u stackHwm=%lu",
          static_cast<unsigned>(statusSelection_), static_cast<unsigned long>(session_->stateGeneration()),
          ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          static_cast<unsigned long>(dictionary::io_metrics::currentTaskStackHighWaterBytes()));
  statusSaved_ = true;
  mode_ = Mode::Definition;
  requestUpdate();
}

void DictionaryActivity::returnToShortlist() {
  dictionary::lookup::SessionError error = dictionary::lookup::SessionError::NONE;
  if (!session_->filterShortlist(*shortlist_, error)) {
    LOG_ERR("DICT", "Shortlist status filter failed: %s", dictionary::lookup::sessionErrorName(error));
  }
  if (shortlist_->count == 0) {
    finish();
    return;
  }
  if (selected_ >= shortlist_->count) selected_ = static_cast<uint16_t>(shortlist_->count - 1);
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
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selected_ = selected_ == 0 ? static_cast<uint16_t>(shortlist_->count - 1) : selected_ - 1;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selected_ = static_cast<uint16_t>((selected_ + 1) % shortlist_->count);
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      const uint16_t rows = static_cast<uint16_t>(shortlistRowsPerPage());
      selected_ = selected_ > rows ? static_cast<uint16_t>(selected_ - rows) : 0;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      const uint16_t rows = static_cast<uint16_t>(shortlistRowsPerPage());
      selected_ = static_cast<uint16_t>(std::min<size_t>(shortlist_->count - 1, selected_ + rows));
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
    changeDefinitionPage(-1);
  } else if (next) {
    changeDefinitionPage(1);
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
  char header[200]{};
  if (canonicalHeadword_[0] != '\0' && std::strcmp(headword_, canonicalHeadword_) != 0) {
    std::snprintf(header, sizeof(header), "%s · %s", headword_, canonicalHeadword_);
  } else {
    std::snprintf(header, sizeof(header), "%s", headword_);
  }
  renderer.drawText(UI_12_FONT_ID, left + kSideMargin, top + kHeaderY, header, true, EpdFontFamily::BOLD);

  if (definitionFailed_) {
    renderer.drawText(UI_10_FONT_ID, left + kSideMargin, top + kListTop, definitionFailureMessage());
  } else if (definitionPage_) {
    int y = top + kListTop;
    const int lineStep = renderer.getLineHeight(UI_10_FONT_ID) + kDefinitionLineGap;
    for (uint8_t index = 0; index < definitionPage_->lineCount; ++index) {
      const auto& line = definitionPage_->lines[index];
      if (line.analysisStart) {
        y += kAnalysisDividerHeight / 2;
        const int centerX = left + (renderer.getScreenWidth() - left - right) / 2;
        renderer.fillRect(centerX - kAnalysisDividerWidth / 2, y, kAnalysisDividerWidth, 1, true);
        y += kAnalysisDividerHeight / 2;
      } else if (line.gapBefore && !line.sourceStart) {
        y += kDefinitionMeaningGap;
      }
      if (line.sourceStart) {
        const auto* source = session_->definitionSource(line.sourceIndex);
        if (source) {
          const int contentLeft = left + kSideMargin;
          const int contentRight = renderer.getScreenWidth() - right - kSideMargin;
          const int centerX = (contentLeft + contentRight) / 2;
          const int labelWidth = renderer.getTextAdvanceX(SMALL_FONT_ID, source->sourceLabel, EpdFontFamily::BOLD);
          const int labelLeft = centerX - labelWidth / 2;
          const int labelRight = labelLeft + labelWidth;
          const int lineY = y + renderer.getLineHeight(SMALL_FONT_ID) / 2;
          if (labelLeft - kSourceDividerGap > contentLeft) {
            renderer.fillRect(contentLeft, lineY, labelLeft - kSourceDividerGap - contentLeft, 1, true);
          }
          if (contentRight > labelRight + kSourceDividerGap) {
            renderer.fillRect(labelRight + kSourceDividerGap, lineY, contentRight - labelRight - kSourceDividerGap, 1,
                              true);
          }
          renderer.drawText(SMALL_FONT_ID, labelLeft, y, source->sourceLabel, true, EpdFontFamily::BOLD);
          y += kSourceDividerHeight;
        }
      }
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
    if (definitionPageIndex_ > 0 || definitionPage_->hasNext) {
      char pageLabel[16]{};
      std::snprintf(pageLabel, sizeof(pageLabel), "%u%s", definitionPageIndex_ + 1,
                    definitionPage_->hasNext ? "+" : "");
      renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - right - kSideMargin - 25,
                        renderer.getScreenHeight() - bottom - kBottomReserved, pageLabel);
    }
  }

  if (statusSaved_) {
    renderer.drawText(SMALL_FONT_ID, left + kSideMargin, renderer.getScreenHeight() - bottom - kBottomReserved,
                      tr(STR_WORD_STATUS_SAVED));
  } else if (contextualSourceWarning_ && !definitionFailed_) {
    renderer.drawText(SMALL_FONT_ID, left + kSideMargin, renderer.getScreenHeight() - bottom - kBottomReserved,
                      tr(STR_SOME_DEFINITION_SOURCES_UNAVAILABLE));
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
  const unsigned long renderStartedAt = millis();
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
  const unsigned long displayStartedAt = millis();
  renderer.displayBuffer();
  LOG_INF("DICT", "Display %s: draw=%lu ms refresh=%lu ms lookupTotal=%lu ms free=%u maxAlloc=%u",
          modeName(static_cast<uint8_t>(mode_)), displayStartedAt - renderStartedAt, millis() - displayStartedAt,
          lookupStartedAt_ == 0 ? 0 : millis() - lookupStartedAt_, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

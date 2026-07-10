#pragma once

#include <DefinitionPager.h>
#include <PageShortlist.h>

#include <memory>

#include "activities/Activity.h"
#include "dictionary/DictionaryLookupSession.h"

class DictionaryActivity final : public Activity {
 public:
  DictionaryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                     std::unique_ptr<dictionary::lookup::Session> session,
                     std::unique_ptr<dictionary::page_shortlist::Shortlist> shortlist, unsigned long lookupStartedAt);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  struct DefinitionCursor {
    dictionary::definition::Cursor entry{};
    uint8_t analysisIndex = 0;
  };

  enum class Mode : uint8_t { Shortlist, Definition, Status };
  std::unique_ptr<dictionary::lookup::Session> session_;
  std::unique_ptr<dictionary::page_shortlist::Shortlist> shortlist_;
  std::unique_ptr<dictionary::definition::Pager> pager_;
  std::unique_ptr<dictionary::definition::Page> definitionPage_;
  DefinitionCursor definitionPageStart_{};
  DefinitionCursor definitionPageNext_{};
  dictionary::LexemeRecord lexeme_{};
  dictionary::EntrySlice entry_{};
  Mode mode_ = Mode::Shortlist;
  uint16_t selected_ = 0;
  uint32_t definitionPageIndex_ = 0;
  uint8_t statusSelection_ = 0;
  char headword_[dictionary::kMaxHeadwordBytes + 1]{};
  char lineScratch_[dictionary::definition::kMaxLineBytes + 1]{};
  unsigned long lookupStartedAt_ = 0;
  bool definitionFailed_ = false;
  bool statusSaved_ = false;

  bool openDefinition();
  bool loadDefinitionPage(const DefinitionCursor& start, uint32_t pageIndex);
  bool openAnalysisEntry(uint8_t analysisIndex);
  void changeDefinitionPage(int delta);
  void saveSelectedStatus();
  void returnToShortlist();
  int shortlistRowsPerPage() const;
  int definitionContentWidth() const;
  void contentMargins(int& top, int& right, int& bottom, int& left) const;
  static int measureDefinitionText(void* context, std::string_view text);

  void renderShortlist();
  void renderDefinition();
  void renderStatus();
};

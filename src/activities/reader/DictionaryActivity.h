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
  enum class Mode : uint8_t { Shortlist, Definition, Status };
  std::unique_ptr<dictionary::lookup::Session> session_;
  std::unique_ptr<dictionary::page_shortlist::Shortlist> shortlist_;
  std::unique_ptr<dictionary::definition::Pager> pager_;
  std::unique_ptr<dictionary::definition::Page> definitionPage_;
  dictionary::definition::Cursor definitionPageStart_{};
  dictionary::LexemeRecord lexeme_{};
  dictionary::EntrySlice entry_{};
  Mode mode_ = Mode::Shortlist;
  uint16_t selected_ = 0;
  uint8_t analysisIndex_ = 0;
  uint32_t definitionPageIndex_ = 0;
  uint8_t statusSelection_ = 0;
  char headword_[dictionary::kMaxHeadwordBytes + 1]{};
  char lineScratch_[dictionary::definition::kMaxLineBytes + 1]{};
  unsigned long lookupStartedAt_ = 0;
  bool definitionFailed_ = false;
  bool statusSaved_ = false;

  bool openDefinition();
  bool loadDefinitionPage(const dictionary::definition::Cursor& start, uint32_t pageIndex);
  void changeDefinitionPage(int delta);
  void changeAnalysis(int delta);
  void saveSelectedStatus();
  void returnToShortlist();
  uint16_t selectedLocalLemmaId() const;
  int shortlistRowsPerPage() const;
  int definitionContentWidth() const;
  void contentMargins(int& top, int& right, int& bottom, int& left) const;
  static int measureDefinitionText(void* context, std::string_view text);

  void renderShortlist();
  void renderDefinition();
  void renderStatus();
};

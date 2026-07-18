#pragma once

#include <DefinitionPager.h>
#include <PageShortlist.h>

#include <array>
#include <cstdint>
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
    uint8_t sourceIndex = 0;
  };

  enum class AnalysisLabelState : uint8_t { Empty = 0, Ready, Failed };
  struct AnalysisLabelCacheEntry {
    dictionary::contextual::CanonicalAnalysisLabel label{};
    AnalysisLabelState state = AnalysisLabelState::Empty;
  };
  static_assert(sizeof(AnalysisLabelCacheEntry) * dictionary::page_shortlist::kMaxAnalysesPerItem <= 512,
                "Analysis-label cache exceeds its 512-byte activity budget");

  enum class Mode : uint8_t { Shortlist, Definition, Status };
  enum class DefinitionFailure : uint8_t {
    None = 0,
    General,
    AttachmentsInvalid,
    NoCompatibleSources,
    NoDefinition,
    SourceCorrupt,
  };
  std::unique_ptr<dictionary::lookup::Session> session_;
  std::unique_ptr<dictionary::page_shortlist::Shortlist> shortlist_;
  std::unique_ptr<dictionary::definition::Pager> pager_;
  std::unique_ptr<dictionary::definition::Page> definitionPage_;
  DefinitionCursor definitionPageStart_{};
  DefinitionCursor definitionPageNext_{};
  std::array<dictionary::lookup::DefinitionIndexLookup, dictionary::contextual::kMaxAttachedSources>
      contextualIndexes_{};
  uint8_t contextualIndexCount_ = 0;
  uint8_t contextualIndexAnalysis_ = UINT8_MAX;
  Mode mode_ = Mode::Shortlist;
  uint16_t selected_ = 0;
  uint32_t definitionPageIndex_ = 0;
  uint8_t statusSelection_ = 0;
  char headword_[dictionary::contextual::kMaxCanonicalHeadwordBytes + 1]{};
  // The activity is heap-owned. Eight fixed cache slots retain labels only for
  // the selected word; loading uses the session's single switching SD reader.
  std::array<AnalysisLabelCacheEntry, dictionary::page_shortlist::kMaxAnalysesPerItem> analysisLabels_{};
  char lineScratch_[dictionary::definition::kMaxLineBytes + 1]{};
  unsigned long lookupStartedAt_ = 0;
  bool definitionFailed_ = false;
  bool contextualSourceWarning_ = false;
  bool statusSaved_ = false;
  DefinitionFailure definitionFailure_ = DefinitionFailure::None;

  bool openDefinition();
  bool loadDefinitionPage(const DefinitionCursor& start, uint32_t pageIndex);
  bool loadContextualDefinitionPage(const DefinitionCursor& start, uint32_t pageIndex,
                                    const dictionary::definition::WidthMeasurer& measurer, size_t maxLines);
  bool loadContextualIndexes(uint8_t analysisIndex);
  bool loadAnalysisLabel(uint8_t analysisIndex);
  void resetAnalysisLabels();
  void changeDefinitionPage(int delta);
  void saveSelectedStatus();
  void returnToShortlist();
  int shortlistRowsPerPage() const;
  int definitionContentWidth() const;
  void contentMargins(int& top, int& right, int& bottom, int& left) const;
  static int measureDefinitionText(void* context, std::string_view text);
  const char* definitionFailureMessage() const;

  void renderShortlist();
  void renderDefinition();
  void renderStatus();
};

#include "WordInboxFeedback.h"

#include <I18n.h>

#include "activities/reader/ReaderUtils.h"
#include "fontIds.h"

namespace WordInboxFeedback {

void show(const GfxRenderer& renderer, const WordInboxSaveResult result) {
  constexpr int paddingX = 20;
  constexpr int paddingY = 12;
  const char* message = result == WordInboxSaveResult::Saved ? tr(STR_WORD_INBOX_SAVED) : tr(STR_WORD_INBOX_FAILED);
  const bool backgroundBlack = ReaderUtils::readerForegroundBlack();
  const int messageWidth = renderer.getTextWidth(UI_10_FONT_ID, message);
  const int messageHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int width = messageWidth + paddingX * 2;
  const int height = messageHeight + paddingY * 2;
  const int x = (renderer.getScreenWidth() - width) / 2;
  const int y = (renderer.getScreenHeight() - height) / 2;

  renderer.fillRect(x, y, width, height, backgroundBlack);
  renderer.drawRect(x, y, width, height, !backgroundBlack);
  renderer.drawText(UI_10_FONT_ID, x + paddingX, y + paddingY, message, !backgroundBlack);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

}  // namespace WordInboxFeedback

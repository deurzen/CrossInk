#include "WordInboxFeedback.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "activities/reader/ReaderUtils.h"
#include "fontIds.h"

namespace WordInboxFeedback {
namespace {

constexpr uint32_t SAVED_TOAST_DURATION_MS = 1200UL;
constexpr uint32_t FAILED_TOAST_DURATION_MS = 2000UL;
constexpr int PADDING_X = 14;
constexpr int PADDING_Y = 8;
constexpr int BOTTOM_GAP = 12;

Controller::Rect computeToastRect(const GfxRenderer& renderer, const char* message) {
  const int width = renderer.getTextWidth(UI_10_FONT_ID, message) + PADDING_X * 2;
  const int height = renderer.getLineHeight(UI_10_FONT_ID) + PADDING_Y * 2;
  int viewableTop = 0;
  int viewableRight = 0;
  int viewableBottom = 0;
  int viewableLeft = 0;
  renderer.getOrientedViewableTRBL(&viewableTop, &viewableRight, &viewableBottom, &viewableLeft);

  const int availableLeft = viewableLeft;
  const int availableRight = renderer.getScreenWidth() - viewableRight;
  const int x = std::max(availableLeft, availableLeft + (availableRight - availableLeft - width) / 2);
  const int y = std::max(viewableTop, renderer.getScreenHeight() - viewableBottom - height - BOTTOM_GAP);
  return {x, y, std::min(width, availableRight - x), std::min(height, renderer.getScreenHeight() - viewableBottom - y)};
}

size_t conservativeRegionCapacity(const Controller::Rect& rect) {
  if (rect.w <= 0 || rect.h <= 0) {
    return 0;
  }
  // Region copies include whole bytes at both horizontal edges. Budget both
  // physical orientations so rotating later does not cause another allocation.
  const size_t portrait = static_cast<size_t>((rect.w + 14) / 8) * static_cast<size_t>(rect.h);
  const size_t landscape = static_cast<size_t>((rect.h + 14) / 8) * static_cast<size_t>(rect.w);
  return std::max(portrait, landscape);
}

}  // namespace

void Controller::show(const GfxRenderer& renderer, const WordInboxSaveResult result) {
  // A repeated capture first returns the framebuffer to the clean page. The
  // panel may still show the old toast, but the next differential refresh will
  // replace it directly with the new one and restart the timeout.
  bool previousToastRestored = false;
  if (active.load(std::memory_order_acquire)) {
    previousToastRestored = restoreFramebuffer(renderer);
    if (!previousToastRestored) {
      clearActive();
      LOG_ERR("WIN", "Could not restore previous Word Inbox toast");
      return;
    }
  }
  const auto abandonToast = [&]() {
    clearActive();
    if (previousToastRestored) {
      // Do not leave the prior toast physically visible if preparing its
      // replacement failed after its framebuffer region was restored.
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
  };

  const char* message = result == WordInboxSaveResult::Saved ? tr(STR_WORD_INBOX_SAVED) : tr(STR_WORD_INBOX_FAILED);
  const Rect toast = computeToastRect(renderer, message);
  const char* otherMessage =
      result == WordInboxSaveResult::Saved ? tr(STR_WORD_INBOX_FAILED) : tr(STR_WORD_INBOX_SAVED);
  const Rect otherToast = computeToastRect(renderer, otherMessage);
  const size_t requiredCapacity = std::max(conservativeRegionCapacity(toast), conservativeRegionCapacity(otherToast));
  const size_t requiredSize = renderer.getRegionByteSize(toast.x, toast.y, toast.w, toast.h);
  if (requiredCapacity == 0 || requiredSize == 0) {
    LOG_ERR("WIN", "Invalid Word Inbox toast region");
    abandonToast();
    return;
  }

  if (!regionBuffer || regionBufferCapacity < requiredCapacity) {
    // Toast regions are too large for the reader task stack. Retain one bounded
    // allocation for this activity and reuse it for every subsequent capture.
    auto replacement = makeUniqueNoThrow<uint8_t[]>(requiredCapacity);
    if (!replacement) {
      LOG_ERR("WIN", "OOM: Word Inbox toast backup (%u bytes)", static_cast<unsigned>(requiredCapacity));
      abandonToast();
      return;
    }
    regionBuffer = std::move(replacement);
    regionBufferCapacity = requiredCapacity;
  }

  if (requiredSize > regionBufferCapacity ||
      !renderer.copyRegionToBuffer(toast.x, toast.y, toast.w, toast.h, regionBuffer.get(), regionBufferCapacity)) {
    LOG_ERR("WIN", "Could not back up Word Inbox toast region");
    abandonToast();
    return;
  }

  savedRegion = toast;
  savedRegionSize = requiredSize;
  savedOrientation = renderer.getOrientation();

  const bool foregroundBlack = ReaderUtils::readerForegroundBlack();
  const bool backgroundBlack = !foregroundBlack;
  renderer.fillRect(toast.x, toast.y, toast.w, toast.h, backgroundBlack);
  renderer.drawRect(toast.x, toast.y, toast.w, toast.h, foregroundBlack);
  renderer.drawText(UI_10_FONT_ID, toast.x + PADDING_X, toast.y + PADDING_Y, message, foregroundBlack);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  shownAtMs = millis();
  durationMs = result == WordInboxSaveResult::Saved ? SAVED_TOAST_DURATION_MS : FAILED_TOAST_DURATION_MS;
  active.store(true, std::memory_order_release);
}

bool Controller::prepareForCapture(const GfxRenderer& renderer) {
  if (!active.load(std::memory_order_acquire)) {
    return true;
  }
  const bool restored = restoreFramebuffer(renderer);
  clearActive();
  if (!restored) {
    LOG_ERR("WIN", "Could not remove Word Inbox toast before capture");
  }
  return restored;
}

void Controller::prepareForRender(const GfxRenderer& renderer) {
  if (!active.load(std::memory_order_acquire)) {
    return;
  }
  // Restoration keeps partial render paths safe. If orientation changed, the
  // upcoming page render replaces the framebuffer and stale coordinates must
  // not be applied.
  if (renderer.getOrientation() == savedOrientation) {
    restoreFramebuffer(renderer);
  }
  clearActive();
}

bool Controller::dismissalDue() const {
  if (!active.load(std::memory_order_acquire)) {
    return false;
  }
  return static_cast<uint32_t>(millis() - shownAtMs) >= durationMs;
}

Controller::DismissResult Controller::dismissIfDue(const GfxRenderer& renderer) {
  if (!dismissalDue()) {
    return DismissResult::NotDue;
  }
  const bool restored = restoreFramebuffer(renderer);
  clearActive();
  if (!restored) {
    LOG_ERR("WIN", "Could not dismiss Word Inbox toast");
    return DismissResult::NeedsRender;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  return DismissResult::Restored;
}

bool Controller::restoreFramebuffer(const GfxRenderer& renderer) {
  return active.load(std::memory_order_acquire) && regionBuffer && savedRegionSize > 0 &&
         renderer.getOrientation() == savedOrientation &&
         renderer.copyBufferToRegion(savedRegion.x, savedRegion.y, savedRegion.w, savedRegion.h, regionBuffer.get(),
                                     savedRegionSize);
}

void Controller::clearActive() {
  active.store(false, std::memory_order_release);
  savedRegionSize = 0;
}

}  // namespace WordInboxFeedback

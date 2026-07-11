#pragma once

#include <GfxRenderer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "WordInboxStore.h"

namespace WordInboxFeedback {

// Owns the temporary toast overlay for one reader activity. The covered
// framebuffer bytes are retained and reused so dismissing the toast does not
// require re-rendering the page or allocating on every capture.
class Controller {
 public:
  enum class DismissResult : uint8_t {
    NotDue,
    Restored,
    NeedsRender,
  };

  struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
  };

  // Draws reader-aware, translated feedback and performs one fast refresh.
  // The caller must hold its activity RenderLock.
  void show(const GfxRenderer& renderer, WordInboxSaveResult result);

  // Restores a pending toast in the framebuffer without refreshing the panel.
  // Call before reading the framebuffer for another capture.
  bool prepareForCapture(const GfxRenderer& renderer);

  // Restores a pending toast before a page render. A full page render may safely
  // continue if restoration fails because it replaces the framebuffer.
  void prepareForRender(const GfxRenderer& renderer);

  // Safe to query from the activity loop while the render task is running.
  bool dismissalDue() const;

  // Automatically restores and fast-refreshes an expired toast. The caller
  // must hold its activity RenderLock.
  DismissResult dismissIfDue(const GfxRenderer& renderer);

 private:
  bool restoreFramebuffer(const GfxRenderer& renderer);
  void clearActive();

  std::unique_ptr<uint8_t[]> regionBuffer;
  size_t regionBufferCapacity = 0;
  size_t savedRegionSize = 0;
  Rect savedRegion;
  GfxRenderer::Orientation savedOrientation = GfxRenderer::Orientation::Portrait;
  uint32_t shownAtMs = 0;
  uint32_t durationMs = 0;
  std::atomic<bool> active{false};
};

}  // namespace WordInboxFeedback

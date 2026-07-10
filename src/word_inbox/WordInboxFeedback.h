#pragma once

#include <GfxRenderer.h>

#include "WordInboxStore.h"

namespace WordInboxFeedback {

// Draws reader-aware, translated feedback over the current framebuffer and
// performs one fast refresh. The caller must hold its activity RenderLock.
void show(const GfxRenderer& renderer, WordInboxSaveResult result);

}  // namespace WordInboxFeedback

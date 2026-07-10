#pragma once

#include <PageShortlist.h>

class Page;

namespace dictionary::current_page_shortlist {

constexpr size_t kMaxRenderedWords = 256;

struct CollectResult {
  size_t renderedWordsVisited = 0;
  size_t visibleTokens = 0;
  bool truncated = false;
};

// Explicit-lookup-only adapter from the shared rendered-page visitor into the
// allocation-free dictionary shortlist workspace.
CollectResult collectVisibleTokens(const Page& page, page_shortlist::Generator& generator);

}  // namespace dictionary::current_page_shortlist

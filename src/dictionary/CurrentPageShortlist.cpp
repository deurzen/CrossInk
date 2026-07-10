#include "CurrentPageShortlist.h"

#include "util/VisiblePageWords.h"

namespace dictionary::current_page_shortlist {
namespace {

bool addWord(void* const context, const VisiblePageWords::Word& word) {
  auto& generator = *static_cast<page_shortlist::Generator*>(context);
  return generator.addRenderedWord(word.text, word.insertedTrailingHyphen);
}

}  // namespace

CollectResult collectVisibleTokens(const Page& page, page_shortlist::Generator& generator) {
  generator.reset();
  const VisiblePageWords::Callbacks callbacks{&generator, addWord, nullptr};
  const auto visit = VisiblePageWords::visitEpubPage(page, kMaxRenderedWords, callbacks);
  generator.finishRenderedPage();
  return {visit.wordsVisited, generator.visibleTokenCount(),
          visit.limitReached || !visit.completed || generator.visibleTokensTruncated()};
}

}  // namespace dictionary::current_page_shortlist

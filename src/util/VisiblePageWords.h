#pragma once

#include <cstddef>
#include <string_view>

class Page;

namespace VisiblePageWords {

struct Word {
  std::string_view text;
  bool insertedTrailingHyphen = false;
};

struct Callbacks {
  void* context = nullptr;
  bool (*onWord)(void* context, const Word& word) = nullptr;
  bool (*onLineEnd)(void* context, bool joinsNextLine) = nullptr;
};

struct Result {
  size_t wordsVisited = 0;
  bool completed = false;
  bool limitReached = false;
};

// Visits rendered EPUB words in visual storage order without allocating. The
// caller supplies a firm word cap and may stop early from either callback.
Result visitEpubPage(const Page& page, size_t maxWords, const Callbacks& callbacks);

}  // namespace VisiblePageWords

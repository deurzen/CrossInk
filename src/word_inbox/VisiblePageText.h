#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "VisibleTextBuffer.h"

class Page;

namespace VisiblePageText {

inline constexpr size_t MAX_TEXT_BYTES = 8192;

// Reconstructs copyable text from the same laid-out elements used to render the
// current EPUB page. The caller owns the bounded output buffer.
VisibleTextBuffer::Result fromEpubPage(const Page& page, char* buffer, size_t capacity);

// TXT/Markdown readers already retain the exact rendered lines.
VisibleTextBuffer::Result fromTxtLines(const std::vector<std::string>& lines, char* buffer, size_t capacity);

}  // namespace VisiblePageText

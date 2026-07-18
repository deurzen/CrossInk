#include "DefinitionPager.h"

#include <algorithm>
#include <cstring>

namespace dictionary::definition {
namespace {

bool isContinuation(const uint8_t byte) { return (byte & 0xC0U) == 0x80U; }

uint8_t utf8Length(const uint8_t first) {
  if (first < 0x80U) return 1;
  if (first >= 0xC2U && first <= 0xDFU) return 2;
  if (first >= 0xE0U && first <= 0xEFU) return 3;
  if (first >= 0xF0U && first <= 0xF4U) return 4;
  return 0;
}

bool isAsciiSpace(const uint8_t byte) { return byte == ' ' || byte == '\t' || byte == '\r'; }

uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8U);
}

bool readExact(const EntryReader& reader, const EntrySlice& entry, const uint32_t relativeOffset, void* output,
               const size_t length) {
  RuntimeError runtimeError = RuntimeError::NONE;
  size_t bytesRead = 0;
  return reader.readChunk != nullptr &&
         reader.readChunk(reader.context, entry, relativeOffset, output, length, bytesRead, runtimeError) &&
         bytesRead == length;
}

}  // namespace

std::string_view Page::lineText(const uint8_t index) const {
  if (index >= lineCount) return {};
  const Line& line = lines[index];
  if (static_cast<size_t>(line.textOffset) + line.textLength > textBytesUsed) return {};
  return {text + line.textOffset, line.textLength};
}

bool Pager::readByte(const EntryReader& reader, const EntrySlice& entry, const uint32_t relativeOffset, uint8_t& value,
                     PagerError& error) {
  if (chunkStart_ == UINT32_MAX || relativeOffset < chunkStart_ || relativeOffset >= chunkStart_ + chunkLength_) {
    chunkStart_ = relativeOffset;
    RuntimeError runtimeError = RuntimeError::NONE;
    if (!reader.readChunk(reader.context, entry, relativeOffset, chunk_, sizeof(chunk_), chunkLength_, runtimeError) ||
        chunkLength_ == 0) {
      error = PagerError::ENTRY_READ_FAILED;
      return false;
    }
  }
  value = chunk_[relativeOffset - chunkStart_];
  return true;
}

bool Pager::load(const EntryReader& reader, const EntrySlice& entry, const Cursor& start, const WidthMeasurer& measurer,
                 const int maxLineWidth, const size_t maxLines, Page& output, PagerError& error) {
  return loadInternal(reader, entry, start, measurer, maxLineWidth, maxLines, true, output, error);
}

bool Pager::append(const EntryReader& reader, const EntrySlice& entry, const Cursor& start,
                   const WidthMeasurer& measurer, const int maxLineWidth, const size_t maxLines, Page& output,
                   PagerError& error) {
  return loadInternal(reader, entry, start, measurer, maxLineWidth, maxLines, false, output, error);
}

bool Pager::readEntryHeader(const EntryReader& reader, const EntrySlice& entry, EntryHeader& output,
                            PagerError& error) const {
  output = {};
  if (entry.length < 4 || entry.length > kMaxEntryBytes) {
    error = PagerError::CURSOR_INVALID;
    return false;
  }
  uint8_t data[4]{};
  if (!readExact(reader, entry, 0, data, sizeof(data))) {
    error = PagerError::ENTRY_READ_FAILED;
    return false;
  }
  output.version = data[0];
  output.flags = data[1];
  output.fieldCount = readU16(data + 2);
  if (output.version != 1 || output.flags != 0 || output.fieldCount == 0 || output.fieldCount > kMaxEntryFieldCount) {
    output = {};
    error = PagerError::ENTRY_READ_FAILED;
    return false;
  }
  return true;
}

bool Pager::readFieldHeader(const EntryReader& reader, const EntrySlice& entry, const uint32_t relativeOffset,
                            EntryFieldHeader& output, PagerError& error) const {
  output = {};
  if (relativeOffset < 4 || static_cast<uint64_t>(relativeOffset) + 4 > entry.length) {
    error = PagerError::CURSOR_INVALID;
    return false;
  }
  uint8_t data[4]{};
  if (!readExact(reader, entry, relativeOffset, data, sizeof(data))) {
    error = PagerError::ENTRY_READ_FAILED;
    return false;
  }
  output.type = data[0];
  output.flags = data[1];
  output.length = readU16(data + 2);
  output.payloadOffset = relativeOffset + 4;
  if (output.type == 0 || output.flags != 0 ||
      static_cast<uint64_t>(output.payloadOffset) + output.length > entry.length) {
    output = {};
    error = PagerError::ENTRY_READ_FAILED;
    return false;
  }
  return true;
}

bool Pager::loadInternal(const EntryReader& reader, const EntrySlice& entry, const Cursor& start,
                         const WidthMeasurer& measurer, const int maxLineWidth, const size_t maxLines,
                         const bool resetOutput, Page& output, PagerError& error) {
  if (resetOutput) output = {};
  output.hasNext = false;
  error = PagerError::NONE;
  chunkStart_ = UINT32_MAX;
  chunkLength_ = 0;

  if (reader.readChunk == nullptr || measurer.measure == nullptr || maxLineWidth <= 0 || maxLines == 0 ||
      maxLines > kMaxPageLines) {
    error = PagerError::INVALID_INPUT;
    return false;
  }

  EntryHeader entryHeader;
  if (!readEntryHeader(reader, entry, entryHeader, error)) return false;
  if (start.fieldIndex > entryHeader.fieldCount || start.fieldHeaderOffset < 4 ||
      start.fieldHeaderOffset > entry.length ||
      (start.fieldIndex == entryHeader.fieldCount &&
       (start.fieldHeaderOffset != entry.length || start.fieldByteOffset != 0))) {
    error = PagerError::CURSOR_INVALID;
    return false;
  }

  Cursor cursor = start;
  while (cursor.fieldIndex < entryHeader.fieldCount && output.lineCount < maxLines) {
    EntryFieldHeader field;
    if (!readFieldHeader(reader, entry, cursor.fieldHeaderOffset, field, error)) return false;
    if (cursor.fieldByteOffset > field.length) {
      error = PagerError::CURSOR_INVALID;
      return false;
    }

    bool gapBeforeNextLine = false;
    while (cursor.fieldByteOffset < field.length && output.lineCount < maxLines) {
      while (cursor.fieldByteOffset < field.length) {
        uint8_t byte = 0;
        if (!readByte(reader, entry, field.payloadOffset + cursor.fieldByteOffset, byte, error)) return false;
        if (!isAsciiSpace(byte)) break;
        ++cursor.fieldByteOffset;
      }
      if (cursor.fieldByteOffset >= field.length) break;

      const uint16_t lineStartInField = cursor.fieldByteOffset;
      size_t lineLength = 0;
      size_t lastBreakLength = 0;
      uint16_t lastBreakConsumed = 0;
      bool endedByNewline = false;

      while (cursor.fieldByteOffset < field.length) {
        uint8_t first = 0;
        if (!readByte(reader, entry, field.payloadOffset + cursor.fieldByteOffset, first, error)) return false;
        if (first == '\n') {
          ++cursor.fieldByteOffset;
          endedByNewline = true;
          break;
        }

        const uint8_t codepointLength = utf8Length(first);
        if (codepointLength == 0 || static_cast<uint32_t>(cursor.fieldByteOffset) + codepointLength > field.length) {
          error = PagerError::INVALID_UTF8;
          return false;
        }
        if (lineLength + codepointLength > sizeof(line_)) {
          if (lastBreakLength == 0) {
            error = PagerError::LINE_TOO_WIDE;
            return false;
          }
          lineLength = lastBreakLength;
          cursor.fieldByteOffset = lastBreakConsumed;
          break;
        }

        for (uint8_t byteIndex = 0; byteIndex < codepointLength; ++byteIndex) {
          uint8_t byte = 0;
          if (!readByte(reader, entry, field.payloadOffset + cursor.fieldByteOffset + byteIndex, byte, error)) {
            return false;
          }
          if (byteIndex > 0 && !isContinuation(byte)) {
            error = PagerError::INVALID_UTF8;
            return false;
          }
          line_[lineLength + byteIndex] = static_cast<char>(byte);
        }
        lineLength += codepointLength;
        cursor.fieldByteOffset = static_cast<uint16_t>(cursor.fieldByteOffset + codepointLength);

        if (first == ' ' || first == '\t') {
          lastBreakLength = lineLength - 1;
          lastBreakConsumed = cursor.fieldByteOffset;
        }

        const std::string_view candidate(line_, lineLength);
        if (measurer.measure(measurer.context, candidate) > maxLineWidth) {
          if (lastBreakLength > 0) {
            lineLength = lastBreakLength;
            cursor.fieldByteOffset = lastBreakConsumed;
          } else if (lineLength > codepointLength) {
            lineLength -= codepointLength;
            cursor.fieldByteOffset = static_cast<uint16_t>(cursor.fieldByteOffset - codepointLength);
          } else {
            error = PagerError::LINE_TOO_WIDE;
            return false;
          }
          break;
        }
      }

      while (lineLength > 0 && isAsciiSpace(static_cast<uint8_t>(line_[lineLength - 1]))) --lineLength;
      if (lineLength == 0 && !endedByNewline) continue;
      if (output.textBytesUsed + lineLength > sizeof(output.text)) {
        cursor.fieldByteOffset = lineStartInField;
        output.next = cursor;
        output.hasNext = true;
        return true;
      }

      Line& outLine = output.lines[output.lineCount++];
      outLine.textOffset = output.textBytesUsed;
      outLine.textLength = static_cast<uint16_t>(lineLength);
      outLine.fieldType = field.type;
      const bool fieldStart = lineStartInField == 0;
      outLine.gapBefore = output.lineCount > 1 && (fieldStart || gapBeforeNextLine);
      gapBeforeNextLine = endedByNewline && lineLength > 0;
      if (lineLength > 0) {
        std::memcpy(output.text + output.textBytesUsed, line_, lineLength);
        output.textBytesUsed = static_cast<uint16_t>(output.textBytesUsed + lineLength);
      }
    }

    if (cursor.fieldByteOffset >= field.length) {
      cursor.fieldHeaderOffset = field.payloadOffset + field.length;
      ++cursor.fieldIndex;
      cursor.fieldByteOffset = 0;
    }
  }

  output.next = cursor;
  output.hasNext = cursor.fieldIndex < entryHeader.fieldCount;
  return true;
}

const char* pagerErrorName(const PagerError error) {
  switch (error) {
    case PagerError::NONE:
      return "none";
    case PagerError::INVALID_INPUT:
      return "invalid input";
    case PagerError::ENTRY_READ_FAILED:
      return "entry read failed";
    case PagerError::CURSOR_INVALID:
      return "cursor invalid";
    case PagerError::INVALID_UTF8:
      return "invalid utf8";
    case PagerError::LINE_TOO_WIDE:
      return "line too wide";
  }
  return "unknown";
}

}  // namespace dictionary::definition

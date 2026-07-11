## Recommendation

Implement a **Word Inbox context capture** integrated into CrossInk’s existing reader shortcuts and WebUI.

A capture stores:

1. Copyable text reconstructed from the visible page.
2. Book, chapter, page, progress, format, and capture-order metadata.
3. Optionally, the current framebuffer as a 1-bit BMP. Screenshots are disabled by default to keep capture and WebUI browsing fast.

The action should take one button press, show a small non-blocking confirmation badge, and leave the reader on the same page. No `freeink-sdk` changes, no network during reading, no new on-device activity, and no dictionary engine.

This plan targets the current clean `v1.4.0` checkout on branch `v1.4.0-dictionary`.

---

# 1. User experience

## Device workflow

1. In **Settings → Controls**, assign **Save to Word Inbox** to one of the existing shortcut slots:
   - Short Power
   - Long Power
   - Long-press Menu
   - Long-press Back
2. While reading, trigger the shortcut.
3. CrossInk captures the visible text and metadata, plus the framebuffer when **Settings → Reader → Word Inbox screenshots** is enabled.
4. A small **Saved to Word Inbox** badge appears near the bottom of the page.
5. The badge dismisses automatically and reading continues without opening a menu or moving the page.

Long-press Menu is probably the best default recommendation, but the feature should not assign itself automatically.

The existing shortcut infrastructure already exposes these configurable action lists in `src/CrossPointSettings.h:203-289` and `src/SettingsList.h:385-540`. We only append new enum values, preserving every persisted numeric value.

## WebUI workflow

Add a **Word Inbox** navigation entry:

1. Choose a book from a dropdown.
2. See context `4 of 17`.
3. Use Previous/Next buttons or keyboard arrow keys.
4. View capture metadata and screenshot.
5. Select or copy the reconstructed text.
6. Use **Copy Text** to send it to a dictionary, Anki, or another learning tool.
7. Optionally delete the current capture or all captures for the selected book.

Initial selection should be the latest capture from the latest book.

---

# 2. Format support

| Format | Screenshot | Copyable text | Notes |
| --- | --: | --: | --- |
| EPUB | Optional | Yes | Primary target and best-quality implementation |
| TXT/Markdown | Optional | Yes | `currentPageLines` already contains rendered lines |
| XTC/XTCH | Optional | No | Without screenshots, these captures retain location metadata only |
| Image preview | No | No | Not treated as a reading context |

TXT already retains the visible lines in `TxtReaderActivity::currentPageLines` (`src/activities/reader/TxtReaderActivity.h:21-24`) and renders those exact lines at `src/activities/reader/TxtReaderActivity.cpp:524-589`.

EPUB pages can be reloaded from the existing section cache. The clipping implementation currently reloads as many as three pages and builds up to 240 `WordRef` objects (`src/activities/reader/EpubReaderActivity.cpp:2960-3004`). Word Inbox should be substantially lighter: reload only the current page and extract its existing serialized line/token data.

XTC/XTCH cannot provide text without OCR because the container stores bitmap pages (`lib/Xtc/README:10-27`). OCR is explicitly outside this implementation.

---

# 3. Component architecture

## New `WordInboxStore`

Create:

```text
src/WordInboxStore.h
src/WordInboxStore.cpp
```

Responsibilities:

- Build and validate internal paths.
- Allocate the next capture ID.
- Atomically write book metadata, context metadata/text, and BMP.
- Enumerate books and captures without loading them all into RAM.
- Read one capture’s metadata or text.
- Delete one capture or one book’s inbox.
- Ignore incomplete `.tmp` files.

Suggested API:

```cpp
enum class WordInboxSaveResult : uint8_t {
  Saved,
  NoText,
  StorageError,
  ScreenshotError,
  MemoryError,
};

enum class WordInboxBookType : uint8_t {
  Epub,
  Txt,
  Xtc,
};

struct WordInboxCapture {
  WordInboxBookType bookType;
  std::string_view bookPath;
  std::string_view title;
  std::string_view author;
  std::string_view chapterTitle;

  int32_t spineIndex;
  uint32_t currentPage;
  uint32_t totalPages;
  uint8_t progressPercent;

  const char* text;
  uint32_t textLength;
  bool textTruncated;
};

class WordInboxStore {
 public:
  static WordInboxSaveResult save(
      const WordInboxCapture& capture,
      const uint8_t* framebuffer,
      int displayWidth,
      int displayHeight);

  static bool visitBooks(void* context, BookVisitor visitor);
  static bool findContext(...);
  static bool streamContextText(...);
  static bool getScreenshotPath(...);
  static bool deleteContext(...);
  static bool deleteBook(...);
};
```

Prefer function-pointer visitors plus context rather than `std::function` in the storage layer. Web enumeration should stream results and not return an unbounded vector.

## New `VisiblePageText`

Create:

```text
src/activities/reader/VisiblePageText.h
src/activities/reader/VisiblePageText.cpp
```

Responsibilities:

- Extract visible EPUB text from one `Page`.
- Extract TXT text from `currentPageLines`.
- Preserve line boundaries.
- Avoid duplicate spaces around punctuation.
- Preserve Unicode.
- Flag truncation.

Use a bounded, fallible heap buffer allocated once per capture:

```cpp
constexpr size_t MAX_VISIBLE_TEXT_BYTES = 8192;
```

Do not place it on the task stack. Allocate with `makeUniqueNoThrow<char[]>`, null-check it, fill it without growth, then free it after saving.

An 8 KB cap is reasonable because it exceeds a normal visible page while bounding largest-block pressure. If exceeded, truncate at a valid UTF-8 boundary and set `textTruncated`. The WebUI should display a warning.

## Reader-specific adapters

Add a private method to each reader:

```cpp
void saveCurrentPageToWordInbox();
```

Do not add a generic method to `Activity` yet. Each reader owns different text state, and forcing a virtual capture abstraction into every activity would be unnecessary surface.

### EPUB adapter

Under `RenderLock`:

1. Validate `epub`, `section`, and current page.
2. Reload exactly the current `Page`.
3. Extract its line text.
4. Collect title, author, TOC chapter title, page, spine, and percentage.
5. Pass the existing framebuffer and metadata to `WordInboxStore::save()`.
6. Draw confirmation after the save.

The framebuffer already contains the final page, status bar, highlights, and publisher markers because page composition is completed in `EpubReaderActivity::renderContents()` (`src/activities/reader/EpubReaderActivity.cpp:4237-4289`).

### TXT adapter

Under `RenderLock`:

1. Use `currentPageLines`.
2. Join lines into the bounded text buffer.
3. Use the existing `ScreenshotInfo` calculations for page/progress.
4. Save through the same store.

### XTC adapter

Save metadata and BMP with an empty text payload and `textAvailable=false`.

---

# 4. Storage layout

Use a protected internal directory:

```text
/.crosspoint/word_inbox/
├── epub_1234567890/
│   ├── book.bin
│   ├── 00000001.ctx
│   ├── 00000001.bmp
│   ├── 00000002.ctx
│   └── 00000002.bmp
├── txt_987654321/
│   └── ...
└── xtc_456789123/
    └── ...
```

The book key should initially follow the existing clipping/bookmark pattern:

```text
<book-type>_<CRC32(book path)>
```

That is consistent with `ClippingStore` rather than introducing a new document-identity subsystem. `book.bin` stores the original path so hash collisions can be detected rather than silently merged.

A later revision could switch to content identity, but that would add hashing work and migration complexity to an otherwise small feature.

## `book.bin` version 1

```text
magic               4 bytes: "WIBK"
version              uint8: 1
book type            uint8
title                length-prefixed UTF-8
author               length-prefixed UTF-8
book path             length-prefixed UTF-8
```

## `*.ctx` version 1

```text
magic                 4 bytes: "WICT"
version               uint8: 1
flags                 uint8
capture ID            uint32
spine index           int32, -1 when unavailable
current page          uint32, 1-based
total pages           uint32
progress percentage   uint8
chapter title         length-prefixed UTF-8
text length           uint32
text bytes            UTF-8
```

Flags:

```text
bit 0: text available
bit 1: text truncated
bit 2: screenshot available
```

Capture IDs are monotonically increasing per book. Determine the next ID by scanning numeric `.ctx` filenames and selecting `max + 1`. This is an occasional cold-path directory scan and avoids maintaining/recovering another mutable counter file.

## Atomic write protocol

1. Ensure root and book directories exist.
2. Update `book.bin` through `book.bin.tmp`, `sync()`, close, rename.
3. When enabled, write the screenshot to `<id>.bmp.tmp`.
4. Write context to `<id>.ctx.tmp`, `sync()`, close.
5. When present, rename BMP to its final name.
6. Rename context last; the `.ctx` rename is the commit point.
7. On failure, remove temporary and partially promoted files.

The WebUI enumerates only valid final `.ctx` files. Therefore power loss cannot expose a capture without readable metadata.

---

# 5. Screenshot handling

Reuse:

```cpp
ScreenshotUtil::saveFramebufferAsBmp(...)
```

Do not use `ScreenshotUtil::takeScreenshot()`, because that adds a border, waits a second, restores a framebuffer snapshot, and performs another refresh (`src/util/ScreenshotUtil.cpp:70-101`).

The lower-level writer already:

- Streams directly from the framebuffer.
- Uses a fixed 68-byte row buffer.
- Rotates output without allocating another framebuffer.
- Supports both X3 and X4 dimensions (`src/util/ScreenshotUtil.cpp:104-186`).

One caveat: this records the composed 1-bit framebuffer, not the panel’s transient grayscale waveform state. It preserves content, layout, status bars, dark mode, and image composition, but anti-alias gray levels may not exactly match the physical panel.

With screenshots enabled, expected SD usage is approximately one framebuffer-sized BMP plus text per capture: around 50–60 KB, depending on device dimensions and metadata. With the default text-only mode, captures are bounded at 8 KB plus small metadata and usually much smaller. There is no additional framebuffer allocation.

---

# 6. Shortcut integration

Append these enum values immediately before their count sentinels:

```cpp
SAVE_WORD_INBOX = 22,
LONG_MENU_SAVE_WORD_INBOX = 21,
```

Existing values must not move because they are persisted raw (`src/CrossPointSettings.h:203-289`).

Add `STR_SAVE_WORD_INBOX` to translation YAML sources and append it to all four existing action lists in `src/SettingsList.h:385-540`:

- Short Power
- Long Power
- Long-press Menu
- Long-press Back

The generated Settings WebUI will receive the new option automatically because settings serialization is driven from `SettingsList`.

Dispatch changes:

- `EpubReaderActivity::executeReaderQuickAction()`
- EPUB short/long Power mappings
- `TxtReaderActivity::executePowerButtonAction()`
- `TxtReaderActivity::executeLongPressBackAction()`
- Equivalent XTC handlers
- `GlobalActions.h` should classify it as reader-only, like Create Clipping

Do not add it as an ordinary reader-menu item in the MVP. Opening the menu would change the framebuffer before capture and defeat “save exactly what I am viewing.”

---

# 7. Confirmation behavior

Do not use the existing screenshot animation or a modal activity.

After successful persistence:

- Back up only the compact toast region.
- Draw **Saved to Word Inbox** near the bottom and perform one FAST refresh.
- After 1.2 seconds, restore that region and perform another differential FAST refresh.

On failure, show **Could not save to Word Inbox** for 2 seconds and log the exact reason.

Repeated captures restore the clean framebuffer before persistence, preventing feedback from entering a screenshot. A page render cancels the pending toast restoration because the new page replaces it.

The feedback drawing should be a shared helper in `ReaderUtils` or a small reusable reader helper, not copied across three readers.

---

# 8. Web API

Add routes in `CrossPointWebServer::begin()`, beside the existing settings/font APIs (`src/network/CrossPointWebServer.cpp:180-226`):

```http
GET  /word-inbox
GET  /api/word-inbox/books
GET  /api/word-inbox/context?book=epub_123&id=42
GET  /api/word-inbox/text?book=epub_123&id=42
GET  /api/word-inbox/image?book=epub_123&id=42
POST /api/word-inbox/delete
POST /api/word-inbox/delete-book
```

## Books response

Stream a bounded JSON array:

```json
[
  {
    "key": "epub_1234567890",
    "title": "Der Prozess",
    "author": "Franz Kafka",
    "type": "epub",
    "count": 17
  }
]
```

## Context response

Return metadata only:

```json
{
  "id": 42,
  "position": 4,
  "count": 17,
  "previousId": 41,
  "nextId": 46,
  "chapter": "Kapitel Drei",
  "page": 8,
  "totalPages": 21,
  "progress": 37,
  "hasText": true,
  "textTruncated": false,
  "hasImage": true
}
```

A single directory scan can compute count, rank, previous ID, and next ID without allocating a context list.

## Text and image responses

Serve text and BMP separately rather than embedding either in JSON:

- Text: `text/plain; charset=utf-8`
- Image: `image/bmp`
- Stream from SD using a fixed 512-byte or 1 KB reusable buffer.
- Do not allocate a complete response body.
- Reset the watchdog and yield during streaming.

All query parameters must be strict internal identifiers, not paths. Accept only validated book keys and unsigned decimal IDs. Never let an endpoint expose arbitrary files under `/.crosspoint`.

---

# 9. Web frontend

Add source files:

```text
web/pages/word-inbox.html
web/pages/word-inbox.css
web/pages/word-inbox.js
```

Update:

```text
web/templates/base.html
scripts/build_web.py
scripts/preview_web.py
```

Generated headers remain build outputs and must not be edited directly. The web build currently discovers an explicit page map in `scripts/build_web.py:27-33` and composes sources into compressed headers at `scripts/build_web.py:90-105`.

Suggested UI:

```text
[Book selector: Der Prozess (17)]

← Previous       Context 4 of 17       Next →

Chapter Drei · page 8/21 · 37%

┌─────────────────────────────────────┐
│ Screenshot                          │
└─────────────────────────────────────┘

[Copy Text]

Das Gespräch wurde plötzlich ...
```

Use a `<pre>` or read-only `<textarea>` for text so line breaks survive and selection works naturally. Add:

- Copy Text button
- Keyboard Left/Right navigation
- Latest/oldest jump
- Delete Current
- Empty and error states
- Truncation warning
- “Text unavailable for pre-rendered XTC page” state

Update the preview server with mock books, context metadata, text, and image responses so the WebUI can be developed without firmware or hardware (`scripts/preview_web.py:23-55`, `scripts/preview_web.py:103-123`).

---

# 10. Memory and concurrency contract

- No second framebuffer.
- Hold `RenderLock` while reading the displayed framebuffer and reader-owned page state.
- Allocate at most one bounded visible-text buffer per capture.
- Use `makeUniqueNoThrow<char[]>`; never bare `new`.
- No unbounded vectors when enumerating books or captures.
- Stream API responses.
- Close/sync files before rename or delete.
- Log free heap and maximum allocatable block around capture during initial development.
- Do not retain capture text after the shortcut returns.
- Do not run capture work in a new FreeRTOS task.

The feature’s steady-state RAM cost should be only small code/static state. Its transient cost is the deserialized EPUB page plus a maximum 8 KB text buffer. The BMP writer adds only its existing 68-byte row buffer.

---

# 11. Implementation sequence

## Phase 1: storage and extraction

1. Add `WordInboxStore`.
2. Define and document binary formats.
3. Implement atomic save and enumeration.
4. Add bounded visible-text extraction for EPUB and TXT.
5. Reuse `ScreenshotUtil::saveFramebufferAsBmp`.
6. Add tests for malformed/truncated records, ID allocation, UTF-8 truncation, punctuation, line breaks, and inserted hyphens.

## Phase 2: reader integration

1. Append stable enum values.
2. Add translations and SettingsList options.
3. Add EPUB capture action.
4. Add TXT/Markdown capture action.
5. Add XTC screenshot-only capture action.
6. Add the one-refresh feedback badge.
7. Extend simulator smoke flow to trigger capture and assert `.ctx` and `.bmp` files exist.

## Phase 3: WebUI

1. Add routes and handlers.
2. Stream book summaries, text, and BMP.
3. Add the Word Inbox page and navigation.
4. Add delete operations.
5. Add preview-server mock endpoints.
6. Document API endpoints and user workflow.

## Phase 4: integration polish

1. Preserve inbox records when ordinary reading caches are cleared.
2. Do not delete inbox records when the source book is deleted; they are learning data.
3. Optionally migrate EPUB path-keyed inbox directories in `BookMoveUtils`, following bookmark/clipping migration.
4. Add `docs/word-inbox.md`.
5. Add `docs/file-formats.md` entries.
6. Add an `Added` entry to `CHANGELOG.md`.

---

# 12. Verification

Automated checks:

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run -e simulator
./scripts/run_simulator_smoke_test.py
pio run -e default
pio run -e tiny -e xlarge
```

Web checks:

```sh
python3 scripts/preview_web.py
```

Hardware checks on both X3 and X4:

1. Bind the action to long-press Menu.
2. Open a German EPUB with punctuation, umlauts, ß, italics, and hyphenated line endings.
3. Capture in portrait, landscape, dark mode, and anti-aliased mode.
4. Confirm one shortcut invocation creates exactly one context.
5. Confirm page position and reading stats are unchanged.
6. Open File Transfer → Word Inbox.
7. Verify screenshot orientation/layout and copyable UTF-8 text.
8. Navigate multiple contexts and multiple books.
9. Delete a context and verify both `.ctx` and `.bmp` disappear.
10. Interrupt power during a capture and confirm `.tmp` files are ignored.
11. Test low-memory failure and verify it logs and returns safely.
12. Test TXT/Markdown and verify line preservation.
13. Test XTC and verify screenshot-only messaging.

## Overall effort

This is a **moderate feature**, not an overhaul: approximately 4–7 focused engineering days including WebUI, tests, and hardware validation. The risky parts are text reconstruction quality and atomic SD persistence; the shortcut and screenshot plumbing already exist.

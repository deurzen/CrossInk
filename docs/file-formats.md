# File Formats

These formats describe the SD-card cache files under `/.crosspoint/epub_<hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8 unless a format notes a
fixed-size char buffer.

## EPUB `META-INF/crossink/language.bin`

### Version 1

Dictionary-compatible optimized EPUBs contain `META-INF/crossink/language.bin`
as an uncompressed ZIP member. It maps stable source-text shards to dictionary
lexemes without tying those shards to a particular font, orientation, or page
layout. Firmware validates the embedded artifact before extracting it to the
book's render-cache directory for random access. The embedded member remains
the source of truth; user learning state is stored separately and is not part
of this file.

All integers are unsigned little-endian. All offsets are absolute file offsets,
are four-byte aligned, and point to non-overlapping sections in the order shown
below. CRC32 uses the standard zlib polynomial and representation. The header
CRC covers bytes `[0, 104)`; the payload CRC covers bytes `[108, fileSize)`.

Fixed 108-byte header:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `CXLG` |
| 4 | 2 | Format version (`1`) |
| 6 | 2 | Header size (`108`) |
| 8 | 4 | Flags; version 1 requires zero |
| 12 | 2 | Tokenizer version |
| 14 | 2 | Analyzer version |
| 16 | 16 | Dictionary bundle UUID; an all-zero UUID is invalid |
| 32 | 8 | Zero-padded source-language tag, maximum 7 ASCII bytes |
| 40 | 8 | Zero-padded target-language tag, maximum 7 ASCII bytes |
| 48 | 2 | Spine count |
| 50 | 2 | Reserved; must be zero |
| 52 | 4 | Shard count |
| 56 | 4 | Total shard-candidate record count |
| 60 | 4 | Local lemma count |
| 64 | 4 | Local surface count |
| 68 | 4 | Spine-directory offset |
| 72 | 4 | Shard-directory offset |
| 76 | 4 | Shard-candidate table offset |
| 80 | 4 | Local-lemma table offset |
| 84 | 4 | Global-to-local table offset |
| 88 | 4 | Surface-detail section offset |
| 92 | 4 | Metadata section offset |
| 96 | 4 | Exact file size |
| 100 | 4 | Payload CRC32 |
| 104 | 4 | Header CRC32 |

Firmware version-1 limits are part of the format contract: 64 MiB maximum file
size, 4096 spines, 65535 shards, 1,000,000 shard-candidate records, 32768 local
lemmas, and 65535 local surfaces. Compilers must fail rather than emit an
artifact beyond these limits.

Fixed table records:

| Table | Record size | Version-1 record |
| --- | ---: | --- |
| Spine directory | 8 | `firstShard:u32`, `shardCount:u32` |
| Shard directory | 16 | `firstRecord:u32`, `recordCount:u16`, `reserved:u16`, `sourceTokenStart:u32`, `sourceTokenEnd:u32` |
| Shard candidates | 16 | `surfaceHash:u64`, `localSurfaceId:u16`, `primaryLocalLemmaId:u16`, `alternateLocalLemmaId:u16`, `surfaceByteLength:u8`, `flags:u8` |
| Local lemmas | 4 | `globalLexemeId:u32` indexed by local lemma ID |
| Global-to-local | 8 | `globalLexemeId:u32`, `localLemmaId:u16`, `reserved:u16`, sorted by global ID |

`surfaceHash` is FNV-1a-64 over the exact NFC UTF-8 surface. Candidate flag bit
0 marks ambiguity, bit 1 marks an offline-split compound, and bit 2 marks a
case-folded fallback rather than an exact surface match. Other bits are invalid
in version 1. `alternateLocalLemmaId` is `UINT16_MAX` when absent; both lemma IDs
are `UINT16_MAX` for a compound without its own dictionary entry.

The surface-detail section begins with this 40-byte header. Its offsets are
relative to the start of the section and are four-byte aligned:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `CXSD` |
| 4 | 2 | Surface-detail version (`1`) |
| 6 | 2 | Header size (`40`) |
| 8 | 4 | Surface count |
| 12 | 4 | Analysis count |
| 16 | 4 | Compound-component count |
| 20 | 4 | Surface-record offset |
| 24 | 4 | Analysis-array offset |
| 28 | 4 | Component-array offset |
| 32 | 4 | UTF-8 string-pool offset |
| 36 | 4 | Exact section size |

Each local surface ID indexes a 20-byte record containing
`stringOffset:u32`, `firstAnalysis:u32`, `firstComponent:u32`,
`stringLength:u16`, `analysisCount:u8`, `componentCount:u8`,
`confidence:u16`, and `flags:u16`. String offsets are relative to the surface
string pool. Analysis and component arrays contain local lemma IDs as `u16`.
Confidence ranges from 0 to 1000. This preserves all plausible analyses and
compound components outside the hot shard record.

The metadata section starts with `CXLM`, `version:u16` (`1`), `headerSize:u16`
(`16`), `jsonLength:u32`, and reserved zero `u32`, followed by deterministic
UTF-8 JSON. Version 1 records tokenizer/analyzer versions and the 64-token shard
size; firmware can ignore this cold-path metadata after compatibility checks.

The allocation-free header validator lives in
`lib/Dictionary/BookLanguageFormat.*`. It rejects unsupported versions and
flags, malformed language tags, zero dictionary identities, excessive counts,
misaligned or overlapping tables, mismatched file sizes, and CRC failures
before later readers seek into a table. `BookLanguageReader` then provides
allocation-free, range-checked access to shard candidates, local lexeme IDs,
and surface details from the validated artifact.

On EPUB load, firmware streams the embedded member through a fixed-header CRC
validator into `<book-cache>/language.bin.tmp`, syncs it, checks tokenizer and
analyzer versions, spine count, and source language, then renames it to
`language.bin` as the commit point. Cached artifacts are CRC-checked with a
96-byte read buffer before reuse. No payload-sized allocation is made.

A deterministically invalid embedded artifact creates `<book-cache>/language.invalid`:
magic `CXLI` followed by its `fileSize:u32`. This prevents repeated extraction
attempts by multiple short-lived `Epub` objects. A missing member removes stale
language cache files, a changed member size retries validation, and clearing the
book cache removes the marker. Transient open/read/write failures do not create
the marker and can therefore recover on the next open.

## Dictionary learning state

Global learning state is stored outside EPUB caches under
`/.crosspoint/language-state/<bundle-uuid>/`. `state.meta` is 36 bytes:
`CXSM`, version/header size (`u16`, `u16`), bundle UUID (16 bytes), global
lexeme count (`u32`), reserved zero (`u32`), and CRC32 over the first 32 bytes.

`status.bin` starts with a 32-byte `CXST` header containing version/header size,
bundle UUID, global lexeme count, and generation (`u32`). Four-bit statuses are
packed low nibble first: 0 unseen, 1 known, 2 learning, 3 ignored, and 4
implicitly familiar. All nonzero states suppress a lexeme from ordinary
shortlists.

A 40-byte `status.wal` (`CXWL`) records bundle UUID, lexeme ID, target
generation, new status, three reserved bytes, and CRC32. Updates sync the WAL,
status byte, and generation in that order. Recovery idempotently replays the WAL
before removing it.

Each EPUB cache may contain `dictionary-suppress.bin`: a 36-byte `CXSP` header
with bundle UUID, local lemma count, global generation, and payload byte count,
followed by one suppression bit per local lemma. Generation or format mismatch
causes an atomic rebuild from `language.bin` and global state. Deleting an EPUB
cache removes only this reproducible projection, never global learning state.

## `/.crosspoint/dictionaries/<bundle-uuid>/`

### Native dictionary package version 1

An installed runtime dictionary is a directory committed under its 128-bit
bundle UUID. Installation uses a temporary directory and publishes the final
directory name only after all file sizes and CRCs match `meta.bin`. The runtime
package contains no morphology tables; those remain in the desktop compiler
half of the `.cpdict` distribution.

```text
meta.bin
lexemes.bin
headwords.bin
entries.bin
licenses.txt
```

`meta.bin` is exactly 80 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `CXDM` |
| 4 | 2 | Package version (`1`) |
| 6 | 2 | Metadata size (`80`) |
| 8 | 4 | Flags; version 1 requires zero |
| 12 | 16 | Nonzero dictionary bundle UUID |
| 28 | 8 | Zero-padded source-language tag, maximum 7 ASCII bytes |
| 36 | 8 | Zero-padded target-language tag, maximum 7 ASCII bytes |
| 44 | 4 | Dense lexeme count |
| 48 | 2 | Lexeme record size (`24`) |
| 50 | 2 | Reserved; must be zero |
| 52 | 4 | Exact `lexemes.bin` size |
| 56 | 4 | Exact `headwords.bin` size |
| 60 | 4 | Exact `entries.bin` size |
| 64 | 4 | `lexemes.bin` CRC32 |
| 68 | 4 | `headwords.bin` CRC32 |
| 72 | 4 | `entries.bin` CRC32 |
| 76 | 4 | Metadata CRC32 over bytes `[0, 76)` |

Lexeme IDs are zero-based indexes into `lexemes.bin`. Each 24-byte record is:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Headword offset into `headwords.bin` |
| 4 | 4 | Entry offset into `entries.bin` |
| 8 | 4 | Entry byte length |
| 12 | 8 | Stable lexeme-key hash used as a future migration hint |
| 20 | 2 | Headword byte length, excluding any terminator |
| 22 | 1 | Coarse part of speech |
| 23 | 1 | Lexeme flags |

`headwords.bin` is a packed UTF-8 string pool with no terminators. Headwords are
NFC-composed and limited to 96 bytes. The lexeme-key hash is FNV-1a-64 over the
exact NFC headword bytes, followed by byte `0x1f` and the one-byte part-of-speech
value. A hash is not a dictionary identity and must never be accepted without
comparing the associated headword and part of speech during migration.

Part-of-speech values are `0` unknown, `1` noun, `2` verb, `3` adjective, `4`
adverb, `5` pronoun, `6` article/determiner, `7` preposition, `8` conjunction,
`9` numeral, `10` particle, `11` interjection, `12` proper noun, `13` phrase,
`14` abbreviation, and `15` other. Lexeme flag bit 0 marks a compound with
component details; bit 1 marks a generated rather than source-authored entry.
Other bits are invalid in version 1.

Each entry slice in `entries.bin` begins with `entryVersion:u8` (`1`),
`flags:u8`, and `fieldCount:u16`. It is followed by `fieldCount` fields encoded
as `type:u8`, `flags:u8`, `byteLength:u16`, and UTF-8 payload bytes. Version-1
field types are definition (`1`), part-of-speech label (`2`), example (`3`),
usage note (`4`), etymology (`5`), cross-reference (`6`), and compound component
(`7`). Unknown field types can be skipped by length. Definitions are rendered
by streaming one field at a time; firmware does not materialize the full entry.
An individual entry is limited to 1 MiB even though its fields are individually
limited to 65535 bytes.

Firmware caps packages at 500,000 lexemes, 64 MiB of headwords, and 1 GiB of
entries. `lib/Dictionary/DictionaryPackage.*` performs dense-ID lookup with
caller-owned headword and entry buffers. It retains callback descriptors, not
open files or dictionary contents, so a HAL adapter can open and close only the
single SD file needed for an operation.

`licenses.txt` is UTF-8 attribution displayed by the dictionary management UI.
It is required by the installer but is deliberately outside the hot-path binary
metadata.

### Compiler `forms.bin`

The desktop half of a `.cpdict` archive contains `compiler/forms.bin`. This file
is not uploaded to the reader. It provides exact NFC surface-form analyses to
the browser book compiler and uses the same bundle UUID and global lexeme IDs as
the runtime package.

Its fixed 64-byte header is:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `CXDF` |
| 4 | 2 | Format version (`1`) |
| 6 | 2 | Header size (`64`) |
| 8 | 16 | Dictionary bundle UUID |
| 24 | 4 | Form count |
| 28 | 4 | Analysis count |
| 32 | 4 | Form-directory offset |
| 36 | 4 | Analysis-table offset |
| 40 | 4 | UTF-8 string-pool offset |
| 44 | 4 | String-pool size |
| 48 | 4 | Exact file size |
| 52 | 4 | Reserved; must be zero |
| 56 | 4 | Payload CRC32 over bytes `[64, fileSize)` |
| 60 | 4 | Header CRC32 over bytes `[0, 60)` |

Each 20-byte form-directory record contains `surfaceHash:u64`,
`stringOffset:u32`, `firstAnalysis:u32`, `stringLength:u16`,
`analysisCount:u8`, and `flags:u8`. Records are ordered by FNV-1a-64 hash and
then exact UTF-8 bytes; hash matches must therefore still compare the string.
Each 8-byte analysis contains `globalLexemeId:u32`, `confidence:u16` in the
range 0–1000, and `flags:u16`. Version 1 emits confidence 1000 for explicit
source forms and zero flags. A form can retain up to 255 analyses.

Compiler caps are 2,000,000 forms, 4,000,000 analyses, 255 UTF-8 bytes per form,
and a 512 MiB string pool. These are desktop bounds; none of these tables are
loaded by firmware.

## `book.bin`

### Version 8

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.
Version 8 stores book and TOC title strings NFC-composed so decomposed
diacritics render correctly with device fonts.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 8
#define MAX_STRING_LENGTH 65535

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("Book language code")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
};

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative uncompressed spine size through this entry")]];
    s16 tocIndex [[comment("Index into TOC, or inherited/previous TOC index when no direct entry exists")]];
};

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)")]];
};

struct BookBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables")]];
    u16 spineCount;
    u16 tocCount;

    Metadata metadata;

    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    u32 spineLut[spineCount] [[comment("Spine entry offsets")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets")]];

    SpineEntry spines[spineCount];
    TocEntry toc[tocCount];
};

BookBin book @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `reader_settings.bin`

### Version 2

Each EPUB cache directory may contain `reader_settings.bin`. Missing files mean
the book uses global Reader settings and the default auto-page-turn interval.

Version 1 stored only:

- `u8 version`
- `u16 autoPageTurnSeconds`

Version 2 stores flags before the full reader-settings snapshot. This lets the
file preserve an auto-page-turn interval without forcing custom font/layout
settings for the book. It also stores a per-book EPUB render mode override,
which can be changed from book action menus before opening the book so a
problematic EPUB can be moved to Balanced or Light rendering without entering
the reader first. Safe Mode also uses this file to save Light rendering with
embedded styles, Bionic Reading, and Guide Dots disabled after that final
fallback successfully opens a difficult book.

```c++
struct ReaderSettingsBin {
    u8 version; // 2
    u8 flags;   // bit 0 = custom reader settings, bit 1 = custom auto-page-turn interval, bit 2 = render mode override
    u16 autoPageTurnSeconds;
    u8 renderMode; // 0 = CrossInk Default, 1 = Balanced, 2 = Light

    u8 fontFamily;
    u8 fontSize;
    u8 lineHeightPercent;
    u8 orientation;
    u8 screenMargin;
    u8 publisherPageNumbers;
    u8 paragraphAlignment;
    u8 embeddedStyle;
    u8 hyphenationEnabled;
    u8 textAntiAliasing;
    u8 readerDarkMode;
    u8 imageRendering;
    u8 extraParagraphSpacing;
    u8 forceParagraphIndents;
    u8 bionicReadingEnabled;
    u8 guideReadingEnabled;
    u8 snapshotRenderMode;
    char sdFontFamilyName[64];
};
```

## `/.crosspoint/clippings/<bookType>_<crc32(path)>.bin`

### Version 1

Clipping files store the per-book EPUB clipping list used by the reader. A
saved clipping is also what CrossInk renders as an in-reader highlight; there is
no separate highlight file. The file lives in `/.crosspoint/clippings/` instead
of the EPUB render-cache directory so clearing/rebuilding layout cache does not
delete user clippings.

The current implementation only writes EPUB clipping files, so `bookType` is
`epub`. The numeric suffix is `uzlib_crc32()` of the book's SD-card path, for
example:

```text
/.crosspoint/clippings/epub_1234567890.bin
```

Binary layout:

- `[0]` version (`1`)
- `[1-2]` clipping count (`uint16_t` LE, maximum `64`)
- book title (`String`)
- book author (`String`)
- book path (`String`)
- repeated clipping records:
  - `spineIndex` (`uint16_t` LE)
  - `startPage` (`uint16_t` LE)
  - `endPage` (`uint16_t` LE)
  - `pageCount` (`uint16_t` LE, at least `1`)
  - `startWordIndex` (`uint16_t` LE)
  - `endWordIndex` (`uint16_t` LE)
  - `wordCount` (`uint16_t` LE)
  - `paragraphIndex` (`uint16_t` LE, `UINT16_MAX` when unavailable)
  - `timestamp` (`uint32_t` LE, seconds since firmware boot when saved)
  - `chapterTitle` (`char[48]`, null-terminated/truncated)
  - selected text (`String`, truncated to `512` bytes for the in-app store)

CrossInk uses the stored spine/page/paragraph fields as anchors, then searches
near that location for the stored clipping text after relayout. This is similar
to keeping both a DOM position and a text quote in a web app: the numeric
position gives a fast starting point, while the text makes jumps and highlights
survive font, layout, or page-count changes when possible.

Creating a clipping also appends a Kindle-style export entry to
`/My Clippings.txt` on the SD-card root. That text export can keep up to `2000`
bytes of the selected text and is append-only. Removing a clipping from the
reader deletes or rewrites only the binary clipping file; it does not remove
previous entries from `/My Clippings.txt`.

When CrossInk moves an EPUB through its built-in move-to-Read flow, it rewrites
the clipping file under the new path-derived name and removes the old one. If a
book is renamed or moved outside CrossInk, the path hash changes, so the old
clipping file may no longer be associated with the book until the file is moved
back or the clipping store is migrated.

## `/.crosspoint/word_inbox/<bookType>_<crc32(path)>/`

### Version 1

Word Inbox data is kept outside render-cache directories so clearing an EPUB,
TXT, or XTC cache does not remove saved learning contexts. The directory suffix
is `uzlib_crc32()` of the source book's SD-card path. A valid capture always has
a context file and may have a BMP with the same eight-digit ID when screenshots
were enabled:

```text
/.crosspoint/word_inbox/epub_1234567890/book.bin
/.crosspoint/word_inbox/epub_1234567890/00000001.ctx
/.crosspoint/word_inbox/epub_1234567890/00000001.bmp
```

`book.bin` layout:

- magic `WIBK` (`char[4]`)
- version (`uint8_t`, currently `1`)
- book type (`uint8_t`: `1` EPUB, `2` TXT/Markdown, `3` XTC/XTCH)
- title (`String`, maximum 512 bytes)
- author (`String`, maximum 512 bytes)
- source path (`String`, maximum 1024 bytes)

Each `.ctx` layout:

- magic `WICT` (`char[4]`)
- version (`uint8_t`, currently `1`)
- flags (`uint8_t`: bit 0 text available, bit 1 text truncated, bit 2 screenshot available)
- capture ID (`uint32_t`)
- spine index (`int32_t`, `-1` when unavailable)
- current page (`uint32_t`)
- total pages (`uint32_t`)
- progress percentage (`uint8_t`, clamped to 0–100)
- chapter title (`String`, maximum 512 bytes)
- visible text (`String`, maximum 8192 bytes)

Each book directory also contains `index.bin` and normally `index.bin.bak`.
The version-1 index layout is:

- magic `WIIX` (`char[4]`)
- index version (`uint8_t`, currently `1`)
- generation, context count, earliest ID, latest ID, and next ID (`uint32_t` each)
- sorted context IDs (`uint32_t[count]`)

The fixed-width ID array supports binary-search navigation without scanning or
opening every context. Existing directories build this index once on first use.
Invalid indexes, interrupted additions, and missing indexed neighbors trigger a
bounded-memory rebuild from valid `.ctx` files. Index replacement uses a synced
`.tmp` plus the previous `.bak` generation; no ID vector is retained in RAM.

Capture writes use `.tmp` files. When present, the BMP is renamed first and the
`.ctx` file next; publishing the updated index makes the capture visible.
Readers ignore temporary files and treat a missing same-ID BMP as incomplete
only when bit 2 is set. Deletion records the ID in `delete.pending`, renames the
context to `.ctx.del`, then publishes an index without that ID. The next access
finishes any interrupted sequence, and rebuilds cannot resurrect deleted
contexts. The stored source path also detects the unlikely case
where two paths produce the same CRC32 directory name.

## `stats_v5.bin`

### Version 5

`stats_v5.bin` stores per-book reading statistics for stats schema version 5.
Versioned filenames let firmware branches with different stats schemas keep
their own per-book stats files without overwriting each other. Version 5 extends
version 4 with a cached live reader book time-left estimate so Home and Reading
Stats can show the same estimate the reader last computed.

When `stats_v5.bin` is missing, CrossInk can read the previous versioned stats
filename (`stats_v4.bin` for version 5, `stats_v5.bin` after a future version 6
bump) before falling back to legacy `stats.bin` files with compatible stats
payloads. Future changes are always saved to the current versioned filename.

Binary layout:

- `[0]` version (`5`)
- `[1-2]` `sessionCount` (`uint16_t` LE)
- `[3-6]` `totalReadingSeconds` (`uint32_t` LE)
- `[7-10]` `totalPagesTurned` (`uint32_t` LE)
- `[11]` `isCompleted` (`uint8_t`)
- `[12-13]` `avgSecondsPerForwardPage` (`uint16_t` LE)
- `[14-15]` `paceSampleCount` (`uint16_t` LE)
- `[16]` flags (`bit0=startDateManual`, `bit1=finishedDateManual`)
- `[17-20]` `startDate` (`year uint16_t` LE, `month uint8_t`, `day uint8_t`)
- `[21-24]` `finishedDate` (`year uint16_t` LE, `month uint8_t`, `day uint8_t`)
- `[25-40]` `timeOfDaySeconds[4]` (`uint32_t` LE each)
- `[41-68]` `dayOfWeekSeconds[7]` (`uint32_t` LE each)
- `[69-72]` `estimatedTimeLeftSeconds` (`uint32_t` LE, `0` means unavailable)

## `section.bin`

### Version 44

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 44 invalidates older section caches so `TextBlock` word data can be
stored as one flat arena. It includes:

- cache-busting fields for font, line compression, extra paragraph spacing,
  forced paragraph indents, paragraph alignment, viewport size, hyphenation,
  embedded CSS, image rendering mode, Bionic Reading, Guide Dots, and EPUB
  render mode
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
- optional per-word Bionic Reading split metadata
- optional per-word Guide Dot x-offset metadata
- optional per-word text flags for CSS backgrounds and layout-inserted hyphens
- reading-aid layout that stores Bionic Reading and Guide Dots as per-word metadata instead of temporary layout words
- publisher CSS page-break handling and adjusted justification spacing baked into page layout
- table fragments
- per-page footnote entries
- per-page publisher page markers
- serialized word style bits for underline, strikethrough, superscript, and
  subscript
- flat TextBlock word storage: per-word arrays plus one shared NUL-terminated
  text blob, replacing length-prefixed word strings and parallel vectors. The
  on-disk order mirrors the in-RAM arena so the firmware reads a whole block
  payload with a single allocation and a single SD read

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 44
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

enum PageElementTag : u8 {
    TAG_PageLine = 1,
    TAG_PageImage = 2,
    TAG_PageTableFragment = 3,
    TAG_PageHorizontalRule = 4
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32
};

enum TextAlign : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    NONE = 4
};

struct BlockStyle {
    TextAlign alignment;
    bool textAlignDefined;
    s16 marginTop;
    s16 marginBottom;
    s16 marginLeft;
    s16 marginRight;
    s16 paddingTop;
    s16 paddingBottom;
    s16 paddingLeft;
    s16 paddingRight;
    s16 textIndent;
    bool textIndentDefined;
    bool isRtl;
    bool directionDefined;
};

struct TextBlock {
    u16 wordCount;
    u8 hasBionic;
    u8 hasGuideDots;
    u8 hasWordFlags;
    u16 textBytes [[comment("Total size of text[], including one NUL per word")]];

    if (wordCount > 0) {
        u16 textOff[wordCount] [[comment("Byte offset of word i's text within text[]")]];
        s16 wordXPos[wordCount];
        if (hasBionic != 0) {
            u16 wordBionicSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
        }
        if (hasGuideDots != 0) {
            u16 wordGuideDotXOffset[wordCount] [[comment("Guide dot x offset from word start; 0 means no dot")]];
        }
        WordStyle wordStyle[wordCount];
        if (hasBionic != 0) {
            u8 wordBionicBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        }
        if (hasWordFlags != 0) {
            u8 wordFlags[wordCount] [[comment("bit 0 = black background, bit 1 = layout-inserted trailing hyphen")]];
        }
        char text[textBytes] [[comment("All words back to back, each NUL-terminated")]];
    }

    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
    s16 width;
    s16 height;
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock block;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock image;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct TableFragmentCell {
    bool isHeader;
    u8 lineCount;
    TextBlock lines[lineCount];
};

struct TableFragmentRow {
    u16 height;
    bool headerSeparator;
    u8 cellCount;
    TableFragmentCell cells[cellCount];
};

struct PageTableFragment {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 columnCount;
    u8 cellPadding;
    u16 lineHeight;
    u8 rowCount;
    TableFragmentRow rows[rowCount];
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageTableFragment) {
        PageTableFragment tableFragment [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
};

struct PublisherPageMarker {
    s16 yPos;
    char label[16];
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];

    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount];

    u8 publisherPageMarkerCount;
    PublisherPageMarker publisherPageMarkers[publisherPageMarkerCount];
};

struct AnchorEntry {
    String anchor;
    u16 page;
};

struct AnchorMap {
    u16 count;
    AnchorEntry entries[count];
};

struct ParagraphLut {
    u16 count;
    u16 paragraphIndex[count];
};

struct SectionBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    bool forceParagraphIndents;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    u8 imageRendering;
    bool bionicReadingEnabled;
    bool guideReadingEnabled;
    u8 renderMode; // 0 = CrossInk Default, 1 = Balanced, 2 = Light

    u16 pageCount;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;

    Page pages[pageCount];

    u32 currentOffset = $;
    if (currentOffset != pageLutOffset) {
        std::warning(std::format("Page LUT offset mismatch: expected 0x{:X}, got 0x{:X}", pageLutOffset, currentOffset));
    }

    u32 pageLut[pageCount] [[comment("Page data offsets")]];

    if (anchorMapOffset != 0) {
        AnchorMap anchorMap @ anchorMapOffset;
    }

    if (paragraphLutOffset != 0) {
        ParagraphLut paragraphLut @ paragraphLutOffset;
    }

    if (listItemLutOffset != 0 && paragraphLutOffset != 0) {
        u16 listItemIndex[paragraphLut.count] @ listItemLutOffset;
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

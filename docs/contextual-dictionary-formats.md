# Contextual Dictionary Binary Formats

## Contract

This document freezes version 1 of the canonical lexicon, definition-source and
attachment formats, plus version 4 of EPUB `language.bin`. All integers are
unsigned little-endian. All unused bytes and flag bits must be zero. CRC-32 is
the IEEE/zlib CRC over the stated byte range with initial and final XOR as
implemented by `zlib.crc32` and `lib/Dictionary/Crc32.*`.

Readers validate fixed headers, counts, exact file sizes, ordered ranges and
CRCs before exposing records. A UUID is valid only if at least one byte is
nonzero. Fixed language fields are zero-padded ASCII BCP-47 subsets of 1–7
bytes. Text pools and entry payloads are NFC UTF-8.

These are new contracts. Firmware does not interpret existing dictionary
package version 1 files as canonical or definition-source packages.

## Canonical runtime package version 1

A `.cplex` distribution uploads these files to
`/.crosspoint/lexicons/<canonical-uuid>/`:

```text
meta.bin
lexemes.bin
headwords.bin
licenses.txt
```

Installation uses hidden staging/backup directories and publishes only after
all files and CRCs validate. Compiler-only DWDSmor and ZDL assets are never
uploaded.

### `meta.bin`

`meta.bin` is exactly 112 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `CXCL` |
| 4 | 2 | Format version (`1`) |
| 6 | 2 | Header size (`112`) |
| 8 | 4 | Flags; zero |
| 12 | 16 | Canonical lexicon UUID |
| 28 | 8 | Source language |
| 36 | 4 | Lexeme count |
| 40 | 2 | Lexeme record size (`16`) |
| 42 | 2 | Canonical POS policy version (`1`) |
| 44 | 4 | Exact `lexemes.bin` size |
| 48 | 4 | Exact `headwords.bin` size |
| 52 | 4 | `lexemes.bin` CRC-32 |
| 56 | 4 | `headwords.bin` CRC-32 |
| 60 | 32 | SHA-256 of `lexemes.bin || headwords.bin` |
| 92 | 16 | Reserved; zero |
| 108 | 4 | Header CRC-32 over bytes `[0,108)` |

Caps are 500,000 lexemes, 8,000,000 bytes of lexeme records, 64 MiB of
headwords, and 96 UTF-8 bytes per headword. `lexemes.bin` must be exactly
`lexemeCount * 16` bytes.

### Lexeme record

A canonical ID is the zero-based record index:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Headword offset |
| 4 | 8 | FNV-1a-64 canonical key hash |
| 12 | 2 | Headword byte length |
| 14 | 1 | Canonical POS value |
| 15 | 1 | Flags |

Headword ranges must be inside `headwords.bin`. The key hash covers exact NFC
headword bytes, byte `0x1f`, and the POS byte. It accelerates migration but does
not replace exact headword/POS comparison. POS values and policy version 1 are
defined in [`contextual-analysis-policy.md`](contextual-analysis-policy.md).
Flag bit 0 marks a compound and bit 1 a generated lexical entry.

Records are sorted by `(NFC headword UTF-8, POS)` before IDs are assigned. The
canonical UUID is UUIDv5 in namespace
`1b924302-36fe-5d3d-aafb-b3dc11267ef7`, named by the lowercase hex SHA-256
payload fingerprint. The namespace is UUIDv5-URL of
`https://github.com/deurzen/CrossInk/formats/canonical-lexicon/v1`. Identical lexical payloads therefore retain
identity; renumbering or changing a headword/POS changes it.

## Definition-source package version 1

A `.cpdef` distribution uploads these files to
`/.crosspoint/definition-sources/<source-uuid>/`:

```text
meta.bin
entry-index.bin
entries.bin
licenses.txt
```

### `meta.bin`

The header is exactly 144 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `CXDS` |
| 4 | 2 | Format version (`1`) |
| 6 | 2 | Header size (`144`) |
| 8 | 4 | Flags; zero |
| 12 | 16 | Definition-source UUID |
| 28 | 16 | Required canonical UUID |
| 44 | 8 | Source language |
| 52 | 8 | Target language |
| 60 | 32 | Zero-padded UTF-8 source label, maximum 31 bytes |
| 92 | 4 | Canonical lexeme count |
| 96 | 2 | Index record size (`8`) |
| 98 | 2 | Entry format version (`1`) |
| 100 | 4 | Exact `entry-index.bin` size |
| 104 | 4 | Exact `entries.bin` size |
| 108 | 4 | Index CRC-32 |
| 112 | 4 | Entries CRC-32 |
| 116 | 4 | Number of nonempty index records |
| 120 | 20 | Reserved; zero |
| 140 | 4 | Header CRC-32 over bytes `[0,140)` |

The canonical count must equal the installed canonical package. The index is
exactly `canonicalLexemeCount * 8` bytes and is capped at 4,000,000 bytes.
`entries.bin` is capped at 1 GiB; an individual entry is capped at 1 MiB.

### Entry index and payload

Each canonical ID selects one fixed record:

```text
entryOffset:u32
entryLength:u32
```

Both zero means missing. No other zero-length representation is valid. Present
ranges must be wholly inside `entries.bin` and may not overlap. Present ranges
are ordered by canonical ID, allowing equal adjacent boundaries but no backward
seek. `coverageCount` equals the number of present records.

Entry payload encoding reuses native entry version 1:

```text
entryVersion:u8 = 1
flags:u8 = 0
fieldCount:u16
repeated fieldCount times:
  type:u8
  flags:u8 = 0
  byteLength:u16
  utf8Payload:u8[byteLength]
```

Field types remain definition (1), POS label (2), example (3), usage (4),
etymology (5), cross-reference (6), and compound component (7). A source UUID
is UUIDv5 in namespace
`e2372afd-1852-542c-876d-11477b36920b`, with the exact name
`<canonical UUID lowercase hex>:<payload SHA-256 lowercase hex>` where the
payload is `entry-index.bin || entries.bin`. The namespace is UUIDv5-URL of
`https://github.com/deurzen/CrossInk/formats/definition-source/v1`. Updating definition text
therefore creates a new source edition without changing learning identity.

## Attachment record version 1

`/.crosspoint/lexicons/<canonical-uuid>/attachments.bin` is exactly 88 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `CXAT` |
| 4 | 2 | Format version (`1`) |
| 6 | 2 | Header size (`88`) |
| 8 | 4 | Flags; zero |
| 12 | 16 | Canonical UUID |
| 28 | 4 | Monotonic generation |
| 32 | 1 | Attached source count (`0..3`) |
| 33 | 3 | Reserved; zero |
| 36 | 48 | Three ordered source UUID slots |
| 84 | 4 | CRC-32 over bytes `[0,84)` |

Used slots must be nonzero, distinct, installed, and target the header's
canonical UUID. Unused slots must be zero. Updates write and sync
`attachments.tmp`, then atomically replace `attachments.bin`; a valid previous
record remains authoritative after an interrupted write.

## EPUB `language.bin` version 4

Version 4 uses a 108-byte header, fixed directory layouts, and bounded shard
encoding. Its identity contract is:

- bytes `[16,32)` identify the canonical lexicon rather than a definition
  package;
- target language is the literal `und`; installed definition sources declare
  their own target languages;
- local lemma IDs resolve to canonical IDs;
- analyzer version `1` means DWDSmor-constrained contextual fusion;
- candidate flags record contextual/fallback provenance;
- metadata pins model and policy identities.

The header fields, offsets, and caps are documented in
[`file-formats.md`](file-formats.md). The format version is `4` and the UUID
field is `canonicalLexiconUuid`. Firmware accepts only this frozen contract;
older artifacts and unreleased drafts must be recompiled.

### Candidate flags and confidence

The 16-byte variable candidate header and following local ID/surface encoding
remain unchanged:

```text
surfaceHash:u64
recordSize:u16
surfaceLength:u8
analysisCount:u8       // 1..8
flags:u8
difficulty:u8          // 0 unknown, 1 easiest, 255 hardest
confidence:u16         // normalized primary-analysis score, 0..1000
localLemmaIds:u16[analysisCount]
surface:u8[surfaceLength]
zeroPadding:u8[]       // four-byte alignment
```

Flag bits are:

| Bit | Meaning |
| ---: | --- |
| 0 | More than one retained analysis |
| 1 | Primary ordering used contextual evidence |
| 2 | Case-folded morphology contributed |
| 3 | More than eight credible analyses were capped deterministically |
| 4 | Primary is low-confidence/fallback morphology |
| 5 | Contextual POS classified a proper noun |
| 6–7 | Reserved; zero |

Analyses are stored primary first, then descending policy score, then canonical
ID. C08 maps the policy's unbounded integer score to `0..1000`; confidence is
presentation/ranking evidence and never an ID. A valid DWDSmor candidate is not
dropped merely because confidence is low.

### Metadata envelope

The metadata section is the existing bounded envelope:

```text
magic[4] = "CXLM"
version:u16 = 1
headerSize:u16 = 16
jsonLength:u32         // <= 16384
flags:u32 = 0
json:utf8[jsonLength]
```

Canonical JSON uses sorted keys and no insignificant whitespace. Required keys
are canonical UUID, DWDSmor edition/version/SHA-256, ZDL model/version/SHA-256,
spaCy version, tokenizer version, analysis policy version, compiler version,
frequency provider/version, and shard token count. The current v4 compiler
records canonical POS layout version 1 and analysis policy version 2; these are
separate because C08b changed ranking without renumbering POS classes. Firmware may ignore JSON
content after bounds/UTF-8 validation; the fixed header UUID controls runtime
compatibility.

## Validation order and corruption fixtures

Readers fail in this order where applicable:

1. fixed header availability, magic, version and exact header size;
2. header CRC;
3. flags/reserved bytes, UUIDs, languages and labels;
4. counts, record sizes, multiplication overflow and hard caps;
5. exact file sizes, table/range bounds, then streamed payload CRC/SHA;
6. individual record invariants.

`test/data/contextual/formats/corruption-cases.json` defines deterministic
mutations of checked-in valid fixtures. C19 allocation-free readers must reject
every case with the named error category before seeking outside the validated
source. `scripts/dictionary/contextual/build_format_fixtures.py` regenerates the
fixtures and checksum manifest byte-for-byte.

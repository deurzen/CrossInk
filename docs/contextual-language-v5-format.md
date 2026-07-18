# Contextual EPUB Language Format Version 5

This document freezes U01. It is the future replacement for contextual
`META-INF/crossink/language.bin` version 4. Current firmware remains v4 until
U09 performs the single cutover; no reader accepts both versions.

All integers are unsigned little-endian. Absolute section offsets and variable
candidate records are aligned to four bytes. Every reserved field and padding
byte is zero.

## Identity and version contract

- Magic is `CXLG`.
- Format version is exactly `5`.
- Header size remains `108` bytes.
- Tokenizer version remains `1`.
- Analyzer version remains `1`; morphology/context ranking is unchanged.
- Bytes `[16,32)` identify the canonical `.cplex` lexicon.
- Source language is `de`; target language is `und` because attached definition
  sources own their own target languages.
- Grammar descriptor layout is exactly version 1 from
  [`contextual-grammar-descriptor-v1.md`](contextual-grammar-descriptor-v1.md).
- Compiler metadata version is `2`.

Canonical and definition package formats, UUIDs, attachment records and global
learning state are unchanged.

## Fixed header

| Offset | Size | Field | Required value or bound |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | `CXLG` |
| 4 | 2 | format version | `5` |
| 6 | 2 | header size | `108` |
| 8 | 4 | flags | zero |
| 12 | 2 | tokenizer version | `1` |
| 14 | 2 | analyzer version | `1` |
| 16 | 16 | canonical lexicon UUID | nonzero |
| 32 | 8 | zero-padded source language | `de` |
| 40 | 8 | zero-padded target language | `und` |
| 48 | 2 | spine count | `1..4096` |
| 50 | 2 | reserved | zero |
| 52 | 4 | shard count | `0..65535` |
| 56 | 4 | total candidate count | `0..1000000` |
| 60 | 4 | local lemma count | `0..32768` |
| 64 | 4 | reserved | zero |
| 68 | 4 | spine-directory offset | aligned absolute offset |
| 72 | 4 | shard-directory offset | aligned absolute offset |
| 76 | 4 | shard-blob offset | aligned absolute offset |
| 80 | 4 | local-lemma table offset | aligned absolute offset |
| 84 | 4 | metadata offset | aligned absolute offset |
| 88 | 8 | reserved offsets | zero |
| 96 | 4 | exact artifact size | `108..67108864` |
| 100 | 4 | payload CRC-32 | bytes `[108,fileSize)` |
| 104 | 4 | header CRC-32 | bytes `[0,104)` |

Sections occur in the table order shown by the offsets. A table may end before
the next section, but every intervening alignment byte is zero. Count × record
size and offset + size calculations use checked arithmetic before any seek.

## Spine directory

Each spine record is eight bytes:

```text
firstShard:u32
shardCount:u32
```

Spines are in OPF reading order. Their shard ranges are contiguous and ordered;
the first non-empty spine starts at shard zero and the final range ends at the
header shard count. A spine with no source tokens may have a zero shard count.

## Shard directory

Each shard record is 20 bytes:

```text
blobOffset:u32       // relative to shard-blob section
blobLength:u16       // <= 24576
recordCount:u16
sourceTokenStart:u32
sourceTokenEnd:u32   // exclusive
reserved:u32         // zero
```

Shard blobs are contiguous in shard order and do not overlap. Source-token
ranges are ordered, non-overlapping and use the compiler's cumulative token
index. `recordCount` cannot exceed `blobLength / 20`; the sum of every shard's
record count equals the header candidate count.

## Candidate record

Each candidate has a 20-byte fixed header followed by local IDs, exact surface
bytes and zero alignment padding:

```text
+0  surfaceHash:u64       // FNV-1a-64 over exact NFC UTF-8 surface
+8  recordSize:u16        // complete aligned record
+10 surfaceLength:u8      // 1..255
+11 analysisCount:u8      // 1..8
+12 flags:u8
+13 difficulty:u8         // 0 unavailable; 1 easiest; 255 hardest
+14 confidence:u16        // 0..1000
+16 grammarDescriptor:u32 // primary analysis; layout 1; zero unavailable
+20 localLemmaIds:u16[analysisCount]
    surface:u8[surfaceLength]
    padding:u8[0..3]      // zero
```

`recordSize` is exactly:

```text
align4(20 + analysisCount * 2 + surfaceLength)
```

Records fit wholly inside one shard blob and are sorted strictly by
`(surfaceHash, surface UTF-8 bytes)`. The surface hash must round-trip. Local IDs
are distinct within a record and each is below the header local-lemma count.
Analysis zero is the primary canonical identity; remaining IDs preserve
compiler score/ID order.

Candidate flags remain unchanged:

| Bit | Meaning |
| ---: | --- |
| 0 | more than one retained analysis |
| 1 | primary ordering used contextual evidence |
| 2 | case-folded morphology contributed |
| 3 | credible analyses exceeded eight and were capped |
| 4 | primary is fallback/low-confidence morphology |
| 5 | contextual POS classified a proper noun |
| 6-7 | reserved; zero |

The grammar descriptor is validated structurally before local IDs or surface
bytes are consumed. Descriptor absence is valid. A nonzero malformed descriptor
invalidates only the candidate read at runtime; full compiler/fixture validation
rejects the artifact.

A shard's final decoded record must end exactly at `blobOffset + blobLength`.
Trailing bytes, duplicate record sort keys, nonzero padding, unknown flags,
invalid confidence or impossible record sizes are corruption.

## Local lemma table

The table is a dense array indexed by local ID:

```text
canonicalLexemeId:u32[localLemmaCount]
```

Values are strictly increasing canonical IDs. Every candidate local ID resolves
through this table. At runtime a resolved canonical ID must be below the
installed canonical lexeme count before state or definition access.

The grammar descriptor never affects this table or learning identity.

## Metadata envelope

The final section retains the bounded `CXLM` envelope:

```text
magic[4] = "CXLM"
version:u16 = 1
headerSize:u16 = 16
jsonLength:u32         // <= 16384
flags:u32 = 0
json:utf8[jsonLength]
```

Canonical JSON is UTF-8, sorted by key and has no insignificant whitespace. In
addition to the existing canonical UUID, model/package hashes, policy versions,
frequency provider and shard-token count, v5 requires:

```json
{
  "compilerVersion": 2,
  "grammarDescriptorVersion": 1,
  "tokenizerVersion": 1
}
```

The fixed header, not JSON, controls device acceptance. Metadata records host
provenance and reproducibility.

## Validation order

### Fixed artifact open

Firmware rejects in this order:

1. 108 header bytes available;
2. magic, exact version 5 and exact header size;
3. header CRC;
4. flags and every reserved header field;
5. canonical UUID, exact languages and analyzer/tokenizer versions;
6. count and file-size caps;
7. exact physical file size;
8. offset alignment and order;
9. checked fixed-table ranges;
10. streamed payload CRC.

Version 4 therefore fails as `unsupported version` before payload validation or
canonical/source access. There is no v4 fallback.

### Shard and candidate read

After the artifact header and payload are trusted, a bounded shard/candidate
read rejects in this order:

1. requested shard/candidate index bounds;
2. fixed directory/header bytes available;
3. shard reserved/range/size/count invariants;
4. candidate record size, alignment, count, flags and confidence;
5. grammar descriptor reserved codes and structural combinations;
6. local-ID bytes and local-lemma bounds/distinctness;
7. surface bytes and FNV hash;
8. zero padding;
9. exact final-record end and record ordering where sequential context exists.

No validation step allocates `blobLength` or seeks outside a previously
validated section.

## Receipts, invalid markers and replacement

The existing 44-byte `language.valid` receipt remains unchanged. Its embedded
size, ZIP central-directory identity, payload CRC, header CRC and canonical UUID
must match the v5 member and extracted artifact. A v4 receipt/header pair is
stale after cutover and is revalidated as unsupported.

The existing `CXLI + fileSize:u32` invalid marker remains unchanged. Replacing
an EPUB changes ZIP identity/size, removes stale language cache files and retries
v5 extraction. Clearing the EPUB cache also removes receipts and invalid
markers; global canonical learning state remains outside that cache.

Compilation writes a sibling temporary EPUB and atomically replaces output only
after the v5 artifact and ZIP close successfully. The two production EPUBs are
recompiled separately in U17 with adjacent pre-v5 backups.

## Size and memory effects

The candidate header grows from 16 to 20 bytes. Since both sizes are four-byte
aligned, every candidate record grows by exactly four bytes without changing
surface or local-ID alignment. The exact candidate-record deltas and U08 measured total artifact deltas are:

| EPUB | Candidates | Record increase | Total artifact increase |
| --- | ---: | ---: | ---: |
| Homma novel | 126,718 | 506,872 bytes | 507,077 bytes |
| Klein collection | 57,242 | 228,968 bytes | 229,171 bytes |

The additional 205/203 bytes are bounded metadata and alignment growth, not
candidate payload. Candidate records still grow by exactly four bytes each.

Firmware adds one `u32` to the reusable decoded candidate and one `u32` to each
of at most 48 shortlist items. It adds no static DRAM, page-turn I/O, definition
page or SD handle. U10 must keep `Shortlist` within its existing 4 KiB static
assert; U16 records actual linked size and flash deltas.

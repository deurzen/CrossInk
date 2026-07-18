# Definition-Source Compiler

C14 adds deterministic `.cpdef` compilation against one C11 canonical UUID.
Definition packages supply text only; missing or conflicting content cannot
create morphology candidates or learning IDs.

## Generic JSON build

```sh
scripts/build_definition_source.py source.json output.cpdef \
  --canonical tmp.local/german-canonical.cplex
```

The schema uses canonical `headword` and `partOfSpeech` keys with fields named
`definition`, `part-of-speech`, `example`, `usage`, `etymology`,
`cross-reference`, or `compound-component`. Input order is irrelevant. Duplicate
canonical entries and exact duplicate fields merge deterministically.

The compiler emits:

```text
manifest.json
device/meta.bin
device/entry-index.bin
device/entries.bin
device/licenses.txt
compiler/coverage.json
```

`entry-index.bin` always has one eight-byte record per canonical lexeme. Missing
content is exactly `(offset=0, length=0)`; present entries are written in
canonical-ID order and never overlap. The source UUID is UUIDv5 of the canonical
UUID and SHA-256 of `entry-index.bin || entries.bin`, so changing definitions
creates a new source edition without changing learning identity.

## Validation and limits

Compilation and focused validation enforce:

- canonical UUID and lexeme-count agreement;
- metadata, index, and entry CRCs plus the fingerprint-derived source UUID;
- 31-byte NFC source labels and bounded ASCII language fields;
- at most 1,024 fields per entry, 65,535 bytes per field, 1 MiB per entry, and
  1 GiB total entry data;
- valid NFC UTF-8 field text and field types;
- ordered non-overlapping ranges and exact fixed-index size;
- deterministic corruption rejection for header CRC, UUID, index, and entry
  mutations.

Unmatched canonical keys are omitted and counted in `compiler/coverage.json`,
with at most 100 examples. This compiler report stays on the host and is not
uploaded.

A synthetic production-canonical run produced source UUID
`ded4d08d-0ac9-57e7-abd4-bb5a2063d468`; two builds were byte-identical with
archive SHA-256 `a0ea99e13e03d7f4c36de7345aa39d3bef30e616a9007603f28eef272de51214`.
C15 replaces this synthetic source with the current de-DE Wiktionary content and
records real coverage.

No new firmware allocation is introduced. C19/C20 will validate and install only
the four `device/` files through the single-reader transactional path; hardware
verification is deferred until those readers exist.

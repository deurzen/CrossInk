# Contextual Multi-Source Dictionary Task Board

This board tracks the implementation of
[`contextual-dictionary-architecture.md`](contextual-dictionary-architecture.md).
It is a second-generation pipeline layered on the existing compiled-book
runtime. The original [`dictionary-tasks.md`](dictionary-tasks.md) remains the
historical board for the current exact-form implementation.

A unit moves to **Done** only when its focused tests pass and its commit remains
independently buildable. Generated private dictionary/model artifacts are not
committed.

## Working rules

- Accuracy work runs on the desktop; ESP32 runtime remains bounded and generic.
- Do not add a compatibility branch for unreleased experimental binary formats.
- Pin and hash every morphology/model asset used for deterministic builds.
- Keep source text, models, compiler resources and device runtime files in
  separate artifacts with explicit licenses/provenance.
- Definition sources never own learning IDs.
- Retain plausible alternatives when contextual confidence is insufficient.
- Never hold multiple SD readers, definition pages or complete source indexes.
- Preserve the current working reader until a migration task explicitly swaps
  the production path.
- Record heap mechanism and hardware verification for every firmware phase.

## Phase A — contracts and reproducible assets

| ID | Status | Unit of work | Completion gate |
| --- | --- | --- | --- |
| C00 | Done | Specify the contextual canonical-lexicon and multi-source architecture | Architecture documents artifacts, provider boundaries, RAM budget, failure behavior and acceptance criteria |
| C01 | Done | Retrieve and fingerprint the DWDSmor 0.18.0 Open Edition baseline | Provenance/license recorded; wheel and automaton SHA-256 reproducible; representative analyses pass |
| C02 | Done | Pin the ZDL static German spaCy model and compiler environment | Locked Python/model versions and hashes; clean environment reproduces reference inference |
| C03 | Done | Define canonical POS/features mapping and scoring policy | Versioned DWDSmor/ZDL→canonical mapping; unsupported tags and low-confidence behavior documented |
| C04 | Done | Specify canonical, definition-source, attachment and next `language.bin` formats | Endianness, CRCs, caps, UUID/fingerprint rules and corruption fixtures documented before readers are written |

## Phase B — host linguistic pipeline

| ID | Status | Unit of work | Completion gate |
| --- | --- | --- | --- |
| C05 | Done | Add a generic host `LanguageAnalyzer` interface and German provider shell | Generic orchestration has no German constants; synthetic provider tests pass |
| C06 | Done | Add bounded DWDSmor sentence/token analysis adapter | All analyzer output is parsed with explicit caps; malformed/oversized output fails cleanly |
| C07 | Done | Add ZDL contextual POS/lemma adapter | Sentence offsets round-trip to XHTML tokens; model errors and token mismatches are reported |
| C08 | Done | Fuse contextual and morphological analyses | Primary plus ≤7 alternatives emitted deterministically; confidence policy has focused unit tests |
| C08a | Done | Augment missing Open Edition analyses from the de-DE form inventory | Missing common forms gain canonical candidates; source precedence/provenance and ambiguity caps are deterministic |
| C08b | Done | Recombine finite separable verbs from sentence context | Particle + finite base can resolve lexical separable lemma without changing unrelated token offsets |
| C09 | Done | Build contextual German ambiguity corpus | Gold cases cover sentence-initial verbs, noun/verb homographs, participles, separable verbs, nominalization, compounds and names; policy 2 measures 16/16 coverage and 14/16 primary accuracy |
| C10 | Done | Benchmark static versus transformer ZDL models | Accuracy, wall time and peak host RAM measured on the same corpus; production selector chosen from evidence |
| C11 | Done | Build deterministic canonical lexicon and compiler bundle | Dense IDs stable across input order; canonical UUID/fingerprint and model provenance emitted |
| C12 | Done | Add full-EPUB host compiler using the contextual provider | Canonical OPF preserved; XHTML markers and `language.bin` transactional; two builds are byte-identical |
| C13 | Done | Retire exact-case-first misranking in the legacy browser compiler | Exact and folded credible analyses are merged; regression tests prevent noun-only sentence-start errors |

## Phase C — definition-source compilation

| ID | Status | Unit of work | Completion gate |
| --- | --- | --- | --- |
| C14 | Done | Implement deterministic `.cpdef` compiler and fixed canonical entry index | Missing entries use zero-length records; caps/CRC/UUID corruption tests pass |
| C15 | Done | Compile current de-DE Wiktionary definitions against canonical IDs | Coverage report emitted; definitions/examples preserve provenance; no morphology ownership |
| C16 | Done | Import and compile private dict.cc de→en source | 1.3M-line TSV streams without whole-file allocation; annotations/POS normalized; duplicate translations capped and reported |
| C17 | Done | Import and compile Kaikki German de→en source | 1 GB JSONL streams line-by-line; senses/qualifiers/POS mapped; malformed records counted, not fatal |
| C18 | Done | Produce source coverage and conflict report | Per-source/union canonical coverage, unmatched keys, POS conflicts and pathological entry sizes documented |

## Phase D — device formats and installation

| ID | Status | Unit of work | Completion gate |
| --- | --- | --- | --- |
| C19 | Done | Implement allocation-free canonical and definition-source header/index readers | Host fixtures reject truncation, overflow, bad CRC, UUID and count mismatches before seeking |
| C20 | Done | Extend transactional installer for `.cplex` and `.cpdef` runtime files | Interrupted install/replace/remove recovers previous package; one-reader hardware rule preserved |
| C21 | Done | Add atomic attachment record for at most three sources | Order persists; duplicate/mismatched/missing UUIDs rejected or skipped; interrupted write retains old record |
| C22 | Done | Extend inventory APIs with canonical/source compatibility | Bounded JSON output reports labels, direction, coverage and attachment order |
| C23 | Done | Add WebUI installation and source-order controls | Compiler models stay on desktop; only runtime files upload; source reorder requires no EPUB recompile |

## Phase E — firmware lookup and UI

| ID | Status | Unit of work | Completion gate |
| --- | --- | --- | --- |
| C24 | Done | Switch learning state and book artifacts to canonical UUID identity | Existing explicit states remain isolated until migration; mismatch behavior tested |
| C25 | Planned | Discover and retain at most three bounded definition descriptors | Measured session growth ≤3.5 KB; no source index or entry loaded wholesale |
| C26 | Planned | Read canonical entry-index records through the switching SD reader | One 8-byte index read per source/lemma; no simultaneous file handles; I/O metrics covered |
| C27 | Planned | Stream source × analysis definitions through the existing pager/page | No second page allocation; missing source entries skipped; backward/forward replay bounded |
| C28 | Planned | Render labeled source dividers and existing analysis/meaning separators | de-DE, dict.cc and Kaikki visibly distinct; pagination accounts for divider height without clipping |
| C29 | Planned | Apply status once to the canonical lexical item | Known/Learning/Ignore suppresses the item independent of source availability or order |
| C30 | Planned | Add translated failure and compatibility UI | Missing canonical/source/corrupt-entry states are actionable and never crash or silently mislabel content |

## Phase F — migration and validation

| ID | Status | Unit of work | Completion gate |
| --- | --- | --- | --- |
| C31 | Planned | Build old de-DE ID → canonical ID migration map | Mapping generated by exact normalized lemma/POS; collisions/unmatched IDs reported |
| C32 | Planned | Implement atomic WAL-safe learning-state migration | All mapped nonzero states preserved; interruption is restartable; old state retained until commit |
| C33 | Planned | Add Python/worker/compiler differential fixtures where formats overlap | Token offsets, shard markers, IDs and binary records agree for shared deterministic stages |
| C34 | Planned | Run complete host, simulator, firmware and static-analysis suite | All tests/builds pass; generated files clean; static DRAM and flash deltas recorded |
| C35 | Planned | Validate production novel and all three sources on X3/X4 | Cold/warm lookup, opens/seeks/reads/bytes, heap/largest block and stack high-water recorded |
| C36 | Planned | Run 100-lookup/status/source-reorder endurance test | No heap decline, stale handle, state loss, cache dependency or attachment corruption |
| C37 | Planned | Promote contextual pipeline and document legacy retirement | User workflow and asset setup documented; old EPUBs intentionally recompiled; changelog complete |

## Explicit decision gates

### Gate 1 — Open Edition coverage

DWDSmor 0.18.0 Open Edition is the production morphology baseline. Its grammar
is suitable, but its sample lexicon has lower open-class coverage than the
unavailable DWDS Edition. C09/C10 must report missing-analysis coverage
separately from contextual ranking accuracy. If coverage is insufficient,
evaluate a named definition-lexicon or Zmorge fallback rather than silently
classifying missing tokens from spelling.

### Gate 2 — contextual model

C10 selects `de-zdl-lg` 4.0.0 static: the transformer produced identical corpus
accuracy while using about 967 MB more peak host RSS and running slower on CPU.
The transformer remains a benchmark-only overlay. Device RAM is unaffected.

### Gate 3 — production cutover

Do not replace the current exact-form path until C31–C36 pass. The new canonical
UUID changes book and learning identity; cutover requires an explicit migration,
new compiled EPUB and recoverable source installation.

## Target budgets

| Resource or operation | Gate |
| --- | ---: |
| Additional firmware lookup-session RAM | ≤3.5 KB |
| Definition page buffers | Exactly one existing page |
| Concurrent SD readers | Exactly one |
| Attached definition sources | ≤3 |
| Alternatives per token | ≤8 |
| Device morphology/model bytes | 0 |
| Ordinary page-turn dictionary I/O | 0 |
| State writes during ordinary page turns | 0 |

## C19 implementation note

`lib/Dictionary/ContextualRuntimeFormat.*` parses canonical and definition-source
metadata through non-owning `RandomAccessSource` callbacks. Opening uses fixed
112-byte or 144-byte stack headers; runtime lookups use 16-byte and 8-byte
record scratch respectively. Full CRC and ordered-index validation reuse a
caller-owned bounded buffer, so the reader classes allocate no heap and retain
no `HalFile`. The C20 storage adapter remains responsible for opening only one
SD file at a time.

## C20 implementation note

`DictionaryInstaller` now applies the existing hidden staging, backup and
removal transaction to canonical and definition-source roots. Commit validates
required licenses, exact sizes, identity compatibility, payload CRCs, canonical
records and ordered source indexes before the final directory rename. It reuses
the caller's validation scratch and the storage backend opens each file
operation locally, so validation adds no heap allocation and cannot retain a
second SD reader. Web upload wiring remains C23 work.

On hardware after C23, interrupt canonical/source uploads before commit, during
same-UUID replacement and immediately after removal. Reboot, retry the
operation, and verify that only the old or new complete package is visible,
with no `.backup-*` restoration after removal and no `DIN` open failures in the
serial log.

## C21 implementation note

`ContextualAttachments.*` parses and emits the fixed 88-byte `CXAT` record and
updates it with optimistic generation checks. The store syncs
`attachments.tmp`, hides the previous record as `attachments.bak`, and commits
with a final rename; recovery prefers a valid committed final record and
otherwise restores the valid backup. A bounded compatibility callback rejects
uninstalled or wrong-canonical sources before writing. The HAL adapter uses one
local `HalFile` at a time and explicitly closes it before rename/remove calls.

On hardware after C23, reorder three installed sources, reboot and confirm the
order and generation persist. Then cut power once after temp sync and once
during replacement; reboot must expose either the complete previous order or
the complete new order, never duplicate UUIDs or a partial record.

## C22 implementation note

`GET /api/dictionaries/contextual` now streams separate canonical and definition
source inventories. It reports validity, UUID/count compatibility, labels,
language direction, coverage, runtime bytes, attachment generation and source
order. Enumeration is capped at 64 packages per class. The cold endpoint uses
one fallible 3,840-byte work allocation and at most 512-byte JSON chunks; it
never reads source indexes or entries. Roughly 1 KB of fixed installer/path
state lives only inside the network server allocation and is released on exit,
so reader-session RAM is unchanged.

On hardware, open the Dictionaries page with valid, mismatched and corrupted
metadata present, request `/api/dictionaries/contextual`, and verify valid JSON,
correct compatibility/order, no `DIN` handle failures and full heap recovery
after leaving network mode.

## C23 implementation note

The Dictionaries page now recognizes `.cplex` and `.cpdef` manifests, enforces
fixed runtime paths and size caps, verifies declared SHA-256 when Web Crypto is
available, and sends only runtime files through the existing resumable 256 KB
requests and reusable 2 KB firmware buffer. Firmware selects roots and allowed
filenames from an explicit package kind, validates an installed canonical
UUID/count before publishing a definition source, and never accepts a client SD
path. Attachment controls use optimistic generation updates and require all
ordered sources to be installed and compatible; reordering does not touch EPUBs.

On X3/X4, install the production canonical package and all three sources over a
throttled or interrupted connection, confirm resume offsets and coverage, then
attach/reorder/detach sources and reboot. Expected behavior is stable ordering,
no uploaded `compiler/` files, no simultaneous `DIN` open failures, and full
network-mode heap recovery after returning to the reader.

## C24 implementation note

The frozen version-4 `language.bin` header now treats bytes `[16,32)` as an
explicit canonical identity, enforces analyzer version 1 and target `und`, and
accepts the two contextual provenance flag bits without weakening version 3.
`Epub` exposes the artifact identity rather than a bundle-specific value.
Lookup selects `/.crosspoint/lexicons/<uuid>/` for v4, validates canonical
metadata and lexeme bounds, and keys the existing WAL state store by that
canonical UUID. Version 3 remains on the legacy package root and bundle-keyed
state namespace, so there is no implicit migration or status leakage.

This adds no heap allocation. The lookup session retains one allocation-free
canonical reader and reuses its existing switching `HalFile`; metadata,
lexeme-table and headword-table sizes are discovered sequentially. On hardware,
open a v4-compiled EPUB with the matching canonical package, change one status,
reboot and confirm it persists across another v4 book using that canonical UUID.
Then open a v3 book with explicit state and confirm neither status appears in
the other namespace. A missing or mismatched canonical UUID must fail lookup
without opening a same-named legacy package.

## Immediate next work

1. Discover and retain at most three bounded definition descriptors in C25.
2. Preserve the two known C09 ranking failures in broader novel evaluation before cutover.

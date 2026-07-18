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
| C06 | Planned | Add bounded DWDSmor sentence/token analysis adapter | All analyzer output is parsed with explicit caps; malformed/oversized output fails cleanly |
| C07 | Planned | Add ZDL contextual POS/lemma adapter | Sentence offsets round-trip to XHTML tokens; model errors and token mismatches are reported |
| C08 | Planned | Fuse contextual and morphological analyses | Primary plus ≤7 alternatives emitted deterministically; confidence policy has focused unit tests |
| C09 | Planned | Build contextual German ambiguity corpus | Gold cases cover sentence-initial verbs, noun/verb homographs, participles, separable verbs, nominalization, compounds and names |
| C10 | Planned | Benchmark static versus transformer ZDL models | Accuracy, wall time and peak host RAM measured on the same corpus; production selector chosen from evidence |
| C11 | Planned | Build deterministic canonical lexicon and compiler bundle | Dense IDs stable across input order; canonical UUID/fingerprint and model provenance emitted |
| C12 | Planned | Add full-EPUB host compiler using the contextual provider | Canonical OPF preserved; XHTML markers and `language.bin` transactional; two builds are byte-identical |
| C13 | Done | Retire exact-case-first misranking in the legacy browser compiler | Exact and folded credible analyses are merged; regression tests prevent noun-only sentence-start errors |

## Phase C — definition-source compilation

| ID | Status | Unit of work | Completion gate |
| --- | --- | --- | --- |
| C14 | Planned | Implement deterministic `.cpdef` compiler and fixed canonical entry index | Missing entries use zero-length records; caps/CRC/UUID corruption tests pass |
| C15 | Planned | Compile current de-DE Wiktionary definitions against canonical IDs | Coverage report emitted; definitions/examples preserve provenance; no morphology ownership |
| C16 | Planned | Import and compile private dict.cc de→en source | 1.3M-line TSV streams without whole-file allocation; annotations/POS normalized; duplicate translations capped and reported |
| C17 | Planned | Import and compile Kaikki German de→en source | 1 GB JSONL streams line-by-line; senses/qualifiers/POS mapped; malformed records counted, not fatal |
| C18 | Planned | Produce source coverage and conflict report | Per-source/union canonical coverage, unmatched keys, POS conflicts and pathological entry sizes documented |

## Phase D — device formats and installation

| ID | Status | Unit of work | Completion gate |
| --- | --- | --- | --- |
| C19 | Planned | Implement allocation-free canonical and definition-source header/index readers | Host fixtures reject truncation, overflow, bad CRC, UUID and count mismatches before seeking |
| C20 | Planned | Extend transactional installer for `.cplex` and `.cpdef` runtime files | Interrupted install/replace/remove recovers previous package; one-reader hardware rule preserved |
| C21 | Planned | Add atomic attachment record for at most three sources | Order persists; duplicate/mismatched/missing UUIDs rejected or skipped; interrupted write retains old record |
| C22 | Planned | Extend inventory APIs with canonical/source compatibility | Bounded JSON output reports labels, direction, coverage and attachment order |
| C23 | Planned | Add WebUI installation and source-order controls | Compiler models stay on desktop; only runtime files upload; source reorder requires no EPUB recompile |

## Phase E — firmware lookup and UI

| ID | Status | Unit of work | Completion gate |
| --- | --- | --- | --- |
| C24 | Planned | Switch learning state and book artifacts to canonical UUID identity | Existing explicit states remain isolated until migration; mismatch behavior tested |
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

Choose static or transformer ZDL only after C10. The transformer must show a
meaningful ambiguity-resolution improvement to justify its larger host download,
RAM and compile time. Device RAM is unaffected either way.

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

## Immediate next work

1. Add the generic C05 host analyzer contract and German provider shell.
2. Implement bounded DWDSmor and ZDL adapters in C06/C07.
3. Build C08/C09 together so fusion decisions are driven by real ambiguous
   sentences rather than isolated token examples.

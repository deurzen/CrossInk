# Contextual Dictionary UX v5 Task Board

This board tracks the next contextual-dictionary iteration after the v4
multi-source pipeline. It covers three reader-facing improvements:

1. label every retained canonical analysis;
2. explain the primary contextual grammatical form;
3. navigate directly between shortlist words while a definition is open.

Performance work follows these features and remains gated by physical C35/C36
measurements. This board does not authorize speculative cache or buffer growth.

## Feasibility evidence

The button change is feasible without HAL work. Definition mode currently
aliases logical Up/Left to the previous definition page and Down/Right to the
next page in `src/activities/reader/DictionaryActivity.cpp:441-509`.
`src/MappedInputManager.cpp:138-147` already distinguishes logical front
Left/Right from side Up/Down while preserving orientation mapping. The activity
can therefore separate the actions without raw button indices or a new setting.

The host pipeline already retains normalized grammatical features in
`scripts/dictionary/contextual/analysis_policy.py:164-176`; they are discarded
when candidate records are packed at
`scripts/dictionary/contextual/epub_compiler.py:384-397`. The anonymous
alternative boundary is rendered at
`src/activities/reader/DictionaryActivity.cpp:544-568`, and canonical records
already expose headword/POS at `lib/Dictionary/ContextualRuntimeFormat.h:37-41`.
The required data and UI seam exist; the main work is
freezing a truthful bounded v5 representation and preserving it through the
shortlist.

The firmware budget is enforceable rather than aspirational:
`lib/Dictionary/BookLanguageReader.h:25` contains the one reusable decoded
candidate, while `lib/Dictionary/PageShortlist.h:13-20,88` caps the shortlist at
48 items and 4 KiB. The v5 design adds one descriptor to each of those existing
fixed structures, not a new container.

## Fixed product behavior

### Definition presentation

The selected word keeps the existing `surface · primary lemma` title. A second,
smaller translated line explains the primary contextual analysis, for example:

```text
knipste · knipsen
Verb · Präteritum · Indikativ · 3. Person Singular
```

When definitions continue into another retained canonical analysis, its divider
is labeled with canonical lemma and part of speech instead of showing an
anonymous rule:

```text
────────  Laden · Substantiv  ────────
```

Only analyzer-provided, validated values may be shown. If repeated occurrences
of the same surface in one shard disagree about the primary form, the compiler
emits no grammatical explanation rather than presenting a potentially false
one. Definition sources remain presentation content only and never supply
morphology or identity.

### Definition-mode buttons

The existing logical `MappedInputManager::Button` values remain authoritative;
no raw hardware indices enter the activity.

| Mode | Back | Confirm | Front Left/Right | Side Up/Down |
| --- | --- | --- | --- | --- |
| Shortlist | Close | Open word | Previous/next word | Previous/next shortlist page |
| Definition | Return to shortlist | Set status | Previous/next definition page | Previous/next word |
| Status | Cancel | Save | Previous/next status | Previous/next status |

Previous/next word follows the sorted shortlist and wraps at either end,
matching existing one-word shortlist navigation. Changing words resets the
definition cursor to page zero, source warning, status feedback, canonical title
and grammatical line, while reusing the existing pager and single definition
page. It must also work when the current word has no definition or a partial
source failure.

After a status is saved, the first side-button word change reapplies canonical
status filtering. Moving down selects the item that shifted into the removed
item's index; moving up selects its predecessor. If no unknown items remain,
the dictionary closes cleanly. Bottom button hints continue to describe the
four front buttons; a bounded `current/total` indicator in the definition header
makes side-button word movement visible.

## Format decision

This work introduces contextual `language.bin` **version 5**. Firmware and host
tools accept only v5 after cutover; no v4 reader, migration or fallback branch
is added. Existing contextual EPUBs must be recompiled. Canonical `.cplex`,
definition `.cpdef`, attachment and canonical learning-state formats remain
unchanged, so installed packages and learned statuses retain their identity.

Each v5 candidate adds exactly one little-endian `grammarDescriptor:u32` after
the existing confidence field. It describes only the primary contextual
analysis. Local canonical IDs remain ordered exactly as in v4.

The descriptor uses fixed, independently validated fields for case, degree,
gender, mood, number, person, tense and verb form. Zero means unavailable.
Reserved encodings and bits are rejected. The exact bit layout and allowed
cross-field combinations must be frozen in the format specification before
compiler or firmware implementation starts.

Adding the four-byte descriptor increases each aligned candidate record by four
bytes: exactly 506,872 record bytes for the 126,718-candidate Homma novel and
228,968 for the 57,242-candidate Klein collection. U08 measured total artifact
growth of 507,077 and 229,171 bytes after bounded metadata/alignment. Runtime
still adds four bytes to the allocation-free candidate and at most four bytes
per retained shortlist item. No definition payload or canonical package changes.

## Resource and lifecycle gates

| Resource | Gate |
| --- | ---: |
| Additional retained dictionary-activity/session heap | ≤768 bytes |
| Additional shortlist storage | ≤192 bytes (`4 * 48` items) |
| Analysis-label cache | ≤512 bytes, fixed capacity, no dynamic strings |
| Additional static DRAM | 0 bytes |
| Definition pages | Exactly one existing page |
| Concurrent SD readers | Exactly one |
| Ordinary page-turn dictionary I/O | 0 |
| Analysis-label reads | Explicit definition open/page load only |
| Grammar descriptor bytes per candidate | Exactly 4 |
| Retained analyses | Existing maximum of 8 |

Canonical labels are loaded lazily through the existing switching reader before
rendering a definition page, never from `render()`. A fixed activity-owned cache
may retain truncated, UTF-8-safe display headwords plus POS for at most eight
analyses. It is part of the heap-allocated activity object, not a new allocation,
and is cleared when changing words. Pagination reserves the existing analysis
divider slot before loading content so labels cannot clip the final line.

## Phase A — freeze v5 contracts

| ID | Status | Work item | Completion gate |
| --- | --- | --- | --- |
| U00 | Done | Freeze [grammatical descriptor semantics and merge policy](contextual-grammar-descriptor-v1.md) | Bit layout, allowed values, conflict-to-unavailable behavior and examples are documented |
| U01 | Done | Specify [contextual `language.bin` v5](contextual-language-v5-format.md) | Header/version, 20-byte candidate header, record alignment, CRC coverage and strict rejection order are frozen |
| U02 | Done | Define [translated grammar presentation policy](contextual-grammar-presentation.md) | POS/feature labels, ordering, omission rules, width fallback and UTF-8 truncation are specified |
| U03 | Done | Freeze [definition-mode input behavior](contextual-dictionary-input-contract.md) | All three modes, wrap/filter behavior, failure-state navigation and orientation-aware logical buttons have test cases |

U00 deliberately uses contextual ZDL features rather than form-only morphology.
It freezes a 17-bit payload inside a 32-bit word, strict structural validation,
POS compatibility, and conservative within-shard/page conflict handling. This
keeps grammatical display truthful when a surface has multiple occurrences or a
separable lemma is recombined.

U01 keeps the 108-byte artifact header and every identity contract stable while
moving to version 5 and a 20-byte candidate header. The four-byte grammar word
sits before local IDs, so every aligned candidate grows by exactly four bytes;
v4 is rejected before payload or package access after cutover.

U02 freezes POS-aware field order, full English/German terminology,
person+number translation units, fixed-buffer width fallback and 22-pixel
labeled alternative dividers. Formatting is performed before rendering with no
dynamic string or render-time SD I/O.

U03 separates logical front Left/Right definition paging from side Up/Down word
movement. It freezes event priority, sorted wrap behavior, page-zero resets,
failed-definition escape, post-status filtering destinations and zero-allocation
reuse of the existing pager/page/session.

## Phase B — host grammatical pipeline

| ID | Status | Work item | Completion gate |
| --- | --- | --- | --- |
| U04 | Done | Add deterministic grammar descriptor encoder | Every supported DWDSmor/ZDL canonical feature maps to a validated packed value; malformed combinations fail |
| U05 | Done | Preserve primary grammar through surface aggregation | Identical evidence survives; conflicting repeated-surface evidence emits unavailable deterministically |
| U06 | Done | Emit only v5 contextual records | Compiler writes the descriptor, bumps compiler metadata and rejects out-of-range/reserved values |
| U07 | Done | Add v5 differential and corruption fixtures | Token offsets, markers, IDs, grammar words, record bytes, CRCs and complete artifact SHA agree independently |
| U08 | Done | [Re-evaluate production corpora](contextual-grammar-production-evaluation.md) | German ambiguity baseline remains stable; v5 grammar accuracy is manually checked for verbs, nouns, adjectives and ambiguous forms |

U04 adds a strict host encoder/decoder with no dependency on spaCy/DWDSmor enum
ordinals. Checked-in vectors cover every field code and contradiction; an
exhaustive test round-trips all 2,113 valid 17-bit payloads and rejects every
reserved high bit. Contextual POS gating uses only ZDL token features and keeps
noun/proper-noun compatibility explicit.

U05 carries grammar only for the canonical ID that was primary at each surface
occurrence. Per-field unavailable values union conservatively, equal evidence
survives, and value or structural conflicts are sticky and finalize as zero.
Permutation tests prove aggregation order cannot recover conflicted evidence or
allow an alternative analysis to contaminate the selected primary.

U06 makes host output v5-only with a 20-byte fixed candidate header and compiler
metadata version 2. Every descriptor is validated again at the serializer
boundary; diagnostics count available occurrences, missing features, POS
mismatches and within-shard conflicts in metadata and CLI output. Existing
records grow by exactly four aligned bytes.

U07 upgrades the independent compiler fixture with noun, proper-noun, finite and
infinitive grammar words plus exact candidate hex and full-artifact SHA. The
regenerable format fixture is v5-only; CRC-correct corruption vectors cover
reserved descriptor bits/codes and every structural contradiction class so U09
can exercise candidate validation rather than fail earlier on payload CRC.

U08 preserves the 14/16 fused ambiguity baseline and both known ranking
failures. Dry-running both production books yields nonzero descriptors for
108,067/126,718 and 43,784/57,242 final candidates; manual verb, noun, adjective
and ambiguity checks pass representative cases while explicitly recording ZDL
and primary-ranking errors rather than adding guesses.

## Phase C — bounded firmware support

| ID | Status | Work item | Completion gate |
| --- | --- | --- | --- |
| U09 | Planned | Replace v4 parser with strict v5 parser | Host tests reject v4 and every malformed grammar bit/combination before out-of-range reads |
| U10 | Planned | Carry primary grammar into the shortlist | Fixed item growth stays within the 4 KiB shortlist and 192-byte incremental budget |
| U11 | Planned | Add bounded canonical analysis-label loading | Labels use the switching reader, lazy fixed cache and no render-time I/O or second handle |
| U12 | Planned | Render primary grammar and labeled alternatives | Primary line and every alternative boundary are translated, pagination-safe and orientation-safe |
| U13 | Planned | Add direct previous/next word navigation | Side buttons switch sorted words, reuse buffers, reset cursors, handle saved-status filtering and work from failures |
| U14 | Planned | Update button hints and position feedback | Front hints remain accurate; current/total feedback fits bezel-safe bounds in all orientations |

## Phase D — tests, cutover and qualification

| ID | Status | Work item | Completion gate |
| --- | --- | --- | --- |
| U15 | Planned | Add focused host and simulator interaction tests | Grammar omission, alternatives, pagination, all button modes, edge wrap and post-status removal are covered |
| U16 | Planned | Run complete automated qualification | Host, contextual Python, WebUI, simulator smoke, firmware, format, static analysis and generated-file checks pass |
| U17 | Planned | Recompile the two production German EPUBs | Adjacent byte-identical pre-v5 backups are preserved; outputs validate as v5 with the existing canonical UUID |
| U18 | Planned | Validate UX and resources on X3/X4 | `knipste · knipsen` grammar, alternative labels, direct word navigation, orientations, heap, largest block, stack and one-reader behavior pass |
| U19 | Planned | Run updated 100-cycle endurance gate | Word changes, definition paging, status removals, source reorder and reboot show no decline or stale state |

## Phase E — measurement-gated optimization

| ID | Status | Work item | Completion gate |
| --- | --- | --- | --- |
| U20 | Planned | Analyze C35/C36 v5 logs | Cold/warm stage timings and opens/seeks/reads/bytes identify a measured dominant cost |
| U21 | Planned | Select at most one optimization | Expected mechanism, RAM/flash/SD trade-off and rollback threshold are documented before code |
| U22 | Planned | Implement and A/B test the selected optimization | Same hardware/book/page shows a repeatable gain without heap, stack, handle or artifact regressions |
| U23 | Planned | Promote contextual v5 | User workflow, compiler docs, formats, troubleshooting, task boards and changelog reflect v5-only production |

Candidate optimizations are deliberately not preselected. If metadata discovery
dominates, evaluate a transactional compact catalog that reduces SD opens. If
entry streaming dominates, benchmark bounded 128/256/512-byte chunks. Retaining
a complete lookup session between activities, prefetching during page turns, or
adding a second SD handle is out of scope regardless of benchmark results.

## Required verification artifacts

- checked-in deterministic v5 and corruption fixtures;
- grammar encoder/decoder vectors for every allowed value;
- screenshots for primary grammar and at least two alternative analyses;
- simulator input traces for front-page and side-word navigation;
- firmware size/static RAM comparison against the v4 cutover commit;
- X3 and X4 C35/C36 raw logs plus parser JSON;
- SHA-256 and backup paths for both recompiled production EPUBs.

## Immediate order

1. Complete U00-U03 without implementation.
2. Implement host v5 U04-U08 before changing firmware acceptance.
3. Implement firmware/UI U09-U14 and focused tests U15.
4. Cut over, recompile and qualify through U19.
5. Consider performance work only from U20 measurements.

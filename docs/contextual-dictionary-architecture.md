# Contextual Multi-Source Dictionary Architecture

## Status and scope

This document specifies the target architecture for replacing CrossInk's current
Wiktionary-form lookup with an offline, context-sensitive lexical pipeline:

```text
DWDSmor morphology
+ contextual German POS/lemma selection
+ de-DE, dict.cc and Kaikki definition sources
```

The feature remains a compiled-book dictionary. No morphology model, global
headword search, network request, or definition database is loaded into ESP32
RAM while reading. The desktop performs linguistic work before upload; firmware
performs bounded ID lookup and sequential SD reads.

This architecture intentionally supersedes the earlier idea that the current
de-DE package must be the primary identity. Morphology, learning identity, and
definition content have different lifecycles and are separated. The concrete
binary contracts are frozen in
[`contextual-dictionary-formats.md`](contextual-dictionary-formats.md).

## Why the current pipeline misclassifies words

The current `forms.bin` maps a surface to all source lexeme IDs, sorts those IDs,
and assigns every analysis confidence 1000. The compiler now keeps exact-case
analyses first and conservatively appends distinct case-folded analyses, fixing
the earlier behavior where capitalization could hide a sentence-initial verb.
It still has no sentence context or POS model, so it cannot linguistically rank
the retained alternatives.

A larger dictionary alone cannot resolve this. German surface forms are
legitimately ambiguous; morphology enumerates analyses, while contextual POS
and lemma selection ranks them.

## Core invariants

1. The canonical lexicon owns stable dense IDs and learning state.
2. Definition sources never define morphology or learning identity.
3. A compiled EPUB references one canonical lexicon UUID and version.
4. Up to three attached definition sources are read sequentially, never held in
   memory together.
5. Contextual preprocessing may select a primary analysis but retains bounded
   credible alternatives.
6. Firmware accepts only fully validated, version-matched artifacts.
7. Ordinary page turns perform no dictionary I/O or linguistic work.
8. No operation retains more than one SD file handle on X3/X4.

## Artifact model

### 1. Canonical lexical bundle (`.cplex`)

The canonical bundle is language-analysis data, not a dictionary edition:

```text
manifest.json

runtime/
  meta.bin             canonical UUID, language, counts, fingerprints
  lexemes.bin          dense ID → headword/POS/flags
  headwords.bin        UTF-8 pool
  licenses.txt

compiler/
  dwdsmor/             pinned analyzer metadata or external-asset references
  contextual-model.json
  frequency.bin
  analyzer.json
  licenses/
```

The DWDSmor Open Edition automata and contextual model remain desktop assets.
They are not uploaded to the reader. Runtime lexical files contain only enough
data to review learning state and validate definition-source alignment.

Canonical IDs are assigned deterministically by normalized `(lemma, coarse
POS)` key. C11 seeds the initial identity set from the verified 181,609 de-DE
lexical records and embeds pinned DWDSmor/ZDL provenance without copying forms,
definitions, or models. The reproducible build and current UUID are documented
in [`canonical-lexicon-compiler.md`](canonical-lexicon-compiler.md). A change
that renumbers IDs creates a new canonical UUID and requires an explicit state
migration map.

### 2. Definition-source bundle (`.cpdef`)

Each source is independently installable and aligned to one canonical UUID:

```text
manifest.json

device/
  meta.bin             source UUID, canonical UUID, languages, source label
  entry-index.bin      canonical ID → offset/length, zero length means absent
  entries.bin          structured fields, uncompressed
  licenses.txt
```

`entry-index.bin` uses a fixed record per canonical lexeme. At 250,000 lexemes,
an eight-byte offset/length record costs about 2 MB on SD and zero resident RAM.
It avoids a second ID namespace and avoids a runtime alignment search.

The initial editions are:

| Source | Direction | Role |
| --- | --- | --- |
| Current German Wiktionary extraction | de→de | Native-language definitions and examples; C15 aligns all 181,609 canonical records ([report](de-de-definition-source.md)) |
| dict.cc private export | de→en | Concise, practical and colloquial translations; C16 aligns 105,036 canonical records ([private report](dictcc-definition-source.md)) |
| Kaikki English-Wiktionary German extraction | de→en | Structured senses, qualifiers and explanatory glosses; C17 aligns 62,710 canonical records ([report](kaikki-definition-source.md)) |

Source compilers normalize headword/POS keys offline, preserve provenance per
field where useful, deduplicate exact text, cap pathological entries, and leave
missing index records empty. Missing coverage never produces placeholder text.
C14's generic compiler and validation rules are documented in
[`definition-source-compiler.md`](definition-source-compiler.md). C18's
validated three-source union, POS-conflict, and entry-size report is in
[`definition-source-coverage.md`](definition-source-coverage.md).

### 3. Compiled EPUB artifact (`language.bin`)

The next format version retains self-contained, sequential source shards but
changes identity from a definition bundle UUID to a canonical lexicon UUID.
Each candidate records:

- exact rendered surface and hash;
- primary canonical lemma ID;
- bounded alternative canonical IDs;
- contextual POS/morphology confidence;
- frequency-derived difficulty;
- flags for fallback, ambiguity, truncation and named-entity handling.

Definition-source UUIDs are not embedded in the EPUB. Sources can be installed,
removed, reordered or updated without recompiling books, provided they target
the same canonical UUID.

### 4. Learning state

Learning state remains global and WAL-protected, but is keyed by canonical UUID:

```text
/.crosspoint/language-state/<canonical-uuid>/
```

Known/Learning/Ignore applies to the lexical item, independent of which
installed definition source explains it. A migration file maps old de-DE global
IDs to canonical IDs by exact normalized lemma/POS key; migration is atomic and
idempotent.

## Desktop linguistic pipeline

### Canonical morphology

The production morphology source is DWDSmor 0.18.0 Open Edition. DWDSmor
enumerates lemmas, coarse/fine POS, inflectional features, compounds and
word-formation analyses. The package, individual automata, build revision and
hashes are pinned in the canonical manifest.

The Open Edition uses the comprehensive DWDSmor grammar with a sample lexicon,
so its inflection behavior is suitable but open-class lexical coverage is lower
than the unavailable private DWDS Edition. Contextual inference cannot recover
a candidate absent from morphology. The measured C09 baseline in
[`contextual-german-baseline.md`](contextual-german-baseline.md) therefore
requires an offline fallback from the retained Wiktionary/de-DE form inventory.
The same contextual policy ranks that candidate union, so this adds no device
model, search, or runtime allocation.

### Contextual selector

The production selector is ZDL's static German spaCy pipeline because it is
trained for lexicographic German and reports approximately 98.6% lemmatization
accuracy with contextual POS tagging. C10 measured identical accuracy on the
C09 corpus for static and transformer models, while the transformer used about
967 MB more peak host RSS and was slower on CPU. The evidence and reproduction
command are recorded in [`zdl-model-selection.md`](zdl-model-selection.md).

For every XHTML spine:

1. Extract visible text while retaining source offsets and block boundaries.
2. Segment sentences and tokenize with a pinned tokenizer.
3. Run contextual POS/lemma inference on complete sentences.
4. Query DWDSmor for all morphological analyses of each surface.
5. Score analyses by contextual lemma/POS agreement, morphological validity,
   case, corpus prior and frequency.
6. Map accepted analyses to canonical IDs.
7. Retain the primary result plus at most seven credible alternatives.
8. Emit shard markers and deterministic candidate records.

The contextual model never invents a canonical entry. DWDSmor constrains valid
morphology; the model ranks analyses. When they disagree or confidence is low,
the artifact retains alternatives rather than silently choosing one.

### Determinism and evaluation

Production builds pin:

- DWDSmor automaton hash;
- contextual model package/version/hash;
- spaCy and tokenizer versions;
- POS mapping table version;
- scoring-policy version;
- frequency provider/version;
- canonical lexicon UUID.

Golden tests include sentence-level ambiguities, not isolated words. Required
cases include noun/verb capitalization, sentence-initial verbs, separable verbs,
participles used adjectivally, nominalized adjectives, compounds, names,
archaic forms and typographic variants.

## Browser and host workflow

A full spaCy/DWDSmor pipeline is not realistically executed by the ESP32 or by
the current small JavaScript Worker. The quality-first path is therefore a host
compiler:

```text
crossink-dictionary build-lexicon ...
crossink-dictionary build-definitions ...
crossink-dictionary compile-epub input.epub output.epub
```

C12 implements the current command as `scripts/compile_contextual_epub.py`;
its preservation, transaction, determinism, and v4 metadata behavior are
specified in [`contextual-epub-compiler.md`](contextual-epub-compiler.md).
The resulting EPUB remains uploadable through File Transfer and receives the
same transactional device-side extraction and validation as today. The WebUI
continues to install runtime lexical/definition bundles and manage source order.

The current browser-only exact-form compiler remains available during
migration, clearly labeled as legacy/basic analysis. A future browser-native
contextual backend is optional and must produce byte-identical artifacts for a
fixed model before replacing the host compiler.

## Firmware runtime

### Installation and attachment

The reader installs one canonical runtime bundle and up to three definition
sources per canonical UUID. All installs use staging directories, full CRC
validation and atomic rename. A small atomic attachment record stores source
UUIDs and display order; invalid or missing sources are skipped without
invalidating the book.

### Lookup session

The lookup session opens the existing `language.bin` reader and learning state,
then loads at most three bounded definition-source descriptors. A descriptor
contains fixed paths, metadata and one index source; it does not contain the
index or definitions.

For a selected canonical ID:

1. Read one eight-byte index record from source 1.
2. If present, stream fields into the current definition page.
3. Switch the single SD reader to source 2 and repeat.
4. Continue with source 3.
5. Paginate only when the existing page overflows.

Source boundaries use a labeled centered divider. Sense fields within a source
use the smaller meaning gap. The existing one-page buffer and pager are reused.

### RAM budget

| Item | Additional bound |
| --- | ---: |
| Three definition source descriptors and paths | ≤ 3 KB |
| Index record scratch | 8–16 B |
| Source label/render state | ≤ 128 B |
| Additional definition page | 0 B |
| Additional framebuffer | 0 B |
| Morphology/context model on device | 0 B |

The target is no more than 3.5 KB additional steady lookup-session memory over
the current implementation. Definition sources are sequential, so adding a
source increases SD reads and storage but not definition-page RAM. Hardware
validation must confirm free heap, largest block and task high-water marks after
100 repeated multi-source lookups.

## Failure and lifecycle behavior

- Missing canonical bundle: dictionary action unavailable.
- Canonical UUID mismatch: reject EPUB or source at validation.
- Missing definition source: skip it and show available sources.
- Corrupt source index/entry: log and skip that source for the lookup.
- No source has an entry: return to shortlist with a translated failure message.
- Source update: atomically replace only that source.
- Canonical update with stable IDs: version/fingerprint rules decide whether it
  is compatible; otherwise require migration and EPUB recompilation.
- Cache deletion: learning state and installed sources survive.
- Interrupted attachment write: retain previous valid attachment record.

## Language independence

Firmware understands canonical IDs, source descriptors, confidence and
structured fields; it contains no DWDS, German POS or spaCy logic. Host analysis
uses a provider interface:

```text
LanguageAnalyzer
  tokenize(document)
  analyze_sentence(tokens)
  canonicalize(analysis)
  score_alternatives(context, analyses)
```

German supplies the DWDSmor/ZDL implementation. Other languages can provide a
different morphology and contextual model without changing the firmware or
binary source package contract.

## Acceptance criteria

The architecture is complete only when:

- contextual golden tests resolve the documented German ambiguity set;
- de-DE, dict.cc and Kaikki install independently and display in configured
  order;
- all three sources stream through one page buffer and one SD handle;
- existing learning state migrates without changing nonzero statuses;
- a production novel compiles deterministically twice;
- malformed model output, canonical bundles, source indexes and entries fail
  safely;
- normal page-turn timing is unchanged;
- cold/warm lookup latency and SD operations are recorded on X3 and X4;
- 100 repeated lookups/status edits show no heap decline or file-handle leak.

# Contextual EPUB Compiler

## Build workflow

C12 adds the pinned host-only EPUB compiler:

```sh
.cache/contextual/.venv/bin/python scripts/compile_contextual_epub.py \
  input.epub output.epub \
  --canonical tmp.local/german-canonical.cplex \
  --form-inventory tmp.local/german-wiktionary.cpdict \
  --frequency-ranking wordfreq
```

The command loads the C11 canonical index, DWDSmor Open Edition, the de-DE form
inventory, ZDL static model, policy-2 fusion, and finite separable-verb
recombination. Only canonical IDs and bounded evidence enter the EPUB. Model,
form, and frequency indexes remain on the host.

## EPUB preservation and transaction

The compiler resolves the package document selected by
`META-INF/container.xml`, follows its manifest and spine, and modifies only
spine XHTML plus `META-INF/crossink/language.bin`. It copies the canonical OPF
bytes and every unrelated ZIP entry unchanged. An existing language artifact is
replaced rather than duplicated.

Output is written to a sibling temporary file, closed, synced, and atomically
renamed. Analysis or ZIP failure leaves an existing output untouched and removes
the temporary file. Input and output paths must differ. Original ZIP metadata is
preserved; the added language entry has a fixed timestamp and storage method,
so two builds from the same EPUB and pinned assets are byte-identical.

## Text and artifact behavior

Visible block text is analyzed with source offsets while script, style, head,
and non-rendered text remain excluded. XHTML word tokens still define 64-token
shards, keeping marker placement compatible with firmware's rendered-word
lookup. Decomposed visible Unicode is normalized for analysis and aligned back
to original XHTML offsets; the publisher text itself is not rewritten.

`language.bin` version 5 records:

- canonical UUID `c6246d63-38eb-5df8-9d83-ab0d812503f9` for the current seed;
- primary and at most seven alternate canonical IDs;
- normalized contextual confidence and optional `wordfreq` difficulty;
- one validated layout-1 contextual grammar descriptor for the primary ID;
- contextual, folded, fallback, ambiguity, truncation, and proper-noun flags;
- DWDSmor, ZDL, spaCy, canonical POS, analysis-policy, tokenizer, frequency, and
  compiler provenance.

Repeated surfaces are merged only inside their 64-token source shard. Candidate
IDs are ordered by evidence then canonical ID, so output does not depend on
provider iteration order. Primary grammar unions field by field; a repeated
value or structural conflict produces descriptor zero. Analyses absent from the
canonical lexicon are counted and omitted rather than assigned an unstable
book-local identity. The command also reports available occurrence grammar,
missing contextual features, POS mismatches and within-shard conflicts.

A production-provider fixture containing sentence-initial `Laden`, finite
`steht … auf`, participial `geschrieben`, and `Goethe` compiled with zero
unmapped candidates. Two runs produced byte-identical EPUBs; `steht` encoded
canonical `aufstehen` first and `stehen` second.

## Resource and hardware verification

The compiler intentionally builds a host dictionary from canonical keys for
O(1) mapping and retains one spine's analyzed records while constructing the
bounded artifact. These desktop allocations avoid repeated binary searches and
have no device cost. The emitted EPUB contains no morphology or model files.

During the staged v5 implementation, compiler output is intentionally newer
than firmware acceptance until U09 performs the v5-only parser cutover. Do not
replace production reading EPUBs before U17. Hardware validation follows
U18–U19: upload the compiled EPUB and matching canonical runtime files, clear
the book's `.crosspoint/epub_<hash>/` cache, then confirm grammar, alternative
labels, direct word navigation, CRC logs, one-reader behavior, and stable heap
on X3/X4.

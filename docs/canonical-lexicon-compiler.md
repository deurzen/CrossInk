# Canonical German Lexicon Compiler

## Purpose

`scripts/build_canonical_lexicon.py` builds the canonical lexical identity
bundle used by contextual EPUBs and every attached definition source. The
bundle owns dense learning IDs; it contains no definitions and does not make a
definition edition authoritative for morphology.

The initial identity set is seeded from the verified lexical records of the
existing de-DE bundle. This is compiler input only: `forms.bin`, entries, and
definition text are not copied into the canonical bundle. DWDSmor and ZDL
manifests are embedded as provenance references, while their wheels and models
remain external desktop assets.

## Build

```sh
scripts/build_canonical_lexicon.py \
  --de-de-bundle tmp.local/german-wiktionary.cpdict \
  --output tmp.local/german-canonical.cplex
```

The production seed currently produces:

| Field | Value |
| --- | --- |
| Lexemes | 181,609 |
| Canonical UUID | `c6246d63-38eb-5df8-9d83-ab0d812503f9` |
| Lexical payload SHA-256 | `294d9bf020620a17210edaa118f97fa7e1070af34f8f7755ccf95fadd8a6dfc2` |
| `.cplex` SHA-256 | `6ece9a13a8f495121477d5f4abd2dab0b9bcc3d27fd61f218d4fe6b0bb294351` |
| Canonical POS layout | 1 |
| Analysis policy | 2 |
| Archive size | approximately 4.8 MiB |

Two independent builds from the same seed are byte-identical. Input order does
not affect IDs: NFC `(headword UTF-8, POS)` keys are sorted before records are
written, and duplicate keys merge their flags. The canonical UUID is UUIDv5 of
the `lexemes.bin || headwords.bin` SHA-256, so model metadata changes do not
silently renumber learning identity.

## Archive contents

```text
manifest.json
runtime/meta.bin
runtime/lexemes.bin
runtime/headwords.bin
runtime/licenses.txt
compiler/analyzer.json
compiler/dwdsmor-open.json
compiler/zdl-model.json
compiler/licenses.txt
```

`compiler/analyzer.json` records the de-DE seed archive hash and UUID, canonical
POS version, analysis policy version, compiler version, and hashes of the
canonicalized DWDSmor/ZDL manifests. The ZDL wheel is not redistributed: its
upstream code is GPL-3.0-or-later, while the wheel does not declare a model
license and training datasets have separate terms, so model provenance records
`NOASSERTION`.

## Resource behavior

The seed loader opens only `meta.bin`, `lexemes.bin`, `headwords.bin`, and the
license. It does not materialize the 27 MiB form table or 16 MiB definition
payload. Lexemes are sorted once as prepared host records, then compact runtime
byte arrays are emitted. These are desktop heap allocations required to assign
global dense IDs; no compiler index or model enters firmware RAM.

The bundle is not yet installable on hardware. C19 and C20 add bounded runtime
readers and transactional installation. Hardware verification at that point
must upload this exact UUID, validate all CRCs, and confirm no compiler files
are copied to `/.crosspoint/lexicons/<uuid>/`.

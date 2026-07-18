# Contextual dictionary compiler environment

This directory pins the desktop-only German contextual selector described in
[`docs/contextual-dictionary-architecture.md`](../../../docs/contextual-dictionary-architecture.md).
None of these packages or model files are uploaded to the reader.

## DWDSmor Open Edition

The production morphology baseline is `dwdsmor` 0.18.0 Open Edition on
CPython 3.12. Its grammar is comprehensive, but its sample lexicon has lower
open-class coverage than the unavailable private DWDS Edition. Contextual
selection can rank only analyses that DWDSmor emits, so coverage must be
measured explicitly in C09/C10 and retained definition lexicons will provide a
bounded fallback later.

Create the repository-local environment with `uv` and retrieve the pinned
wheel:

```sh
uv venv --python 3.12 .cache/contextual/.venv
uv pip install --python .cache/contextual/.venv/bin/python \
  -r scripts/dictionary/contextual/requirements-dwdsmor.lock
uv pip install --python .cache/contextual/.venv/bin/python \
  --reinstall .cache/contextual/wheels/dwdsmor-0.18.0-py3-none-any.whl
.cache/contextual/.venv/bin/python \
  scripts/dictionary/contextual/verify_dwdsmor_open.py
```

The last reinstall command is optional; it demonstrates that the locally
cached wheel is sufficient. Download that ignored local wheel from the URL in
`dwdsmor-open.json`. The verifier pins all packaged automata by exact size and
SHA-256, checks the wheel RECORD, and runs representative noun/verb,
verb/adjective, inflection, and compound analyses. DWDSmor and its Open Edition
are GPL-2.0-only desktop dependencies and are not linked into firmware.

On NixOS, create the venv with the Nix CPython interpreter:

```sh
nix-shell -p python312 --run \
  'uv venv --python "$(command -v python3.12)" .cache/contextual/.venv'
```

## ZDL static model

The production candidate is `de-zdl-lg` 4.0.0 with spaCy 3.8.14 on CPython
3.12. Its 598 MiB wheel is fetched from ZDL's public Git.UP package registry and
is authenticated by the SHA-256 fragment in `requirements-zdl.lock`. The
manifest also pins the upstream model source tag and commit.

Create an isolated environment with CPython 3.12:

```sh
python3.12 -m venv .venv-zdl
.venv-zdl/bin/python -m pip install -r scripts/dictionary/contextual/requirements-zdl.lock
.venv-zdl/bin/python scripts/dictionary/contextual/verify_zdl_model.py
```

The verifier rejects another Python minor, spaCy version, model version,
pipeline component set, installed wheel file digest, or reference inference.
Upstream code is GPL-3.0-or-later, but the wheel metadata leaves the model
license blank and upstream notes separate training-dataset terms. The pinned
manifest therefore records the model license as `NOASSERTION`; the wheel stays
an external compiler asset and is not redistributed in `.cplex` bundles.
The reference fixture is intentionally a reproducibility snapshot rather than
the linguistic gold corpus planned in C09.

Only POS, morphology, and lemma inference are enabled. Parser and NER weights
are excluded when loading the model because the initial fusion stage does not
consume those annotations. This reduces host compile memory and time without
changing firmware resources.

The lock was resolved on Linux x86-64. On another host platform, preserve the
exact top-level Python, spaCy, and model versions and regenerate only
platform-specific transitive wheels; do not change `zdl-model.json` without a
new reference run and review.

## Canonical and EPUB builds

Build the deterministic canonical identity bundle and then compile an EPUB with
the pinned contextual providers:

```sh
scripts/build_canonical_lexicon.py \
  --de-de-bundle tmp.local/german-wiktionary.cpdict \
  --output tmp.local/german-canonical.cplex
.cache/contextual/.venv/bin/python scripts/compile_contextual_epub.py \
  input.epub output.epub \
  --canonical tmp.local/german-canonical.cplex \
  --form-inventory tmp.local/german-wiktionary.cpdict
```

The EPUB build is transactional and leaves the canonical OPF untouched. See
[`docs/contextual-epub-compiler.md`](../../../docs/contextual-epub-compiler.md).
Definition JSON can be aligned independently with:

```sh
scripts/build_definition_source.py source.json output.cpdef \
  --canonical tmp.local/german-canonical.cplex
```

See [`docs/definition-source-compiler.md`](../../../docs/definition-source-compiler.md).
The retained German Wiktionary source has a direct verified importer:

```sh
scripts/build_de_de_definition_source.py \
  --dictionary tmp.local/german-wiktionary.cpdict \
  --canonical tmp.local/german-canonical.cplex \
  --output tmp.local/german-wiktionary.cpdef
```

A private dict.cc export is streamed without loading the TSV wholesale:

```sh
scripts/build_dictcc_definition_source.py \
  --source tmp.local/dict-de-en.txt \
  --canonical tmp.local/german-canonical.cplex \
  --output tmp.local/dictcc-de-en.cpdef
```

Both the dict.cc source and output remain private-use artifacts. Kaikki's
English-Wiktionary German JSONL is also streamed line-by-line:

```sh
scripts/build_kaikki_definition_source.py \
  --source tmp.local/kaikki.org-dictionary-German.jsonl \
  --canonical tmp.local/german-canonical.cplex \
  --output tmp.local/kaikki-de-en.cpdef
```

Validate and compare up to three compiled sources with
`scripts/report_definition_coverage.py`; the production aggregate is documented
in [`docs/definition-source-coverage.md`](../../../docs/definition-source-coverage.md).

## Transformer benchmark environment

`de-zdl-dist` is pinned only to reproduce C10. Install
`requirements-zdl-dist.lock` as an overlay in a separate environment; it brings
PyTorch and platform-specific dependencies. The measured comparison in
[`docs/zdl-model-selection.md`](../../../docs/zdl-model-selection.md) found no
accuracy gain and substantially higher host RSS, so the normal compiler remains
on `de-zdl-lg`.

# Contextual dictionary compiler environment

This directory pins the desktop-only German contextual selector described in
[`docs/contextual-dictionary-architecture.md`](../../../docs/contextual-dictionary-architecture.md).
None of these packages or model files are uploaded to the reader.

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

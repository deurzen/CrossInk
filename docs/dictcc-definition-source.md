# Private dict.cc Definition Source

C16 streams a private dict.cc German-English export into a canonical `.cpdef`:

```sh
scripts/build_dictcc_definition_source.py \
  --source tmp.local/dict-de-en.txt \
  --canonical tmp.local/german-canonical.cplex \
  --output tmp.local/dictcc-de-en.cpdef
```

The source and output are private-use artifacts and must not be committed,
published, or given away. The importer preserves the export's license header in
`device/licenses.txt` and records `LicenseRef-dict.cc-private-use` provenance.

## Production result

Source SHA-256:
`c1663edad350a60b06b9e913cc49249c80ed5cff77917021a9313138cfdc0944`.

| Metric | Value |
| --- | ---: |
| Data lines streamed | 1,312,270 |
| Aligned lines | 303,882 |
| Emitted canonical-key entries | 304,301 |
| Malformed/unsupported TSV rows | 3,261 |
| Unaligned lines | 1,005,127 |
| Covered canonical lexemes | 105,036 / 181,609 (57.836341%) |
| Truncated fields | 0 |
| Source UUID | `122f8887-ab1b-574b-9026-ced568335fc0` |
| Payload SHA-256 | `0a8b8f15cf72c757310ca7ee255c88937e1020237f2457d3e4349838ef0fcc4a` |
| Private `.cpdef` SHA-256 | `39dfb0c23f5190f9877217e887bbc491f298e629d147d56709972a1c61fb211e` |
| Archive size | approximately 9.6 MiB |

Two complete imports were byte-identical.

## Alignment policy

The importer HTML-decodes and NFC-normalizes text, removes dict.cc gender and
trailing annotation syntax from German alignment keys, and maps the export's
noun/verb/adjective/adverb/preposition/pronoun/conjunction and participle tags
to canonical POS classes. Unlabeled headwords align only when the canonical
headword has exactly one POS, avoiding silent assignment of ambiguous phrases.
English translations remain definition fields; source POS and subject labels
remain structured POS/usage fields.

Input is read one line at a time with a 64 KiB line cap. The 77 MiB TSV is never
loaded wholesale. Canonical fields must still remain in host memory long enough
to deduplicate and sort deterministic entries. Each canonical entry retains the
lexicographically first 128 unique fields if a pathological source exceeds the
cap; discarded unique fields are counted. The production source did not hit the
cap.

No dict.cc data participates in morphology or learning identity, and no source
index is loaded by firmware. Device installation and one-reader streaming remain
C19–C27 work.

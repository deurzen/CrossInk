# Kaikki English-Wiktionary Definition Source

C17 streams Kaikki's English-Wiktionary German JSONL into a canonical de→en
`.cpdef`:

```sh
scripts/build_kaikki_definition_source.py \
  --source tmp.local/kaikki.org-dictionary-German.jsonl \
  --canonical tmp.local/german-canonical.cplex \
  --output tmp.local/kaikki-de-en.cpdef
```

## Production result

Source SHA-256:
`89cd1841f6ddabec84a1291780d2499d1b8c74d4daf59add0f0522e718c83dd9`.

| Metric | Value |
| --- | ---: |
| JSONL records streamed | 368,352 |
| Aligned records | 64,787 |
| Covered canonical lexemes | 62,710 / 181,609 (34.530227%) |
| Unaligned records | 303,557 |
| Malformed/oversized records | 8 |
| Parsed senses | 628,945 |
| Parsed glosses | 957,404 |
| Parsed examples | 29,311 |
| Truncated fields | 0 |
| Source UUID | `84546eba-7af7-53e3-8f0c-a9e79414cc9c` |
| Payload SHA-256 | `16aa7e402e6f77bb682c7de7541995f1a6cbd643170e4e6ce0f4f161e567f11e` |
| `.cpdef` SHA-256 | `44109b1305fcd517b2a40e1f3ea4af832c3761343a559e3b5819a06edc88e782` |
| Archive size | approximately 13 MiB |

Two complete 1 GiB imports were byte-identical.

## Mapping policy

Kaikki noun, verb, adjective, adverb, name, function-word, phrase,
abbreviation, and other source POS labels map explicitly to canonical POS
classes. Headword/POS must match exactly after NFC normalization; form-of data
does not invent morphology or redirect learning identity.

Sense glosses become definitions. Qualifiers, tags, raw tags, and topics become
usage fields. German examples retain English translations in the same example
field, and record-level etymology remains an etymology field. Exact duplicate
fields merge. The deterministic per-entry cap is 128 lexicographically ordered
fields; production data did not hit it.

The importer reads one JSON object at a time with a 256 KiB line cap. Invalid
JSON, unsupported records, and empty definitions are counted and skipped rather
than aborting the 1 GiB build. Canonical field sets remain in host memory only
to deduplicate and emit canonical-ID order; the source file itself is never
materialized.

The extraction is derived from English Wiktionary contributors via Kaikki.org
and is distributed under CC-BY-SA-4.0 (English Wiktionary also offers GFDL).
Neither source content nor its index participates in morphology or device
learning identity. Firmware streaming remains C19–C27 work.

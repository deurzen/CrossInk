# German Wiktionary Definition Source

C15 converts the retained German Wiktionary `.cpdict` into a definition-only
`.cpdef` aligned to the C11 canonical lexicon:

```sh
scripts/build_de_de_definition_source.py \
  --dictionary tmp.local/german-wiktionary.cpdict \
  --canonical tmp.local/german-canonical.cplex \
  --output tmp.local/german-wiktionary.cpdef
```

## Production result

| Field | Value |
| --- | --- |
| Canonical UUID | `c6246d63-38eb-5df8-9d83-ab0d812503f9` |
| Source UUID | `8bccd788-22d0-5306-a00d-f8d96b8d8652` |
| Canonical lexemes | 181,609 |
| Covered lexemes | 181,609 (100%) |
| Unmatched source entries | 0 |
| Payload SHA-256 | `573879e3ad2791f0c7af2308e9b21e5e34b61e5ece1fbe09d1697445401d847e` |
| `.cpdef` SHA-256 | `a9408a61a82142bc17cbe80a199900009cf45a69cbd018ac7e9d025366ea7c99` |
| Archive size | approximately 17 MiB |
| License | CC-BY-SA-4.0 |

Two full builds were byte-identical. The source has complete coverage because
the initial canonical identity set is deliberately seeded from the same verified
lexical records. Future canonical additions may have empty records in this
source without changing the fixed-index contract.

The importer validates the original archive hashes, entry CRC, ordered entry
ranges, field headers, NFC UTF-8, and source license before alignment. Existing
definitions, examples, usage labels, etymologies, cross-references, and compound
components retain their structured field types. Package provenance records the
original archive hash and bundle UUID.

Only `device/meta.bin`, `device/entry-index.bin`, `device/entries.bin`, and
`device/licenses.txt` are eventual installation inputs. The source does not
provide morphology candidates and never owns learning identity. Firmware and
hardware verification remain deferred to C19–C27; those phases must stream this
17 MiB source through one SD reader without loading its 1.45 MiB index.

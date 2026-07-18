# Contextual Grammar v5 Production Evaluation

U08 evaluates grammar descriptor layout 1 without changing ranking policy or
safety caps. The pinned static provider stack is DWDSmor Open 0.18.0, the
181,609-lexeme de-DE form inventory, `de-zdl-lg` 4.0.0, spaCy 3.8.14 and
analysis policy 2. Canonical UUID is
`c6246d63-38eb-5df8-9d83-ab0d812503f9`.

## Ambiguity regression corpus

The 16-case checked-in baseline remains unchanged at 13/16 contextual identity,
16/16 morphology coverage and 14/16 fused-primary accuracy. Its schema-2 report
now also records normalized ZDL features, descriptor words and descriptor status
for every target.

The two existing ranking failures remain explicit:

- `Liebe deinen Nächsten`: ZDL and fusion select the noun `Liebe`; descriptor
  `0x00000240` consequently reports feminine singular noun evidence. The
  descriptor does not disguise the wrong primary identity.
- archaic `hub`: ZDL has no contextual features and folded nouns outrank the
  exact verb `heben`; the descriptor is zero.

No v5 grammar or aggregation change altered canonical ordering. The baseline is
regenerated with:

```sh
.cache/contextual/.venv-static/bin/python \
  scripts/dictionary/contextual/evaluate_german_corpus.py \
  --form-inventory tmp.local/german-wiktionary.cpdict \
  --output test/data/contextual/german-ambiguity-baseline.json
```

On NixOS the local run also set `LD_LIBRARY_PATH` to the pinned GCC runtime
listed in `.claude/CONTEXT.md`.

## Production-book dry runs

Both byte-identical pre-v4 backups were compiled to temporary v5 EPUBs. The
installed/current v4 books and their backups were not modified; U17 performs the
later production replacement.

| Measurement | Homma novel | Klein collection |
| --- | ---: | ---: |
| Spines | 298 | 134 |
| Shards | 4,244 | 1,991 |
| Final candidates | 126,718 | 57,242 |
| Nonzero final descriptors | 108,067 (85.28%) | 43,784 (76.49%) |
| Available occurrence descriptors | 111,504 (85.49%) | 47,929 (77.11%) |
| Occurrences without contextual features | 16,714 (12.81%) | 13,197 (21.23%) |
| Occurrences suppressed by POS mismatch | 2,214 (1.70%) | 1,031 (1.66%) |
| Within-shard conflicts cleared to zero | 124 (0.098% of records) | 113 (0.197% of records) |
| Unmapped analysis candidates | 11,586 | 4,495 |
| v4 `language.bin` | 3,477,193 bytes | 1,561,037 bytes |
| v5 `language.bin` | 3,984,270 bytes | 1,790,208 bytes |
| Exact record growth | 506,872 bytes | 228,968 bytes |
| Metadata/alignment growth | 205 bytes | 203 bytes |
| Total artifact growth | 507,077 bytes | 229,171 bytes |

The record growth is exactly four bytes per candidate. Total artifact growth is
slightly larger because v5 adds bounded compiler/grammar metadata; this corrects
the earlier estimate that treated record growth as the complete artifact delta.

Both temporary EPUB ZIPs pass `ZipFile.testzip()`. Their `language.bin` header
and payload CRCs pass and preserve the v4 spine, shard, candidate and local-lemma
counts.

| Artifact | SHA-256 |
| --- | --- |
| Homma temporary v5 EPUB | `7f48cc6fe957c0fa95c63c66d1f1941db99656ab7a9e95df4fe90f0dd48af818` |
| Homma v5 `language.bin` | `496c33d142512c2dc8b9ab818453993a0f9db9c896849b227734e92e6c75371c` |
| Klein temporary v5 EPUB | `8c5d2a324a2ead714ab7c3c6f8cfb0ffc987f24ea2b9be22a1057038dc2618b0` |
| Klein v5 `language.bin` | `bcd1a674e79a18bd21bd28dbb5afd5fbc49aa7dfb9db1550eef27d388000e56f` |

## Manual descriptor review

The review decoded v5 words independently back to canonical fields and checked
them against source sentences. Missing fields are acceptable; a field is never
filled from form-only morphology.

| Class/context | Primary | Descriptor | Review |
| --- | --- | --- | --- |
| `Anna knipste noch ein Foto` | `knipsen`, verb | `0x0000DA80`: past, indicative, third-person singular, finite | correct |
| `Er steht jeden Morgen früh auf` | `aufstehen`, verb | `0x0000BA80`: present, indicative, third-person singular, finite | correct; recombination retains token grammar |
| `hat den Brief geschrieben` | `schreiben`, verb | `0x00018000`: participle | correct, deliberately incomplete |
| `Häuser schossen an ihr vorbei` | `Haus`, noun | `0x00000460`: neuter plural | correct; case unavailable |
| `die beiden Männer` | `Mann`, noun | `0x00000420`: masculine plural | correct; case unavailable |
| `den schönen Abend` | `schön`, adjective | `0x0000022A`: accusative masculine singular positive | correct |
| `mit großen Augen` | `groß`, adjective | `0x0000040B`: dative plural positive | correct; gender unavailable |
| noun/verb `Laden` ambiguity corpus | `Laden` / `laden` | noun `0x00000220`; verb `0x0000AC80` | both intended readings correct |
| repeated `sagte` across prose/dialogue | `sagen`, verb | mostly past third-person singular; incompatible shard evidence becomes zero | conservative aggregation works |

## Observed model/ranking limitations

The dry run is not presented as perfect grammatical truth. Manual inspection
also found:

- some production `Laden` noun occurrences receive neuter rather than masculine
  ZDL gender;
- some `sagte` occurrences receive present-tense or incomplete evidence;
- masculine-context `ihm` can select canonical `es` with neuter evidence;
- the Homma name `Bella` can select canonical `Annabell`.

The last two are primary identity/ranking errors, not descriptor packing errors.
The first two are contextual model errors. U00 deliberately forbids filling or
overriding them from an unrelated morphology alternative, and U08 adds no
surface-specific guesses. Zero/conflict behavior remains preferable to merging
contradictory occurrences.

These limitations are retained as promotion evidence alongside the two C09
failures. Hardware qualification must verify truthful formatting and bounded
behavior; it must not claim that every ZDL prediction is linguistically correct.

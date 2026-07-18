# Definition-Source Coverage and Conflicts

C18 validates all three production `.cpdef` files before comparing their fixed
canonical indexes:

```sh
scripts/report_definition_coverage.py \
  --canonical tmp.local/german-canonical.cplex \
  --source de-de=tmp.local/german-wiktionary.cpdef \
  --source dictcc=tmp.local/dictcc-de-en.cpdef \
  --source kaikki=tmp.local/kaikki-de-en.cpdef \
  --output test/data/contextual/definition-coverage.json
```

The checked-in JSON contains no private dict.cc definition text. It records
canonical IDs, public headwords for the 20 largest records, aggregate coverage,
and importer diagnostics.

## Coverage

| Source | Direction | Covered | Rate |
| --- | --- | ---: | ---: |
| German Wiktionary | de→de | 181,609 | 100.000000% |
| dict.cc | de→en | 105,036 | 57.836341% |
| Kaikki English Wiktionary | de→en | 62,710 | 34.530227% |
| dict.cc ∪ Kaikki | de→en | 115,255 | 63.463264% |
| All sources | mixed | 181,609 | 100.000000% |

Pairwise intersections are 105,036 for de-DE/dict.cc, 62,710 for
de-DE/Kaikki, and 52,491 for dict.cc/Kaikki. All three sources cover 52,491
canonical lexemes. Because de-DE seeded the initial canonical set, it has 66,354
source-only records; neither English source has records outside de-DE coverage.

## Input conflicts and unmatched data

| Source | Unaligned input rows/records | Headword exists, POS conflicts |
| --- | ---: | ---: |
| dict.cc | 1,005,127 | 20,621 |
| Kaikki | 303,557 | 5,488 |

Large unaligned counts are expected: both exports contain phrases, inflected
forms, specialist names, and lexical classes outside the initial canonical
seed. They remain excluded rather than being attached by spelling alone.
Headword-present POS conflicts are reported separately so future canonical
expansion or mapping review can target them. The private dict.cc report retains
counts only.

## Entry-size distribution

| Source | p50 | p95 | p99 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| de-DE | 70 B | 218 B | 353 B | 1,728 B |
| dict.cc | 58 B | 211 B | 381 B | 2,219 B |
| Kaikki | 100 B | 651 B | 1,265 B | 6,954 B |

All records are far below the 1 MiB format cap. Kaikki has the largest tail due
to examples, qualifiers, and etymology, but firmware still streams those bytes
through the existing definition page rather than allocating complete entries.
No production source hit the deterministic 128-field import cap.

This report changes no runtime artifacts and adds no device allocation. C19 can
use its largest-record list as corruption/boundary fixtures; hardware I/O and
heap validation remain C25–C36 work.

# Contextual Grammar Descriptor Layout 1

This document freezes the grammatical semantics required by U00. U01 embeds
this descriptor in contextual `language.bin` version 5; U02 separately defines
translated presentation and width behavior.

The descriptor explains the inflected **surface in its sentence context**. It
does not change canonical identity, select a definition source, or describe the
canonical lemma's complete paradigm.

## Authoritative evidence

The descriptor is derived only from `AnalyzedToken.context.features`, produced
by the pinned ZDL contextual model and normalized through
`CanonicalFeatures`. Ranked DWDSmor/form-inventory analyses remain authoritative
for canonical lemma identity and ranking, but their feature bundles are not
copied into the descriptor.

This separation is intentional. Form-only morphology can return a grammatical
reading that is impossible in the sentence. For example, the surface `schönen`
can have several cases; the contextual model may safely establish plural and
positive degree while leaving case unavailable. The compiler must not fill the
missing case from an unrelated morphology alternative.

A descriptor may be emitted only when all of these conditions hold:

1. at least one ranked analysis resolves to an installed canonical ID;
2. the first retained canonical analysis is the candidate's primary ID;
3. contextual POS equals the primary canonical POS, except that noun and proper
   noun are mutually compatible;
4. contextual POS is neither `UNKNOWN` nor `OTHER`;
5. the canonical contextual feature combination encodes under the strict rules
   below.

A POS mismatch or an entirely empty feature bundle produces descriptor zero. It
is an ordinary absence, not a compiler error. Unknown canonical feature strings,
reserved values, or structurally invalid feature combinations are compiler
errors; they indicate drift in a supposedly versioned analyzer contract.

This policy also preserves grammatical context for a recombined separable verb.
For example, a primary canonical identity such as `anknipsen` may come from
sentence augmentation while its finite/past/person/number values still come
from the contextual token `knipste`.

## Fixed 32-bit layout

The descriptor is an unsigned little-endian 32-bit word. Zero means that no
truthful grammatical explanation is available.

| Bits | Width | Field | Valid codes |
| ---: | ---: | --- | --- |
| 0-2 | 3 | case | `0..4` |
| 3-4 | 2 | degree | `0..3` |
| 5-6 | 2 | gender | `0..3` |
| 7-8 | 2 | mood | `0..3` |
| 9-10 | 2 | number | `0..2`; `3` reserved |
| 11-12 | 2 | person | `0..3` |
| 13-14 | 2 | tense | `0..3` |
| 15-16 | 2 | verb form | `0..3` |
| 17-31 | 15 | reserved | zero |

### Case

| Code | Canonical value |
| ---: | --- |
| 0 | unavailable |
| 1 | nominative |
| 2 | accusative |
| 3 | dative |
| 4 | genitive |
| 5-7 | invalid/reserved |

### Degree

| Code | Canonical value |
| ---: | --- |
| 0 | unavailable |
| 1 | positive |
| 2 | comparative |
| 3 | superlative |

### Gender

| Code | Canonical value |
| ---: | --- |
| 0 | unavailable |
| 1 | masculine |
| 2 | feminine |
| 3 | neuter |

### Mood

| Code | Canonical value |
| ---: | --- |
| 0 | unavailable |
| 1 | indicative |
| 2 | subjunctive |
| 3 | imperative |

### Number

| Code | Canonical value |
| ---: | --- |
| 0 | unavailable |
| 1 | singular |
| 2 | plural |
| 3 | invalid/reserved |

### Person

| Code | Canonical value |
| ---: | --- |
| 0 | unavailable |
| 1 | first |
| 2 | second |
| 3 | third |

### Tense

| Code | Canonical value |
| ---: | --- |
| 0 | unavailable |
| 1 | present |
| 2 | past |
| 3 | perfect |

### Verb form

| Code | Canonical value |
| ---: | --- |
| 0 | unavailable/not verbal |
| 1 | finite |
| 2 | infinitive |
| 3 | participle |

## Structural validation

Validation is independent of canonical-package reads so malformed candidate
records can be rejected while the bounded book record is decoded.

- Reserved bits `[17,32)` must be zero.
- Case codes `5..7` and number code `3` are invalid.
- An infinitive descriptor must contain no case, degree, gender, mood, number,
  person, or tense.
- A participle descriptor must contain no mood or person. Tense and nominal or
  adjectival agreement fields are allowed.
- A finite descriptor must contain no case, degree, or gender. Mood, number,
  person, and tense may be unavailable independently.
- When verb form is unavailable, mood and tense must also be unavailable.
  Person remains allowed because contextual pronouns carry person without a
  verb-form field.

The validator deliberately does not require every linguistically expected
field. `finite + past` without person is incomplete but truthful and valid;
missing evidence is omitted in the UI. It rejects contradictions, not
incompleteness.

## Same-surface aggregation

One candidate record represents a surface within one 64-token shard, while the
same surface may occur more than once. Grammar evidence is associated only with
the canonical ID that was primary for that occurrence. An occurrence where the
same ID was merely an alternative contributes no grammar evidence to it.

For each canonical ID, merge primary descriptors field by field:

1. zero evidence contributes nothing;
2. unavailable plus a known value yields the known value;
3. equal known values remain unchanged;
4. two different known values create a sticky conflict for that canonical ID.

If the final merged candidate chooses a conflicted canonical ID as primary, its
descriptor is zero. Otherwise it receives the non-conflicting union for that
primary ID. Candidate ordering, confidence, provenance and canonical IDs remain
unchanged.

The page shortlist applies the same conservative rule across shard records. If
the same visible surface is deduplicated across shards and its primary local ID
differs, grammar becomes unavailable. If the primary ID matches, descriptors
merge field by field and any conflict clears the complete descriptor. This is
necessary because the shortlist selects a surface, not one exact occurrence on
the rendered page.

No conflict sentinel is serialized. Conflict is compiler/shortlist state only;
the on-disk/runtime descriptor is either a valid explanation or zero.

## Deterministic examples

| Surface context | Primary identity/POS | Descriptor fields | Result |
| --- | --- | --- | --- |
| `Er knipste ein Foto.` | `knipsen`, verb | indicative, singular, third, past, finite | emit |
| `Er knipste das Licht an.` | `anknipsen`, verb | indicative, singular, third, past, finite | emit; recombined lemma does not erase context |
| `Im Laden` | `Laden`, noun | neuter, singular; case unavailable | emit only known fields |
| `die schönen Häuser` | `schön`, adjective | positive, plural; case unavailable | emit only known fields |
| `Sie gehen` | `sie`, pronoun | nominative, plural, third; verb form unavailable | emit; person is valid for pronouns |
| noun/verb POS disagreement | any | otherwise populated | zero due to incompatible POS |
| repeated `Sie` with singular and plural readings in one selected surface | same primary ID | number conflict | zero after aggregation |
| unsupported canonical feature string | any | unversioned value | compiler error |

The `knipste` descriptor word is:

```text
mood=1, number=1, person=3, tense=2, verbForm=1
= (1 << 7) | (1 << 9) | (3 << 11) | (2 << 13) | (1 << 15)
= 0x0000DA80
```

Checked-in encoder/decoder vectors in U04 must include every valid field value,
all reserved encodings, structural contradictions, zero, the examples above,
and merge-order permutations.

## Diagnostics and versioning

The v5 compiler reports deterministic counts for:

- candidates with a nonzero descriptor;
- candidates with no contextual features;
- candidates suppressed by POS mismatch;
- candidates cleared by within-shard grammar conflict.

The firmware does not log ordinary descriptor absence. Invalid serialized words
are candidate-record corruption and use the bounded book-reader failure path.

This layout is **grammar descriptor version 1**. Its values are file-format
contracts, not enum ordinals imported from spaCy or DWDSmor. Future values use a
new contextual book format or explicitly versioned descriptor layout; reserved
bits must not be reinterpreted under v5.

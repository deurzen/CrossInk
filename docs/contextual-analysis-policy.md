# German Contextual Analysis Policy v1

This document defines policy version 1 used to combine ZDL contextual output
with DWDSmor morphology. The executable source of truth is
`scripts/dictionary/contextual/analysis_policy.py`.

## Canonical lexical classes

Canonical IDs use a normalized lemma plus one coarse lexical class. Numeric
values intentionally retain the existing runtime POS layout where practical:

| Value | Class | DWDSmor | ZDL universal POS |
| ---: | --- | --- | --- |
| 0 | Unknown | unmapped | unmapped |
| 1 | Noun | `NN` | `NOUN` |
| 2 | Verb | `V` | `VERB`, `AUX` |
| 3 | Adjective | `ADJ` | `ADJ` |
| 4 | Adverb | `ADV`, `PROADV` | `ADV` |
| 5 | Pronoun | `PPRO`, `REL`, `WPRO` | `PRON` |
| 6 | Determiner | `ART`, `DEM`, `INDEF`, `POSS` | `DET` |
| 7 | Adposition | `PREP`, `POSTP`, `PREPART` | `ADP` |
| 8 | Conjunction | `CONJ` | `CCONJ`, `SCONJ` |
| 9 | Numeral | `CARD`, `FRAC`, `ORD` | `NUM` |
| 10 | Particle | `PTCL` | `PART` |
| 11 | Interjection | `INTJ` | `INTJ` |
| 12 | Proper noun | `NPROP` | `PROPN` |
| 13 | Phrase | compiler source only | — |
| 14 | Abbreviation | compiler source only | — |
| 15 | Other | `PUNCT` | `PUNCT`, `SYM`, `X` |

The ZDL STTS tag is a fallback only when universal POS is absent. It also
preserves distinctions such as attributive determiners versus standalone
pronouns. Unknown tags map to `Unknown`; they are never guessed from spelling.

Auxiliary and modal verbs share canonical verb identity because dictionary
editions generally attach their definitions to verb lemmas rather than to a
separate syntactic AUX class. Noun and proper-noun candidates are compatible
for ranking but remain separate canonical IDs.

## Canonical features

Only features shared reliably by the two providers are retained:

- case: nominative, accusative, dative, genitive;
- degree: positive, comparative, superlative;
- gender: masculine, feminine, neuter;
- mood: indicative, subjunctive, imperative;
- number: singular, plural;
- person: first, second, third;
- tense: present, past, perfect;
- verb form: finite, infinitive, participle.

Provider-specific, unknown, underspecified, or multi-valued features are
omitted. Omission means “no evidence,” not disagreement. Features refine a
ranking but never create lexical identity.

## Scoring

Every DWDSmor candidate is mapped before scoring against the ZDL token:

| Evidence | Score |
| --- | ---: |
| Exact canonical POS | +600 |
| Noun/proper-noun compatible | +350 |
| One side has unknown POS | 0 |
| Conflicting known POS | −600 |
| Lemma agrees after NFC/case-folding | +400 |
| Lemma differs | −100 |
| Shared feature agrees | +30 each |
| Shared feature conflicts | −20 each |

The asymmetric lemma penalty is deliberate. The contextual model can emit a
bad lemma while still supplying useful POS evidence; for example, pinned ZDL
4.0.0 currently lemmatizes *mehr* as *sehr* in one reference sentence.
DWDSmor remains the authority for valid candidates, so contextual output only
ranks its analyses.

C08 will sort by score and deterministic canonical key, retain the top analysis,
and retain at most seven alternatives within 180 points of it. Ties are not
resolved by source lexeme ID. No score causes a valid morphology candidate to
be destructively deleted before the bounded-alternative policy runs.

## Versioning

Any change to numeric classes, mappings, normalized feature values, weights,
alternative cap, or score window increments `POLICY_VERSION`. The policy
version is recorded in compiler manifests and `language.bin`; changing it does
not silently reuse an existing compiled-book artifact.

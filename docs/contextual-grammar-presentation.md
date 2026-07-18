# Contextual Grammar Presentation Policy

This document freezes U02. It defines how grammar descriptor layout 1 and
canonical POS/headwords are presented on the device. It does not change binary
formats or input behavior.

All firmware-owned labels come from `tr(STR_*)`; canonical headwords and book
surfaces remain source content. Separators (` · `), divider rules, numerals and
the Unicode ellipsis are punctuation and may be fixed. Logs remain English-only
diagnostics.

## Definition header

Definition mode uses the existing bezel-safe left/right margins and reserves the
current title region above definition content:

```text
surface · primary lemma                         current/total
primary POS · grammatical explanation
```

- The title remains bold `UI_12_FONT_ID` at the current header position.
- The grammatical line uses `SMALL_FONT_ID`, regular weight, with an eight-pixel
  larger title-to-grammar baseline gap than the first hardware build and remains
  above definition content.
- U14 owns the right-aligned `current/total` counter. Title fitting reserves its
  measured width plus an eight-pixel gap.
- If surface and canonical lemma are byte-identical, the title contains one
  copy, as it does today.
- POS is displayed whenever the primary canonical record is available, even
  when the descriptor is zero.
- Descriptor zero therefore renders `Verb`, `Noun`, and so on rather than an
  empty or guessed grammatical explanation.
- Definition failure messages retain the title and available grammatical line.

The primary grammar line is prepared when a word opens, not during `render()`.
It lives in a fixed activity-owned UTF-8 buffer of at most 192 bytes. There is no
`String`, `std::string`, vector or per-render allocation.

## Canonical POS labels

U12 adds one translation key for every frozen canonical POS value. `UNKNOWN`
and `OTHER` share the translated presentation `Other`, but remain distinct
binary values.

| Canonical POS | English source label | German source label |
| --- | --- | --- |
| `UNKNOWN` | Other | Sonstiges |
| `NOUN` | Noun | Substantiv |
| `VERB` | Verb | Verb |
| `ADJECTIVE` | Adjective | Adjektiv |
| `ADVERB` | Adverb | Adverb |
| `PRONOUN` | Pronoun | Pronomen |
| `DETERMINER` | Determiner | Determinativ |
| `ADPOSITION` | Adposition | Adposition |
| `CONJUNCTION` | Conjunction | Konjunktion |
| `NUMERAL` | Numeral | Zahlwort |
| `PARTICLE` | Particle | Partikel |
| `INTERJECTION` | Interjection | Interjektion |
| `PROPER_NOUN` | Proper noun | Eigenname |
| `PHRASE` | Phrase | Phrase |
| `ABBREVIATION` | Abbreviation | Abkürzung |
| `OTHER` | Other | Sonstiges |

These are source strings, not text hardcoded in firmware. Translators may use
the conventional grammatical term for their language.

## Feature labels

Each nonzero descriptor value has one translation key:

| Field | English labels | German labels |
| --- | --- | --- |
| case | Nominative, Accusative, Dative, Genitive | Nominativ, Akkusativ, Dativ, Genitiv |
| degree | Positive, Comparative, Superlative | Positiv, Komparativ, Superlativ |
| gender | Masculine, Feminine, Neuter | Maskulinum, Femininum, Neutrum |
| mood | Indicative, Subjunctive, Imperative | Indikativ, Konjunktiv, Imperativ |
| number | Singular, Plural | Singular, Plural |
| person | First person, Second person, Third person | 1. Person, 2. Person, 3. Person |
| tense | Present, Past, Perfect | Präsens, Präteritum, Perfekt |
| verb form | Finite, Infinitive, Participle | Finit, Infinitiv, Partizip |

Six additional keys represent person+number combinations as one translated
unit, such as `Third person singular` / `3. Person Singular`. This avoids
assuming that every language places person and number in English order. If only
person or only number is available, the individual key is used.

English defines every key. German defines every key because this feature is
first qualified for German books. Other locale files may use the normal English
fallback until translated; firmware never bypasses `tr()`.

## Field order and omission

The renderer uses deterministic POS-aware ordering. It never changes canonical
identity or descriptor values.

### Finite verbs

```text
POS · tense · mood · person+number
```

`Finite` is omitted when any tense, mood, person or number is available because
those fields already establish a finite form. It is shown only when finite is
the sole grammatical evidence.

Example:

```text
Verb · Past · Indicative · Third person singular
Verb · Präteritum · Indikativ · 3. Person Singular
```

### Infinitives

```text
POS · Infinitive
```

The strict descriptor contract permits no other fields.

### Participles

```text
POS · Participle · tense · case · gender · number · degree
```

Unavailable fields disappear without placeholders.

### Nouns and proper nouns

```text
POS · case · gender · number
```

### Adjectives

```text
POS · case · gender · number · degree
```

### Pronouns, determiners and numerals

```text
POS · case · gender · person+number
```

### Adverbs

```text
POS · degree
```

### Other canonical classes

Use this generic order for any descriptor fields that survived strict host
validation:

```text
POS · case · gender · person+number · tense · mood · verb form · degree
```

Positive degree and indicative mood are not silently omitted. They are explicit
analyzer evidence and remain useful when distinguishing alternatives.

## Width fallback

The full translated line is attempted first in `SMALL_FONT_ID`. Width is
measured through `GfxRenderer::getTextAdvanceX()` before display.

If the full line exceeds the available content width, remove complete optional
components in this order until it fits:

1. positive degree;
2. gender;
3. indicative mood;
4. case;
5. standalone number;
6. standalone person;
7. non-positive degree;
8. non-indicative mood;
9. tense;
10. combined person+number;
11. nonfinite verb form.

POS is never removed. A component named in the POS-specific form as essential
is removed only when every earlier fallback still exceeds the width. In
practice the 480-pixel portrait width should retain normal German examples; the
fallback is a hard safety rule for long translations and custom fonts.

If POS alone exceeds the available width, truncate it at a UTF-8 boundary and
append `…`. The fitter operates in the fixed output buffer and removes complete
code points; it does not call the allocation-returning
`GfxRenderer::truncatedText()` from the render path.

The buffer-cap rule is identical: append complete translated components only.
If the next component would exceed 191 payload bytes, treat it as a width
failure and apply the same removal order. Never emit partial UTF-8.

## Alternative-analysis labels

The primary analysis is identified by the title and grammar line and receives
no duplicate divider. Every later canonical analysis is introduced once by a
labeled divider:

```text
────────  canonical lemma · POS  ────────
```

- Labels use `SMALL_FONT_ID`, bold.
- Divider height is 22 pixels, matching the existing source-divider height.
- Pagination reserves one visual divider row before definition content is
  appended. If the divider plus one content line cannot fit, both move to the
  next page.
- An alternative beginning at page zero of a new page still receives its label;
  an entry continuation does not repeat it.
- Reverse navigation reconstructs the same labels through bounded replay.
- Source labels remain separate. When an analysis and source begin together,
  the analysis label is rendered first and both reserved rows count.

The fixed analysis-label cache stores at most 48 UTF-8 headword bytes plus a
terminator and POS per analysis, for at most eight analyses. Loading truncates a
long canonical headword at a UTF-8 boundary and appends `…` within that cap.
Before drawing, pixel fitting preserves the translated POS suffix and further
truncates only the headword if required.

Rules flank the centered label only when at least eight pixels remain between
text and each rule. If not, draw the centered label without one or both rules;
never overlap text.

If an alternative canonical headword cannot be read, log the canonical read
failure and skip that source×analysis entry rather than drawing definitions
under a false or unlabeled identity. Other analyses and definition sources stay
usable.

## Pagination and memory

The grammar line and its hardware-qualified title gap reserve 68 pixels from
the bezel-safe top before definition content. Labeled alternative analysis
dividers replace the existing anonymous divider slot rather than adding a
second slot. The footer reserves enough height for one previous/next-word
preview line and one status/page-indicator line above the 40-pixel hint boxes.

The presentation budget is:

| Storage | Maximum | Lifetime |
| --- | ---: | --- |
| primary grammar UTF-8 line | 192 bytes | dictionary activity |
| eight cached display headwords + POS/state | ≤512 bytes | dictionary activity |
| previous/next word previews | 128 bytes | dictionary activity |
| formatter component metadata | ≤96 bytes | transient stack |

Formatting writes directly into the activity buffer and may reuse the existing
384-byte line scratch for divider text. It must not combine multiple
near-192-byte arrays in one stack frame. Both retained buffers are members of
the already heap-allocated activity and add no allocation churn or static DRAM.

## Required presentation tests

U12/U15 cover at least:

- `knipste · knipsen` with the complete finite-past German and English lines;
- descriptor zero showing POS only;
- finite-only, infinitive and participle forms;
- noun, adjective and pronoun ordering;
- every translation-key mapping;
- person+number combined and individual fallbacks;
- long translated labels and UTF-8 truncation without split code points;
- an alternative divider in the middle of a page and exactly at a page start;
- simultaneous analysis/source boundaries consuming two rows;
- reverse replay producing byte-identical labels;
- portrait, inverted and both landscape safe-area widths;
- canonical-label read failure remaining explicit and nonfatal.

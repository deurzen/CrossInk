#!/usr/bin/env python3
"""Convert a Wiktextract German JSONL dump into CrossInk dictionary source JSON.

Usage:
    python3 scripts/convert_wiktextract.py \
        ~/downloads/raw-wiktextract-data.jsonl \
        /tmp/german-wiktionary.json

Then compile the dictionary bundle:
    python3 scripts/build_dictionary_bundle.py /tmp/german-wiktionary.json /tmp/german-wiktionary.cpdict
"""

from __future__ import annotations

import json
import sys
import unicodedata
import uuid
from pathlib import Path
from typing import Any

# Map Wiktextract POS to CrossInk POS values
POS_MAP: dict[str, str] = {
    "noun": "noun",
    "verb": "verb",
    "adj": "adjective",
    "adv": "adverb",
    "pron": "pronoun",
    "det": "determiner",
    "prep": "preposition",
    "conj": "conjunction",
    "num": "numeral",
    "particle": "particle",
    "intj": "interjection",
    "name": "proper-noun",
    "phrase": "phrase",
    "abbrev": "abbreviation",
}

# Max bytes for headword — warn if exceeded
MAX_HEADWORD_BYTES = 96

# Wiktextract includes conjugation metadata in the same `forms` array as real
# surface inflections. These rows describe the lexeme or its table; they are not
# words that should resolve back to every lexeme carrying that metadata.
NON_SURFACE_FORM_TAGS = frozenset(
    {
        "abbreviation",
        "auxiliary",
        "class",
        "inflection-template",
        "table-tags",
    }
)


def normalize(text: str) -> str:
    return unicodedata.normalize("NFC", text)


def is_useful_form(form: str) -> bool:
    """Return True if form is a single-word surface usable for lookup.
    Filter out multi-word forms (geliebt haben, geliebt werden, etc.)
    and forms consisting entirely of punctuation."""
    stripped = form.strip().rstrip("!")
    if not stripped or not any(c.isalpha() or c == "-" or c == "'" for c in stripped):
        return False
    if " " in stripped:
        return False
    return True


def clean_form(form: str) -> str:
    """Return a lookup-ready form by removing trailing imperative marks."""
    return form.strip().rstrip("!").strip()


def extract_forms(entry: dict[str, Any]) -> set[str]:
    """Extract single-word surface forms from a Wiktextract entry's forms array."""
    forms: set[str] = set()
    for form_entry in entry.get("forms", []):
        tags = form_entry.get("tags", [])
        if isinstance(tags, list) and (
            NON_SURFACE_FORM_TAGS.intersection(tags)
            or any(isinstance(tag, str) and tag.startswith("error-") for tag in tags)
        ):
            continue
        raw = form_entry.get("form", "")
        text = normalize(clean_form(raw))
        if is_useful_form(text):
            forms.add(text)
    return forms


def extract_glosses(entry: dict[str, Any]) -> list[str]:
    """Extract definition text from senses."""
    glosses: list[str] = []
    for sense in entry.get("senses", []):
        for gloss in sense.get("glosses", []):
            text = normalize(gloss.strip())
            if text:
                glosses.append(text)
    return glosses


def convert(
    source_path: Path,
    output_path: Path,
    bundle_uuid: str | None = None,
    source_url: str | None = None,
    attribution: str | None = None,
) -> None:
    import hashlib

    if bundle_uuid is None:
        # Derive a UUID from the file content for stability
        bundle_uuid = str(uuid.uuid5(uuid.NAMESPACE_URL, str(source_path.resolve())))

    if source_url is None:
        source_url = "https://kaikki.org/dewiktionary/rawdata.html"
    if attribution is None:
        attribution = (
            "Derived from the German Wiktionary via Kaikki.org. "
            "Licensed under CC BY-SA 4.0. "
            "See https://creativecommons.org/licenses/by-sa/4.0/"
        )

    # Collect lexemes: key = (headword, crossink_pos) -> {
    #   headword, pos, forms: set[str], fields: set[(int, str)]
    # }
    lexemes: dict[tuple[str, str], dict[str, Any]] = {}
    total = 0
    skipped_no_def = 0
    skipped_punct = 0
    skipped_long = 0
    skipped_other = 0
    form_count = 0

    with open(source_path, encoding="utf-8") as infile:
        for lineno, line in enumerate(infile, 1):
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError:
                print(f"Warning: skipping malformed JSON at line {lineno}", file=sys.stderr)
                continue

            if entry.get("lang_code") != "de":
                continue

            word = normalize(entry.get("word", ""))
            if not word:
                continue

            # Only process lemmas (entries with definitions, not form_of entries)
            senses = entry.get("senses", [])
            has_form_of = any(s.get("form_of") for s in senses)
            if has_form_of:
                continue
            has_glosses = any(s.get("glosses") for s in senses)
            if not has_glosses:
                skipped_no_def += 1
                continue

            pos_raw = entry.get("pos", "unknown")
            crossink_pos = POS_MAP.get(pos_raw, "other")
            key = (word, crossink_pos)

            if key in lexemes:
                merged = lexemes[key]
            else:
                # Check headword byte length before creating
                word_bytes = word.encode("utf-8")
                if len(word_bytes) > MAX_HEADWORD_BYTES:
                    skipped_long += 1
                    continue
                merged = {
                    "headword": word,
                    "partOfSpeech": crossink_pos,
                    "forms": set(),
                    "fields": set(),
                }
                lexemes[key] = merged

            # Collect forms
            for form in extract_forms(entry):
                form_bytes = form.encode("utf-8")
                if len(form_bytes) <= MAX_HEADWORD_BYTES:
                    merged["forms"].add(form)
                    form_count += 1

            # Collect glosses (keep max 5 to limit output size)
            definition_texts = extract_glosses(entry)
            for text in definition_texts[:5]:
                merged["fields"].add((1, text))

            total += 1
            if lineno % 50000 == 0:
                print(f"  Processed {lineno} lines, {total} lemmas, {form_count} forms", flush=True)

    print(f"\nSummary:", flush=True)
    print(f"  Total lines read:      {lineno}", flush=True)
    print(f"  German lemmas:          {total}", flush=True)
    print(f"  Skipped (no glosses):   {skipped_no_def}", flush=True)
    print(f"  Skipped (too long):     {skipped_long}", flush=True)
    print(f"  Total surface forms:    {form_count}", flush=True)
    print(f"  Unique lexeme keys:     {len(lexemes)}", flush=True)

    # Build output JSON
    # License: CC BY-SA 4.0 for Wiktionary-derived content
    source_data = {
        "bundleUuid": bundle_uuid,
        "sourceLanguage": "de",
        "targetLanguage": "de",
        "license": {
            "spdx": "CC-BY-SA-4.0",
            "sourceUrl": source_url,
            "attribution": attribution,
        },
        "lexemes": [],
    }

    # Sort by headword for deterministic output
    sorted_keys = sorted(lexemes, key=lambda k: (k[0], k[1]))
    for headword, pos in sorted_keys:
        entry = lexemes[(headword, pos)]
        # Build forms list (sorted, deduped)
        forms = sorted(entry["forms"])
        # Build fields list (sorted by type then text)
        fields = sorted(
            ({"type": "definition", "text": text} for type_id, text in entry["fields"]),
            key=lambda f: f["text"],
        )
        source_data["lexemes"].append({
            "headword": entry["headword"],
            "partOfSpeech": entry["partOfSpeech"],
            "forms": forms,
            "fields": fields,
        })

    # Write output
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_suffix(".json.tmp")
    with open(temporary, "w", encoding="utf-8") as outfile:
        json.dump(source_data, outfile, ensure_ascii=False, indent=2, sort_keys=True)
    temporary.replace(output_path)

    output_mb = output_path.stat().st_size / 1024 / 1024
    print(f"\nWrote {output_path} ({output_mb:.1f} MB)", flush=True)
    print(f"  Lexemes: {len(source_data['lexemes'])}", flush=True)
    print(f"  Bundle UUID: {bundle_uuid}", flush=True)
    print(f"\nNext step:", flush=True)
    print(f"  python3 scripts/build_dictionary_bundle.py {output_path} /tmp/german-wiktionary.cpdict", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Wiktextract JSONL file (raw-wiktextract-data.jsonl)")
    parser.add_argument("output", type=Path, help="Output CrossInk dictionary JSON")
    parser.add_argument("--uuid", help="Override bundle UUID (default: derived from source path)")
    parser.add_argument("--source-url", help="Dictionary source URL (default: Kaikki.org)")
    parser.add_argument("--attribution", help="Attribution text (default: Kaikki/Wiktionary CC BY-SA)")
    args = parser.parse_args()

    if not args.source.is_file():
        print(f"Source file not found: {args.source}", file=sys.stderr)
        return 2

    convert(args.source, args.output, args.uuid, args.source_url, args.attribution)
    return 0


if __name__ == "__main__":
    import argparse
    raise SystemExit(main())

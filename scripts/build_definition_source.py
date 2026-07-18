#!/usr/bin/env python3
"""Build a deterministic .cpdef bundle from canonical-keyed JSON."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.analysis_policy import CanonicalPos  # noqa: E402
from dictionary.contextual.canonical_lexicon import (  # noqa: E402
    CanonicalLexiconError,
    load_canonical_lexicon_index,
)
from dictionary.contextual.definition_source import (  # noqa: E402
    DefinitionEntryInput,
    DefinitionFieldInput,
    DefinitionSourceError,
    compile_definition_source,
)

POS = {value.name.lower().replace("_", "-"): value for value in CanonicalPos}
FIELDS = {
    "definition": 1,
    "part-of-speech": 2,
    "example": 3,
    "usage": 4,
    "etymology": 5,
    "cross-reference": 6,
    "compound-component": 7,
}


def parse_source(path: Path):
    try:
        source = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise DefinitionSourceError(f"cannot read definition source: {error}") from error
    if not isinstance(source, dict) or source.get("schemaVersion") != 1:
        raise DefinitionSourceError("unsupported definition source schema")
    raw_entries = source.get("entries")
    if not isinstance(raw_entries, list):
        raise DefinitionSourceError("entries must be an array")
    entries = []
    for index, raw in enumerate(raw_entries):
        if not isinstance(raw, dict):
            raise DefinitionSourceError(f"entry {index} must be an object")
        pos = POS.get(raw.get("partOfSpeech"))
        if pos is None:
            raise DefinitionSourceError(f"entry {index} has unsupported part of speech")
        raw_fields = raw.get("fields")
        if not isinstance(raw_fields, list):
            raise DefinitionSourceError(f"entry {index} fields must be an array")
        fields = []
        for field_index, field in enumerate(raw_fields):
            if not isinstance(field, dict) or field.get("type") not in FIELDS:
                raise DefinitionSourceError(
                    f"entry {index} field {field_index} has unsupported type"
                )
            fields.append(
                DefinitionFieldInput(FIELDS[field["type"]], field.get("text"))
            )
        entries.append(DefinitionEntryInput(raw.get("headword"), pos, tuple(fields)))
    license_data = source.get("license")
    if not isinstance(license_data, dict):
        raise DefinitionSourceError("license must be an object")
    for key in ("spdx", "sourceUrl", "attribution"):
        if not isinstance(license_data.get(key), str) or not license_data[key].strip():
            raise DefinitionSourceError(f"license.{key} must be non-empty text")
    license_text = (
        f"SPDX-License-Identifier: {license_data['spdx']}\n"
        f"Source: {license_data['sourceUrl']}\n\n"
        f"{license_data['attribution'].strip()}\n"
    )
    return source, tuple(entries), license_text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--canonical", required=True, type=Path)
    args = parser.parse_args()
    try:
        source, entries, license_text = parse_source(args.source)
        canonical = load_canonical_lexicon_index(args.canonical)
        compiled = compile_definition_source(
            canonical,
            entries,
            source.get("sourceLanguage"),
            source.get("targetLanguage"),
            source.get("sourceLabel"),
            license_text,
            {
                "inputSha256": hashlib.sha256(args.source.read_bytes()).hexdigest(),
                "license": source["license"],
            },
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.output.with_name(args.output.name + ".tmp")
        try:
            with temporary.open("wb") as output:
                output.write(compiled.archive_bytes)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, args.output)
        finally:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass
    except (CanonicalLexiconError, DefinitionSourceError, OSError) as error:
        print(f"definition source build failed: {error}", file=sys.stderr)
        return 1
    print(
        json.dumps(
            {
                "coverageCount": compiled.coverage_count,
                "output": str(args.output),
                "sourceUuid": str(compiled.source_uuid),
                "unmatchedCount": compiled.unmatched_count,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

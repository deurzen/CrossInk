#!/usr/bin/env python3
"""Stream Kaikki German JSONL into a canonical de-en .cpdef bundle."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.canonical_lexicon import (  # noqa: E402
    CanonicalLexiconError,
    load_canonical_lexicon_index,
)
from dictionary.contextual.definition_source import DefinitionSourceError  # noqa: E402
from dictionary.contextual.kaikki_definitions import (  # noqa: E402
    KaikkiError,
    compile_kaikki_definition_source,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--canonical", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        canonical = load_canonical_lexicon_index(args.canonical)
        compiled, stats = compile_kaikki_definition_source(args.source, canonical)
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
    except (CanonicalLexiconError, DefinitionSourceError, KaikkiError, OSError) as error:
        print(f"Kaikki definition build failed: {error}", file=sys.stderr)
        return 1
    print(
        json.dumps(
            {
                "coverageCount": compiled.coverage_count,
                "import": stats.as_dict(),
                "output": str(args.output),
                "sourceUuid": str(compiled.source_uuid),
                "truncatedFieldCount": compiled.truncated_field_count,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

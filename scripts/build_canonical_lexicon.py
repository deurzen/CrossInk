#!/usr/bin/env python3
"""Build a deterministic canonical German .cplex bundle."""

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
    compile_de_de_canonical_bundle,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--de-de-bundle", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    try:
        bundle = compile_de_de_canonical_bundle(args.de_de_bundle)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.output.with_name(args.output.name + ".tmp")
        try:
            with temporary.open("wb") as output:
                output.write(bundle.archive_bytes)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, args.output)
        finally:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass
    except (CanonicalLexiconError, OSError) as error:
        print(f"canonical lexicon build failed: {error}", file=sys.stderr)
        return 1

    print(
        json.dumps(
            {
                "canonicalUuid": str(bundle.canonical_uuid),
                "lexemeCount": bundle.lexeme_count,
                "output": str(args.output),
                "payloadSha256": bundle.payload_sha256,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

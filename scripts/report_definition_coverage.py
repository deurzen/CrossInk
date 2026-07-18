#!/usr/bin/env python3
"""Validate and report canonical coverage across definition sources."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.canonical_lexicon import (  # noqa: E402
    CanonicalLexiconError,
    load_canonical_lexicon_index,
)
from dictionary.contextual.coverage_report import (  # noqa: E402
    CoverageReportError,
    build_coverage_report,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--canonical", required=True, type=Path)
    parser.add_argument("--source", action="append", required=True, metavar="NAME=PATH")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    sources = {}
    for value in args.source:
        name, separator, path = value.partition("=")
        if not separator or not name or name in sources:
            parser.error("each --source must be a unique NAME=PATH")
        sources[name] = Path(path)
    try:
        canonical = load_canonical_lexicon_index(args.canonical)
        report = build_coverage_report(canonical, sources)
        encoded = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    except (CanonicalLexiconError, CoverageReportError, OSError) as error:
        print(f"definition coverage report failed: {error}", file=sys.stderr)
        return 1
    print(
        json.dumps(
            {
                "output": str(args.output),
                "sources": len(report["sources"]),
                "unionCoverage": report["union"]["covered"],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

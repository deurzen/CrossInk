#!/usr/bin/env python3
"""Compile a CrossInk dictionary source JSON file into a .cpdict bundle."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from dictionary.compiler import CompileError, FrequencyProvider, compile_file


def load_wordfreq_provider() -> FrequencyProvider:
    checkout = Path(__file__).resolve().parents[1] / "wordfreq"
    if not (checkout / "wordfreq" / "__init__.py").is_file():
        raise CompileError("wordfreq submodule is missing; run git submodule update --init wordfreq")
    sys.path.insert(0, str(checkout))
    try:
        from wordfreq import zipf_frequency
    except ImportError as exc:
        raise CompileError(f"wordfreq dependencies are unavailable: {exc}") from exc

    def score(surface: str, language: str) -> float:
        primary_language = language.split("-", 1)[0].lower()
        try:
            return zipf_frequency(surface, primary_language, wordlist="best")
        except (LookupError, ValueError):
            return 0.0

    score.provider_id = "wordfreq-v3.2"  # type: ignore[attr-defined]
    score.license_text = (  # type: ignore[attr-defined]
        "wordfreq data attribution\n\n"
        + (checkout / "NOTICE.md").read_text(encoding="utf-8")
        + "\n\nApache-2.0 license for wordfreq code\n\n"
        + (checkout / "LICENSE.txt").read_text(encoding="utf-8")
    )
    return score


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="dictionary source JSON")
    parser.add_argument("output", type=Path, help="output .cpdict path")
    parser.add_argument("--frequency-ranking", choices=("none", "wordfreq"), default="wordfreq",
                        help="offline familiarity provider embedded for book shortlist ranking")
    args = parser.parse_args()

    try:
        provider = load_wordfreq_provider() if args.frequency_ranking == "wordfreq" else None
        compile_file(args.source, args.output, provider)
    except CompileError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

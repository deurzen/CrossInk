#!/usr/bin/env python3
"""Compile a CrossInk dictionary source JSON file into a .cpdict bundle."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from dictionary.compiler import CompileError, compile_file


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="dictionary source JSON")
    parser.add_argument("output", type=Path, help="output .cpdict path")
    args = parser.parse_args()

    try:
        compile_file(args.source, args.output)
    except CompileError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Compile XHTML spine files into a CrossInk book-language artifact."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import sys
import tempfile
import zipfile

from dictionary.book_compiler import BookCompileError, compile_book, load_compiler_dictionary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dictionary", type=Path, help="source .cpdict bundle")
    parser.add_argument("output", type=Path, help="output directory")
    parser.add_argument("spines", type=Path, nargs="+", help="XHTML spine files in reading order")
    args = parser.parse_args()

    try:
        with zipfile.ZipFile(args.dictionary) as archive:
            dictionary = load_compiler_dictionary(
                archive.read("device/meta.bin"), archive.read("compiler/forms.bin")
            )
        xhtml_spines = [path.read_text(encoding="utf-8") for path in args.spines]
        compiled = compile_book(xhtml_spines, dictionary)
    except (OSError, KeyError, UnicodeError, zipfile.BadZipFile, BookCompileError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=args.output.name + ".tmp-", dir=args.output.parent))
    try:
        (temporary / "language.bin").write_bytes(compiled.language_artifact)
        for index, xhtml in enumerate(compiled.xhtml_spines):
            (temporary / f"{index:04d}.xhtml").write_text(xhtml, encoding="utf-8")
        if args.output.exists():
            shutil.rmtree(args.output)
        temporary.replace(args.output)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    print(f"Wrote {args.output / 'language.bin'} and {len(compiled.xhtml_spines)} marked spine(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

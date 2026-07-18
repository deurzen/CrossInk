#!/usr/bin/env python3
"""Build and run the simulator smoke test against an isolated fs_ directory."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / ".pio" / "build" / "simulator" / "program"
DEFAULT_BOOK = ROOT / "test" / "epubs" / "test_reader_rendering_matrix.epub"
CRASH_PATTERNS = (
    "std::bad_alloc",
    "terminating due to uncaught exception",
    "Assertion failed",
    "Segmentation fault",
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
)
THEMES = {
    "classic": 0,
    "lyra": 1,
    "lyra-extended": 2,
    "lyra_extended": 2,
    "lyra3": 2,
    "lyra-3-covers": 2,
    "roundedraff": 3,
    "rounded-raff": 3,
    "lyra-carousel": 4,
    "lyra_carousel": 4,
    "carousel": 4,
}


def build_simulator() -> None:
    print("Building simulator...", flush=True)
    proc = subprocess.run(["pio", "run", "-e", "simulator"], cwd=ROOT)
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)


def prepare_fs(temp_root: Path, book: Path) -> str:
    books_dir = temp_root / "fs_" / "books"
    books_dir.mkdir(parents=True, exist_ok=True)

    target = books_dir / book.name
    shutil.copy2(book, target)
    return f"/books/{book.name}"


def install_dictionary_fixture(temp_root: Path, device_dir: Path) -> None:
    required = ("meta.bin", "lexemes.bin", "headwords.bin", "entries.bin")
    for name in required:
        if not (device_dir / name).is_file():
            raise ValueError(f"dictionary fixture is missing {name}: {device_dir}")

    metadata = (device_dir / "meta.bin").read_bytes()
    if len(metadata) != 80 or metadata[:4] != b"CXDM" or metadata[12:28] == bytes(16):
        raise ValueError(f"dictionary fixture has invalid meta.bin: {device_dir}")

    target = temp_root / "fs_" / ".crosspoint" / "dictionaries" / metadata[12:28].hex()
    target.mkdir(parents=True, exist_ok=True)
    for name in required:
        shutil.copy2(device_dir / name, target / name)


def run_smoke(args: argparse.Namespace) -> int:
    book = Path(args.book).resolve()
    if not book.exists():
        print(f"Smoke test book not found: {book}", file=sys.stderr)
        return 2

    if args.build:
        build_simulator()

    if not PROGRAM.exists():
        print(f"Simulator binary not found: {PROGRAM}", file=sys.stderr)
        print("Run: pio run -e simulator", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="crossink-sim-smoke-") as temp_dir_name:
        temp_root = Path(temp_dir_name)
        simulator_book_path = prepare_fs(temp_root, book)
        if args.dictionary_device_dir:
            try:
                install_dictionary_fixture(temp_root, Path(args.dictionary_device_dir).resolve())
            except ValueError as error:
                print(error, file=sys.stderr)
                return 2

        env = os.environ.copy()
        env["CROSSINK_SIMULATOR_SMOKE_TEST"] = "1"
        env["CROSSINK_SIMULATOR_SMOKE_BOOK"] = simulator_book_path
        env["CROSSINK_SIMULATOR_SMOKE_PAGE_TURNS"] = str(args.page_turns)
        if args.dictionary_device_dir:
            env["CROSSINK_SIMULATOR_SMOKE_DICTIONARY"] = "1"
        if args.theme:
            env["CROSSINK_SIMULATOR_SMOKE_THEME"] = str(THEMES[args.theme])
        if args.headless:
            env.setdefault("SDL_VIDEODRIVER", "dummy")

        print(f"Running simulator smoke test with isolated fs_: {temp_root / 'fs_'}", flush=True)
        proc = subprocess.run(
            [str(PROGRAM)],
            cwd=temp_root,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=args.timeout,
        )

    print(proc.stdout, end="")

    if proc.returncode != 0:
        print(f"Simulator smoke test failed with exit code {proc.returncode}", file=sys.stderr)
        return proc.returncode

    for pattern in CRASH_PATTERNS:
        if pattern in proc.stdout:
            print(f"Simulator smoke test output contained crash pattern: {pattern}", file=sys.stderr)
            return 2

    if "Simulator smoke test passed" not in proc.stdout:
        print("Simulator smoke test did not print its success marker", file=sys.stderr)
        return 2

    if "Validated dictionary input trace across 4 orientations" not in proc.stdout:
        print("Simulator smoke test did not validate dictionary input orientations", file=sys.stderr)
        return 2

    if "Saved word inbox context 1" not in proc.stdout:
        print("Simulator smoke test did not persist a Word Inbox context", file=sys.stderr)
        return 2

    if args.dictionary_device_dir and "Entering activity: Dictionary" not in proc.stdout:
        print("Simulator smoke test did not open the Dictionary activity", file=sys.stderr)
        return 2

    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--book", default=str(DEFAULT_BOOK), help="EPUB fixture to copy into the isolated simulator fs_")
    parser.add_argument("--timeout", type=int, default=45, help="Seconds before the simulator run is treated as hung")
    parser.add_argument("--page-turns", type=int, default=2, help="Number of EPUB page-forward taps to run")
    parser.add_argument("--theme", choices=sorted(THEMES), help="UI theme to use during the smoke test")
    parser.add_argument(
        "--dictionary-device-dir",
        help="Install a matching compiled device/ directory and exercise dictionary shortlist/definition UI",
    )
    parser.add_argument("--no-build", dest="build", action="store_false", help="Run the existing simulator binary")
    parser.add_argument("--window", dest="headless", action="store_false", help="Show the SDL window instead of using dummy video")
    parser.set_defaults(build=True, headless=True)
    return parser.parse_args()


def main() -> int:
    return run_smoke(parse_args())


if __name__ == "__main__":
    raise SystemExit(main())

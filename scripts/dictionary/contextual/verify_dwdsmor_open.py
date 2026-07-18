#!/usr/bin/env python3
"""Verify the pinned DWDSmor Open Edition and required sample analyses."""

from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.metadata
import json
from pathlib import Path
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_MANIFEST = Path(__file__).with_name("dwdsmor-open.json")


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as source:
        value = json.load(source)
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def verify_distribution_files(distribution_name: str) -> tuple[int, int]:
    distribution = importlib.metadata.distribution(distribution_name)
    verified_files = 0
    verified_bytes = 0
    for package_path in distribution.files or ():
        expected = package_path.hash
        if expected is None:
            continue
        path = Path(package_path.locate())
        digest = hashlib.new(expected.mode)
        size = 0
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
        actual = base64.urlsafe_b64encode(digest.digest()).rstrip(b"=").decode("ascii")
        if actual != expected.value:
            raise RuntimeError(f"installed package file digest mismatch: {package_path}")
        verified_files += 1
        verified_bytes += size
    if verified_files == 0:
        raise RuntimeError(f"{distribution_name}: no wheel RECORD digests found")
    return verified_files, verified_bytes


def verify_automata(automata_dir: Path, expected_files: dict[str, Any]) -> int:
    actual_names = {path.name for path in automata_dir.iterdir() if path.is_file()}
    if actual_names != set(expected_files):
        raise RuntimeError(
            f"unexpected automata files: found {sorted(actual_names)}, "
            f"expected {sorted(expected_files)}"
        )
    verified_bytes = 0
    for name, expected in expected_files.items():
        path = automata_dir / name
        size = path.stat().st_size
        if size != expected["bytes"]:
            raise RuntimeError(f"{name}: expected {expected['bytes']} bytes, found {size}")
        actual_sha256 = file_sha256(path)
        if actual_sha256 != expected["sha256"]:
            raise RuntimeError(f"{name}: SHA-256 mismatch")
        verified_bytes += size
    return verified_bytes


def verify_reference(analyzer: Any, fixture_path: Path) -> tuple[int, int]:
    fixture = read_json(fixture_path)
    if fixture.get("schemaVersion") != 1:
        raise ValueError(f"{fixture_path}: unsupported schema version")
    cases = fixture.get("cases")
    if not isinstance(cases, list) or not cases:
        raise ValueError(f"{fixture_path}: missing analysis cases")

    traversal_count = 0
    for case_index, case in enumerate(cases):
        surface = case.get("surface")
        required = case.get("required")
        if not isinstance(surface, str) or not isinstance(required, list):
            raise ValueError(f"{fixture_path}: invalid case {case_index}")
        analyses = tuple(analyzer.analyze(surface))
        traversal_count += len(analyses)
        actual = {(analysis.analysis, analysis.pos) for analysis in analyses}
        expected = {(item["lemma"], item["pos"]) for item in required}
        missing = expected - actual
        if missing:
            raise RuntimeError(
                f"{surface}: missing required analyses {sorted(missing)}; "
                f"found {sorted(actual)}"
            )
    return len(cases), traversal_count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument(
        "--skip-distribution-digests",
        action="store_true",
        help="skip verification against the installed wheel RECORD",
    )
    args = parser.parse_args()

    manifest = read_json(args.manifest)
    expected_python = manifest["pythonMinor"]
    actual_python = f"{sys.version_info.major}.{sys.version_info.minor}"
    if actual_python != expected_python:
        raise RuntimeError(f"CPython {expected_python} required, found {actual_python}")

    package = manifest["package"]
    distribution_name = package["distribution"]
    actual_version = importlib.metadata.version(distribution_name)
    if actual_version != package["version"]:
        raise RuntimeError(
            f"{distribution_name} {package['version']} required, found {actual_version}"
        )

    import dwdsmor

    if dwdsmor.edition.strip() != manifest["edition"]:
        raise RuntimeError(
            f"DWDSmor edition {manifest['edition']} required, found {dwdsmor.edition!r}"
        )
    automata_dir = Path(dwdsmor.default_automata_dir)
    if (automata_dir / "BUILT").read_text(encoding="utf-8") != manifest["built"]:
        raise RuntimeError("DWDSmor automata build timestamp mismatch")
    if (automata_dir / "GIT_REV").read_text(encoding="utf-8") != manifest["upstreamGitRevision"]:
        raise RuntimeError("DWDSmor automata Git revision mismatch")

    automata_bytes = verify_automata(automata_dir, manifest["automata"])
    case_count, traversal_count = verify_reference(
        dwdsmor.analyzer(automaton_type="lemma"),
        ROOT / manifest["referenceFixture"],
    )

    distribution_files = 0
    distribution_bytes = 0
    if not args.skip_distribution_digests:
        distribution_files, distribution_bytes = verify_distribution_files(distribution_name)

    print(
        json.dumps(
            {
                "analysisCases": case_count,
                "analysisTraversals": traversal_count,
                "automataBytes": automata_bytes,
                "distributionBytes": distribution_bytes,
                "distributionFiles": distribution_files,
                "edition": dwdsmor.edition.strip(),
                "package": f"{distribution_name}=={actual_version}",
                "python": actual_python,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

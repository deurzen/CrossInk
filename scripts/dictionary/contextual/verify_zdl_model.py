#!/usr/bin/env python3
"""Verify the pinned ZDL contextual model and its reference inference."""

from __future__ import annotations

import argparse
import base64
import hashlib
import importlib
import importlib.metadata
import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_MANIFEST = Path(__file__).with_name("zdl-model.json")


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as source:
        value = json.load(source)
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def verify_distribution_files(distribution_name: str) -> tuple[int, int]:
    """Verify every installed file carrying a wheel RECORD digest."""
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
            raise RuntimeError(f"installed model file digest mismatch: {package_path}")
        verified_files += 1
        verified_bytes += size
    if verified_files == 0:
        raise RuntimeError(f"{distribution_name}: no wheel RECORD digests found")
    return verified_files, verified_bytes


def token_record(token: Any) -> dict[str, Any]:
    return {
        "lemma": token.lemma_,
        "pos": token.pos_,
        "start": token.idx,
        "tag": token.tag_,
        "text": token.text,
    }


def verify_reference(nlp: Any, fixture_path: Path) -> int:
    fixture = read_json(fixture_path)
    if fixture.get("schemaVersion") != 1:
        raise ValueError(f"{fixture_path}: unsupported schema version")
    sentences = fixture.get("sentences")
    if not isinstance(sentences, list) or not sentences:
        raise ValueError(f"{fixture_path}: missing reference sentences")

    for sentence_index, reference in enumerate(sentences):
        text = reference.get("text")
        expected_tokens = reference.get("tokens")
        if not isinstance(text, str) or not isinstance(expected_tokens, list):
            raise ValueError(f"{fixture_path}: invalid sentence {sentence_index}")
        actual_tokens = [token_record(token) for token in nlp(text)]
        if actual_tokens != expected_tokens:
            expected_json = json.dumps(expected_tokens, ensure_ascii=False, sort_keys=True)
            actual_json = json.dumps(actual_tokens, ensure_ascii=False, sort_keys=True)
            raise RuntimeError(
                f"reference inference changed for sentence {sentence_index}\n"
                f"expected: {expected_json}\nactual:   {actual_json}"
            )
    return len(sentences)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument(
        "--skip-file-digests",
        action="store_true",
        help="skip verification of installed files against wheel RECORD",
    )
    args = parser.parse_args()

    manifest = read_json(args.manifest)
    expected_python = manifest["pythonMinor"]
    actual_python = f"{sys.version_info.major}.{sys.version_info.minor}"
    if actual_python != expected_python:
        raise RuntimeError(f"CPython {expected_python} required, found {actual_python}")

    model = manifest["model"]
    distribution_name = model["distribution"]
    expected_model_version = model["version"]
    actual_model_version = importlib.metadata.version(distribution_name)
    if actual_model_version != expected_model_version:
        raise RuntimeError(
            f"{distribution_name} {expected_model_version} required, "
            f"found {actual_model_version}"
        )

    expected_spacy_version = manifest["spacyVersion"]
    actual_spacy_version = importlib.metadata.version("spacy")
    if actual_spacy_version != expected_spacy_version:
        raise RuntimeError(
            f"spaCy {expected_spacy_version} required, found {actual_spacy_version}"
        )

    import spacy

    enabled_components = manifest["enabledComponents"]
    excluded_components = manifest["excludedComponents"]
    model_module = importlib.import_module(model["module"])
    model_meta = read_json(Path(model_module.__file__).parent / "meta.json")
    declared_components = set(model_meta.get("pipeline", ()))
    expected_components = set(enabled_components) | set(excluded_components)
    if declared_components != expected_components:
        raise RuntimeError(
            f"unexpected model components: {sorted(declared_components)}; "
            f"expected {sorted(expected_components)}"
        )
    nlp = spacy.load(model["module"], exclude=excluded_components)
    if nlp.pipe_names != enabled_components:
        raise RuntimeError(
            f"unexpected enabled pipeline: {nlp.pipe_names}; expected {enabled_components}"
        )

    fixture_path = ROOT / manifest["referenceFixture"]
    sentence_count = verify_reference(nlp, fixture_path)

    verified_files = 0
    verified_bytes = 0
    if not args.skip_file_digests:
        verified_files, verified_bytes = verify_distribution_files(distribution_name)

    print(
        json.dumps(
            {
                "model": f"{distribution_name}=={actual_model_version}",
                "python": actual_python,
                "referenceSentences": sentence_count,
                "spacy": actual_spacy_version,
                "verifiedBytes": verified_bytes,
                "verifiedFiles": verified_files,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

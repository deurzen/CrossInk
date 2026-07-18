#!/usr/bin/env python3
"""Benchmark pinned static and transformer ZDL models in isolated processes."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import resource
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.corpus import evaluate_corpus, load_corpus  # noqa: E402
from dictionary.contextual.dwdsmor_adapter import DwdsmorMorphologyAnalyzer  # noqa: E402
from dictionary.contextual.fusion import GermanAnalysisFuser  # noqa: E402
from dictionary.contextual.zdl_adapter import ZdlContextAnalyzer  # noqa: E402

CONTEXTUAL = ROOT / "scripts" / "dictionary" / "contextual"
DEFAULT_CORPUS = ROOT / "test" / "data" / "contextual" / "german-ambiguity-corpus.json"
DEFAULT_MANIFESTS = (
    CONTEXTUAL / "zdl-model.json",
    CONTEXTUAL / "zdl-dist-model.json",
)


def peak_rss_bytes() -> int:
    value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return value if platform.system() == "Darwin" else value * 1024


def run_worker(manifest_path: Path, corpus_path: Path, repeats: int) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    cases = load_corpus(corpus_path)

    load_start = time.perf_counter()
    context = ZdlContextAnalyzer.from_pinned_model(manifest_path=manifest_path)
    morphology = DwdsmorMorphologyAnalyzer.from_open_edition()
    load_seconds = time.perf_counter() - load_start

    inference_times = []
    evaluations = []
    for _ in range(repeats):
        inference_start = time.perf_counter()
        evaluation = evaluate_corpus(cases, context, morphology, GermanAnalysisFuser())
        inference_times.append(time.perf_counter() - inference_start)
        evaluations.append(evaluation)
    summaries = [evaluation.summary() for evaluation in evaluations]
    if any(summary != summaries[0] for summary in summaries[1:]):
        raise RuntimeError("model benchmark produced nondeterministic accuracy")

    warm_times = inference_times[1:]
    model = manifest["model"]
    return {
        "distribution": model["distribution"],
        "version": model["version"],
        "wheelBytes": model["wheelBytes"],
        "loadSeconds": round(load_seconds, 6),
        "coldCorpusSeconds": round(inference_times[0], 6),
        "warmCorpusMeanSeconds": round(
            sum(warm_times) / len(warm_times) if warm_times else inference_times[0],
            6,
        ),
        "peakRssBytes": peak_rss_bytes(),
        "accuracy": summaries[0],
    }


def run_parent(
    manifests: list[Path],
    pythons: list[Path],
    corpus: Path,
    repeats: int,
) -> dict:
    results = []
    for manifest, python in zip(manifests, pythons):
        command = [
            str(python),
            str(Path(__file__).resolve()),
            "--worker",
            "--manifest",
            str(manifest),
            "--corpus",
            str(corpus),
            "--repeats",
            str(repeats),
        ]
        completed = subprocess.run(command, check=True, capture_output=True, text=True)
        results.append(json.loads(completed.stdout))
    return {
        "schemaVersion": 1,
        "host": {
            "machine": platform.machine(),
            "platform": platform.platform(),
            "processor": platform.processor(),
            "python": platform.python_version(),
            "cpuCount": os.cpu_count(),
        },
        "corpus": str(corpus.relative_to(ROOT)),
        "repeats": repeats,
        "models": results,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    parser.add_argument("--manifest", action="append", type=Path)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument(
        "--python",
        action="append",
        type=Path,
        help="interpreter for each manifest, in manifest order",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.repeats < 1 or args.repeats > 20:
        parser.error("--repeats must be between 1 and 20")

    manifests = args.manifest or list(DEFAULT_MANIFESTS)
    pythons = args.python or [Path(sys.executable)] * len(manifests)
    if len(pythons) != len(manifests):
        parser.error("provide one --python per model manifest")
    if args.worker:
        if len(manifests) != 1:
            parser.error("worker requires exactly one --manifest")
        report = run_worker(manifests[0], args.corpus, args.repeats)
    else:
        report = run_parent(manifests, pythons, args.corpus, args.repeats)
    encoded = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

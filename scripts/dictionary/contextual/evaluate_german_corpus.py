#!/usr/bin/env python3
"""Evaluate the pinned German providers against the ambiguity corpus."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.corpus import evaluate_corpus, load_corpus  # noqa: E402
from dictionary.contextual.dwdsmor_adapter import DwdsmorMorphologyAnalyzer  # noqa: E402
from dictionary.contextual.form_inventory import (  # noqa: E402
    AugmentedMorphologyAnalyzer,
    DeDeFormInventoryAnalyzer,
)
from dictionary.contextual.fusion import GermanAnalysisFuser  # noqa: E402
from dictionary.contextual.separable_verbs import GermanSeparableVerbRecombiner  # noqa: E402
from dictionary.contextual.zdl_adapter import ZdlContextAnalyzer  # noqa: E402

DEFAULT_CORPUS = ROOT / "test" / "data" / "contextual" / "german-ambiguity-corpus.json"


def analysis_json(analysis):
    return {
        "lemma": analysis.lemma,
        "pos": analysis.part_of_speech.name.lower().replace("_", "-"),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--form-inventory",
        type=Path,
        help="verified de-DE .cpdict compiler inventory to augment DWDSmor",
    )
    parser.add_argument("--require-perfect", action="store_true")
    args = parser.parse_args()

    morphology = DwdsmorMorphologyAnalyzer.from_open_edition()
    inventory = None
    if args.form_inventory:
        inventory = DeDeFormInventoryAnalyzer.from_cpdict(args.form_inventory)
        morphology = AugmentedMorphologyAnalyzer(morphology, inventory)
    evaluation = evaluate_corpus(
        load_corpus(args.corpus),
        ZdlContextAnalyzer.from_pinned_model(),
        morphology,
        GermanAnalysisFuser(),
        GermanSeparableVerbRecombiner(morphology),
    )
    report = {
        "schemaVersion": 1,
        "providers": {
            "dwdsmor": "0.18.0-open",
            "analysisPolicy": 2,
            "spacy": "3.8.14",
            "zdl": "de-zdl-lg-4.0.0",
            "formInventory": (
                {
                    "archiveSha256": inventory.identity.source_sha256,
                    "bundleUuid": inventory.identity.bundle_uuid.hex(),
                    "lexemeCount": inventory.identity.lexeme_count,
                    "licenseSpdx": inventory.identity.license_spdx,
                }
                if inventory is not None
                else None
            ),
        },
        "summary": evaluation.summary(),
        "cases": [
            {
                "id": case.case_id,
                "category": case.category,
                "expected": analysis_json(case.expected),
                "context": analysis_json(case.context),
                "morphology": [analysis_json(item) for item in case.morphology],
                "fused": [analysis_json(item) for item in case.fused],
                "contextCorrect": case.context_correct,
                "morphologyCovered": case.morphology_covered,
                "fusedPrimaryCorrect": case.fused_primary_correct,
            }
            for case in evaluation.cases
        ],
    }
    encoded = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    if args.require_perfect and evaluation.fused_primary_correct != len(evaluation.cases):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

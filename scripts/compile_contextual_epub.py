#!/usr/bin/env python3
"""Compile an EPUB with pinned German contextual analysis and language.bin v4."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from build_dictionary_bundle import load_wordfreq_provider  # noqa: E402
from dictionary.contextual.canonical_lexicon import (  # noqa: E402
    CanonicalLexiconError,
    load_canonical_lexicon_index,
)
from dictionary.contextual.dwdsmor_adapter import (  # noqa: E402
    DwdsmorAdapterError,
    DwdsmorMorphologyAnalyzer,
)
from dictionary.contextual.epub_compiler import (  # noqa: E402
    ContextualEpubError,
    compile_contextual_epub,
)
from dictionary.contextual.form_inventory import (  # noqa: E402
    AugmentedMorphologyAnalyzer,
    DeDeFormInventoryAnalyzer,
    FormInventoryError,
)
from dictionary.contextual.fusion import GermanAnalysisFuser  # noqa: E402
from dictionary.contextual.german_provider import GermanLanguageAnalyzer  # noqa: E402
from dictionary.contextual.separable_verbs import GermanSeparableVerbRecombiner  # noqa: E402
from dictionary.contextual.zdl_adapter import ZdlAdapterError, ZdlContextAnalyzer  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--canonical", required=True, type=Path, help="C11 .cplex bundle")
    parser.add_argument("--form-inventory", required=True, type=Path, help="verified de-DE .cpdict")
    parser.add_argument(
        "--frequency-ranking",
        choices=("none", "wordfreq"),
        default="wordfreq",
    )
    args = parser.parse_args()

    try:
        canonical = load_canonical_lexicon_index(args.canonical)
        morphology = AugmentedMorphologyAnalyzer(
            DwdsmorMorphologyAnalyzer.from_open_edition(),
            DeDeFormInventoryAnalyzer.from_cpdict(args.form_inventory),
        )
        analyzer = GermanLanguageAnalyzer(
            ZdlContextAnalyzer.from_pinned_model(),
            morphology,
            GermanAnalysisFuser(),
            candidate_augmenter=GermanSeparableVerbRecombiner(morphology),
        )
        frequency = (
            load_wordfreq_provider()
            if args.frequency_ranking == "wordfreq"
            else None
        )
        compiled = compile_contextual_epub(
            args.input,
            args.output,
            analyzer,
            canonical,
            frequency,
        )
    except (
        CanonicalLexiconError,
        ContextualEpubError,
        DwdsmorAdapterError,
        FormInventoryError,
        OSError,
        ValueError,
        ZdlAdapterError,
    ) as error:
        print(f"contextual EPUB compilation failed: {error}", file=sys.stderr)
        return 1

    print(
        f"Wrote {args.output} with {len(compiled.xhtml_spines)} spine(s), "
        f"{len(compiled.language_artifact)} language bytes, and "
        f"{compiled.missing_canonical_analyses} unmapped analysis candidate(s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

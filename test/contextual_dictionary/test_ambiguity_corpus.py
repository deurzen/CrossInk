import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.analysis_policy import CanonicalAnalysis  # noqa: E402
from dictionary.contextual.corpus import evaluate_corpus, load_corpus  # noqa: E402
from dictionary.contextual.fusion import GermanAnalysisFuser  # noqa: E402
from dictionary.contextual.pipeline import (  # noqa: E402
    AnalysisProvenance,
    ContextToken,
    MorphologyCandidate,
)

CORPUS_PATH = ROOT / "test" / "data" / "contextual" / "german-ambiguity-corpus.json"
BASELINE_PATH = ROOT / "test" / "data" / "contextual" / "german-ambiguity-baseline.json"


class GoldContext:
    def __init__(self, cases):
        self.by_sentence = {}
        for case in cases:
            target = case.target
            start = -1
            search_from = 0
            for _ in range(target.occurrence + 1):
                start = case.sentence.index(target.surface, search_from)
                search_from = start + len(target.surface)
            analysis = CanonicalAnalysis(target.lemma, target.part_of_speech)
            self.by_sentence[case.sentence] = ContextToken(
                target.surface,
                start,
                start + len(target.surface),
                analysis,
            )

    def analyze_sentence(self, sentence):
        return (self.by_sentence[sentence],)


class GoldMorphology:
    def __init__(self, cases):
        self.by_surface = {}
        for case in cases:
            target = case.target
            analysis = CanonicalAnalysis(target.lemma, target.part_of_speech)
            self.by_surface.setdefault(target.surface, set()).add(analysis)

    def analyze_surface(self, surface):
        return tuple(
            MorphologyCandidate(analysis, AnalysisProvenance.PRIMARY_MORPHOLOGY)
            for analysis in self.by_surface[surface]
        )


class AmbiguityCorpusTest(unittest.TestCase):
    def setUp(self):
        self.cases = load_corpus(CORPUS_PATH)

    def test_corpus_covers_required_ambiguity_categories(self):
        categories = {case.category for case in self.cases}
        self.assertTrue(
            {
                "sentence-initial-verb",
                "noun-verb-homograph",
                "participle",
                "separable-verb",
                "nominalization",
                "compound",
                "proper-name",
                "archaic-form",
            }.issubset(categories)
        )
        self.assertEqual(len(self.cases), 16)
        self.assertEqual(len({case.case_id for case in self.cases}), len(self.cases))

    def test_evaluator_separates_context_coverage_and_fusion(self):
        evaluation = evaluate_corpus(
            self.cases,
            GoldContext(self.cases),
            GoldMorphology(self.cases),
            GermanAnalysisFuser(),
        )
        self.assertEqual(
            evaluation.summary(),
            {
                "cases": 16,
                "contextCorrect": 16,
                "morphologyCovered": 16,
                "fusedPrimaryCorrect": 16,
            },
        )

    def test_checked_in_augmented_baseline_is_explicit(self):
        baseline = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
        providers = baseline["providers"]
        self.assertEqual(providers["dwdsmor"], "0.18.0-open")
        self.assertEqual(providers["analysisPolicy"], 2)
        self.assertEqual(
            providers["formInventory"],
            {
                "archiveSha256": "5ae190802e85113ba192319b18c33e65ab1cd83a0f82a6e1154e824ba6a259ea",
                "bundleUuid": "008b41d0021c591bb16e87573a017dfa",
                "lexemeCount": 181609,
                "licenseSpdx": "CC-BY-SA-4.0",
            },
        )
        self.assertEqual(
            baseline["summary"],
            {
                "cases": 16,
                "contextCorrect": 13,
                "morphologyCovered": 16,
                "fusedPrimaryCorrect": 14,
            },
        )
        failures = {
            case["id"]
            for case in baseline["cases"]
            if not case["fusedPrimaryCorrect"]
        }
        self.assertEqual(
            failures,
            {
                "imperative-liebe",
                "archaic-verb-hub",
            },
        )


if __name__ == "__main__":
    unittest.main()

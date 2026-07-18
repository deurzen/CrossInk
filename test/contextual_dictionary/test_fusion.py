from itertools import permutations
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.analysis_policy import (  # noqa: E402
    CanonicalAnalysis,
    CanonicalFeatures,
    CanonicalPos,
)
from dictionary.contextual.fusion import (  # noqa: E402
    GermanAnalysisFuser,
    MAX_NORMALIZED_SCORE,
    MIN_NORMALIZED_SCORE,
    normalize_score,
)
from dictionary.contextual.pipeline import (  # noqa: E402
    AnalysisProvenance,
    ContextToken,
    MorphologyCandidate,
)


class FusionTest(unittest.TestCase):
    def setUp(self):
        self.fuser = GermanAnalysisFuser()

    @staticmethod
    def token(context):
        return ContextToken("surface", 0, 7, context)

    @staticmethod
    def candidates(*analyses):
        return tuple(
            MorphologyCandidate(analysis, AnalysisProvenance.PRIMARY_MORPHOLOGY)
            for analysis in analyses
        )

    def test_context_selects_verb_or_noun_without_id_ordering(self):
        noun = CanonicalAnalysis("Liebe", CanonicalPos.NOUN)
        verb = CanonicalAnalysis("lieben", CanonicalPos.VERB)
        noun_result = self.fuser.rank(self.token(noun), self.candidates(verb, noun))
        verb_result = self.fuser.rank(self.token(verb), self.candidates(noun, verb))
        self.assertEqual([item.analysis for item in noun_result], [noun])
        self.assertEqual([item.analysis for item in verb_result], [verb])
        self.assertEqual(noun_result[0].score, 1000)
        self.assertEqual(verb_result[0].score, 1000)

    def test_collapses_inflection_variants_using_best_feature_evidence(self):
        context = CanonicalAnalysis(
            "Laden",
            CanonicalPos.NOUN,
            CanonicalFeatures(case="nominative", number="singular"),
        )
        nominative = CanonicalAnalysis(
            "Laden",
            CanonicalPos.NOUN,
            CanonicalFeatures(case="nominative", number="singular"),
        )
        dative = CanonicalAnalysis(
            "Laden",
            CanonicalPos.NOUN,
            CanonicalFeatures(case="dative", number="singular"),
        )
        result = self.fuser.rank(self.token(context), self.candidates(dative, nominative))
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0].analysis, nominative)
        self.assertEqual(result[0].score, 1060)

    def test_retains_bounded_credible_alternatives(self):
        context = CanonicalAnalysis("gleich", CanonicalPos.UNKNOWN)
        candidates = tuple(
            CanonicalAnalysis("gleich", pos)
            for pos in (
                CanonicalPos.NOUN,
                CanonicalPos.VERB,
                CanonicalPos.ADJECTIVE,
                CanonicalPos.ADVERB,
                CanonicalPos.PRONOUN,
                CanonicalPos.DETERMINER,
                CanonicalPos.ADPOSITION,
                CanonicalPos.CONJUNCTION,
                CanonicalPos.NUMERAL,
                CanonicalPos.PARTICLE,
            )
        )
        result = self.fuser.rank(self.token(context), self.candidates(*reversed(candidates)))
        self.assertEqual(len(result), 8)
        self.assertTrue(all(item.score == 400 for item in result))
        self.assertEqual(
            [item.analysis.part_of_speech for item in result],
            list(range(CanonicalPos.NOUN, CanonicalPos.CONJUNCTION + 1)),
        )

    def test_deterministic_across_candidate_input_order(self):
        context = CanonicalAnalysis("Band", CanonicalPos.UNKNOWN)
        candidates = (
            CanonicalAnalysis("Band", CanonicalPos.NOUN),
            CanonicalAnalysis("Band", CanonicalPos.VERB),
            CanonicalAnalysis("Band", CanonicalPos.ADJECTIVE),
        )
        expected = self.fuser.rank(self.token(context), self.candidates(*candidates))
        for ordering in permutations(candidates):
            self.assertEqual(
                self.fuser.rank(self.token(context), self.candidates(*ordering)),
                expected,
            )

    def test_no_candidates_produces_no_analysis(self):
        context = CanonicalAnalysis("unbekannt", CanonicalPos.UNKNOWN)
        self.assertEqual(self.fuser.rank(self.token(context), ()), ())

    def test_merges_provenance_across_duplicate_lexical_candidates(self):
        analysis = CanonicalAnalysis("schreiben", CanonicalPos.VERB)
        candidates = (
            MorphologyCandidate(analysis, AnalysisProvenance.PRIMARY_MORPHOLOGY),
            MorphologyCandidate(analysis, AnalysisProvenance.EXACT_FORM_INVENTORY),
        )
        result = self.fuser.rank(self.token(analysis), candidates)
        self.assertEqual(len(result), 1)
        self.assertEqual(
            result[0].provenance,
            AnalysisProvenance.PRIMARY_MORPHOLOGY
            | AnalysisProvenance.EXACT_FORM_INVENTORY,
        )

    def test_confidence_normalization_is_rounded_and_saturated(self):
        self.assertEqual(normalize_score(MIN_NORMALIZED_SCORE - 1), 0)
        self.assertEqual(normalize_score(MIN_NORMALIZED_SCORE), 0)
        self.assertEqual(normalize_score(MAX_NORMALIZED_SCORE), 1000)
        self.assertEqual(normalize_score(MAX_NORMALIZED_SCORE + 1), 1000)
        self.assertEqual(normalize_score(125), 500)
        self.assertLess(normalize_score(500), normalize_score(1000))


if __name__ == "__main__":
    unittest.main()

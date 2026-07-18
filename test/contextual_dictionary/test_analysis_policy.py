from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.analysis_policy import (  # noqa: E402
    ALTERNATIVE_SCORE_WINDOW,
    DWDSMOR_POS_MAP,
    MAX_ALTERNATIVES,
    POLICY_VERSION,
    CanonicalAnalysis,
    CanonicalFeatures,
    CanonicalPos,
    map_dwdsmor_analysis,
    map_zdl_analysis,
    score_analysis,
)


class AnalysisPolicyTest(unittest.TestCase):
    def test_policy_contract_is_versioned_and_bounded(self):
        self.assertEqual(POLICY_VERSION, 1)
        self.assertEqual(MAX_ALTERNATIVES, 8)
        self.assertEqual(ALTERNATIVE_SCORE_WINDOW, 180)
        self.assertEqual(CanonicalPos.NOUN, 1)
        self.assertEqual(CanonicalPos.VERB, 2)
        self.assertEqual(CanonicalPos.PROPER_NOUN, 12)

    def test_every_dwdsmor_vocabulary_class_has_explicit_mapping(self):
        expected = {
            "ADJ", "ADV", "ART", "CARD", "CONJ", "DEM", "FRAC", "INDEF",
            "INTJ", "NN", "NPROP", "ORD", "POSS", "POSTP", "PPRO", "PREP",
            "PREPART", "PROADV", "PTCL", "PUNCT", "REL", "V", "WPRO",
        }
        self.assertEqual(set(DWDSMOR_POS_MAP), expected)

    def test_maps_provider_features_to_shared_vocabulary(self):
        dwdsmor = map_dwdsmor_analysis(
            "bilden",
            "V",
            number="Pl",
            person="3",
            tense="Pres",
            mood="Ind",
        )
        zdl = map_zdl_analysis(
            "bilden",
            "VERB",
            "VVFIN",
            {
                "Mood": "Ind",
                "Number": "Plur",
                "Person": "3",
                "Tense": "Pres",
                "VerbForm": "Fin",
            },
        )
        self.assertEqual(dwdsmor.part_of_speech, CanonicalPos.VERB)
        self.assertEqual(zdl.part_of_speech, CanonicalPos.VERB)
        self.assertEqual(dwdsmor.features.number, "plural")
        self.assertEqual(zdl.features.verb_form, "finite")
        self.assertEqual(score_analysis(zdl, dwdsmor).feature_matches, 4)

    def test_zdl_universal_pos_precedes_stts_fallback(self):
        universal = map_zdl_analysis("laden", "VERB", "NN", {})
        fallback = map_zdl_analysis("Laden", None, "NN", {})
        unknown = map_zdl_analysis("xyz", None, "UNRECOGNIZED", {})
        self.assertEqual(universal.part_of_speech, CanonicalPos.VERB)
        self.assertEqual(fallback.part_of_speech, CanonicalPos.NOUN)
        self.assertEqual(unknown.part_of_speech, CanonicalPos.UNKNOWN)

    def test_context_reverses_noun_verb_homograph_order(self):
        noun_context = CanonicalAnalysis("Liebe", CanonicalPos.NOUN)
        verb_context = CanonicalAnalysis("lieben", CanonicalPos.VERB)
        noun_candidate = CanonicalAnalysis("Liebe", CanonicalPos.NOUN)
        verb_candidate = CanonicalAnalysis("lieben", CanonicalPos.VERB)

        self.assertGreater(
            score_analysis(noun_context, noun_candidate).total,
            score_analysis(noun_context, verb_candidate).total,
        )
        self.assertGreater(
            score_analysis(verb_context, verb_candidate).total,
            score_analysis(verb_context, noun_candidate).total,
        )

    def test_wrong_contextual_lemma_does_not_erase_pos_evidence(self):
        context = CanonicalAnalysis("sehr", CanonicalPos.ADVERB)
        valid_candidate = CanonicalAnalysis("mehr", CanonicalPos.ADVERB)
        wrong_pos = CanonicalAnalysis("Meer", CanonicalPos.NOUN)
        self.assertEqual(score_analysis(context, valid_candidate).total, 500)
        self.assertGreater(
            score_analysis(context, valid_candidate).total,
            score_analysis(context, wrong_pos).total,
        )

    def test_only_shared_features_affect_score(self):
        context = CanonicalAnalysis(
            "laden",
            CanonicalPos.VERB,
            CanonicalFeatures(number="plural", person="first", tense="present"),
        )
        partial = CanonicalAnalysis(
            "laden",
            CanonicalPos.VERB,
            CanonicalFeatures(number="plural"),
        )
        conflict = CanonicalAnalysis(
            "laden",
            CanonicalPos.VERB,
            CanonicalFeatures(number="singular"),
        )
        partial_score = score_analysis(context, partial)
        conflict_score = score_analysis(context, conflict)
        self.assertEqual(partial_score.feature_matches, 1)
        self.assertEqual(partial_score.feature_conflicts, 0)
        self.assertEqual(conflict_score.feature_conflicts, 1)
        self.assertGreater(partial_score.total, conflict_score.total)


if __name__ == "__main__":
    unittest.main()

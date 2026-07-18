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
from dictionary.contextual.pipeline import (  # noqa: E402
    AnalysisProvenance,
    ContextToken,
    MorphologyCandidate,
)
from dictionary.contextual.separable_verbs import (  # noqa: E402
    GermanSeparableVerbRecombiner,
    SeparableVerbError,
    SeparableVerbLimits,
)


PRIMARY = AnalysisProvenance.PRIMARY_MORPHOLOGY
EXACT = AnalysisProvenance.EXACT_FORM_INVENTORY


def analysis(lemma, pos, verb_form=None):
    return CanonicalAnalysis(
        lemma,
        pos,
        CanonicalFeatures(verb_form=verb_form),
    )


def candidate(lemma, pos=CanonicalPos.VERB, provenance=PRIMARY):
    return MorphologyCandidate(CanonicalAnalysis(lemma, pos), provenance)


class LexicalAnalyzer:
    def __init__(self, by_surface=None):
        self.by_surface = by_surface or {}
        self.surfaces = []

    def analyze_surface(self, surface):
        self.surfaces.append(surface)
        return self.by_surface.get(surface, ())


class SeparableVerbTest(unittest.TestCase):
    def test_adds_validated_lexical_lemma_to_finite_token_only(self):
        tokens = (
            ContextToken("Er", 0, 2, analysis("er", CanonicalPos.PRONOUN)),
            ContextToken("steht", 3, 8, analysis("stehen", CanonicalPos.VERB, "finite"), "VVFIN"),
            ContextToken("früh", 9, 13, analysis("früh", CanonicalPos.ADVERB)),
            ContextToken("auf", 14, 17, analysis("auf", CanonicalPos.ADPOSITION), "PTKVZ"),
            ContextToken(".", 17, 18, analysis(".", CanonicalPos.OTHER), "$.")
        )
        rows = (
            (),
            (candidate("stehen"),),
            (),
            (candidate("auf", CanonicalPos.ADPOSITION),),
            (),
        )
        lexical = LexicalAnalyzer({"aufstehen": (candidate("aufstehen", provenance=EXACT),)})
        result = GermanSeparableVerbRecombiner(lexical).augment_sentence(
            "Er steht früh auf.", tokens, rows
        )
        self.assertEqual(lexical.surfaces, ["aufstehen"])
        self.assertEqual(result[0], rows[0])
        self.assertEqual(result[2:], rows[2:])
        self.assertEqual(
            [(item.analysis.lemma, item.provenance) for item in result[1]],
            [
                ("aufstehen", EXACT | AnalysisProvenance.CONTEXT_RECOMBINATION),
                ("stehen", PRIMARY),
            ],
        )

    def test_ignores_prepositions_and_unvalidated_combinations(self):
        finite = ContextToken(
            "hängt",
            9,
            14,
            analysis("hängen", CanonicalPos.VERB, "finite"),
            "VVFIN",
        )
        preposition = ContextToken(
            "an",
            15,
            17,
            analysis("an", CanonicalPos.ADPOSITION),
            "APPR",
        )
        rows = ((candidate("hängen"),), (candidate("an", CanonicalPos.ADPOSITION),))
        lexical = LexicalAnalyzer({"anhängen": (candidate("anhängen", provenance=EXACT),)})
        result = GermanSeparableVerbRecombiner(lexical).augment_sentence(
            "Das Bild hängt an der Wand.",
            (finite, preposition),
            rows,
        )
        self.assertEqual(result, rows)
        self.assertEqual(lexical.surfaces, [])

        particle = ContextToken(
            "los",
            15,
            18,
            analysis("los", CanonicalPos.ADPOSITION),
            "PTKVZ",
        )
        result = GermanSeparableVerbRecombiner(LexicalAnalyzer()).augment_sentence(
            "Er hängt etwas los", (finite, particle), rows
        )
        self.assertEqual(result, rows)

    def test_uses_nearest_finite_verb_and_does_not_cross_sentence_boundary(self):
        tokens = (
            ContextToken("ruft", 0, 4, analysis("rufen", CanonicalPos.VERB, "finite"), "VVFIN"),
            ContextToken("an", 5, 7, analysis("an", CanonicalPos.ADPOSITION), "PTKVZ"),
            ContextToken(".", 7, 8, analysis(".", CanonicalPos.OTHER), "$.") ,
            ContextToken("steht", 9, 14, analysis("stehen", CanonicalPos.VERB, "finite"), "VVFIN"),
            ContextToken("auf", 15, 18, analysis("auf", CanonicalPos.ADPOSITION), "PTKVZ"),
        )
        rows = (
            (candidate("rufen"),),
            (),
            (),
            (candidate("stehen"),),
            (),
        )
        lexical = LexicalAnalyzer(
            {
                "anrufen": (candidate("anrufen", provenance=EXACT),),
                "aufstehen": (candidate("aufstehen", provenance=EXACT),),
            }
        )
        result = GermanSeparableVerbRecombiner(lexical).augment_sentence("ruft an. steht auf", tokens, rows)
        self.assertEqual(lexical.surfaces, ["anrufen", "aufstehen"])
        self.assertEqual({item.analysis.lemma for item in result[0]}, {"rufen", "anrufen"})
        self.assertEqual({item.analysis.lemma for item in result[3]}, {"stehen", "aufstehen"})

    def test_enforces_particle_base_and_lexical_caps(self):
        finite = ContextToken(
            "steht",
            0,
            5,
            analysis("stehen", CanonicalPos.VERB, "finite"),
            "VVFIN",
        )
        particle = ContextToken(
            "auf",
            6,
            9,
            analysis("auf", CanonicalPos.ADPOSITION),
            "PTKVZ",
        )
        rows = ((candidate("stehen"), candidate("gehen")), ())
        with self.assertRaisesRegex(SeparableVerbError, "base-lemma cap 1"):
            GermanSeparableVerbRecombiner(
                LexicalAnalyzer(),
                SeparableVerbLimits(max_base_lemmas=1),
            ).augment_sentence("steht auf", (finite, particle), rows)

        lexical = LexicalAnalyzer(
            {
                "aufstehen": (
                    candidate("aufstehen", provenance=EXACT),
                    candidate("Aufstehen", CanonicalPos.NOUN, EXACT),
                )
            }
        )
        with self.assertRaisesRegex(SeparableVerbError, "lexical-analysis cap 1"):
            GermanSeparableVerbRecombiner(
                lexical,
                SeparableVerbLimits(max_lexical_analyses=1),
            ).augment_sentence("steht auf", (finite, particle), ((candidate("stehen"),), ()))


if __name__ == "__main__":
    unittest.main()

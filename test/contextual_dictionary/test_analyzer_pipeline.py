from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.analysis_policy import CanonicalAnalysis, CanonicalPos  # noqa: E402
from dictionary.contextual.german_provider import GermanLanguageAnalyzer  # noqa: E402
from dictionary.contextual.pipeline import (  # noqa: E402
    AnalysisPipelineError,
    AnalysisProvenance,
    AnalyzedToken,
    AnalyzerLimits,
    AnalyzerPipeline,
    ContextToken,
    LanguageAnalyzer,
    MorphologyCandidate,
    RankedAnalysis,
)


class SyntheticContext:
    def __init__(self, tokens=None, error=None):
        self.tokens = tokens or ()
        self.error = error

    def analyze_sentence(self, sentence):
        if self.error:
            raise self.error
        return self.tokens


class SyntheticMorphology:
    def __init__(self, by_surface=None, error=None):
        self.by_surface = by_surface or {}
        self.error = error

    def analyze_surface(self, surface):
        if self.error:
            raise self.error
        return tuple(
            MorphologyCandidate(analysis, AnalysisProvenance.PRIMARY_MORPHOLOGY)
            for analysis in self.by_surface.get(surface, ())
        )


class SyntheticFuser:
    def __init__(self, override=None):
        self.override = override

    def rank(self, token, candidates):
        if self.override is not None:
            return self.override
        return tuple(
            RankedAnalysis(candidate.analysis, 1000 - index, candidate.provenance)
            for index, candidate in enumerate(candidates)
        )


class AnalyzerPipelineTest(unittest.TestCase):
    def setUp(self):
        self.context_analysis = CanonicalAnalysis("laden", CanonicalPos.VERB)
        self.noun = CanonicalAnalysis("Laden", CanonicalPos.NOUN)
        self.verb = CanonicalAnalysis("laden", CanonicalPos.VERB)
        self.token = ContextToken("laden", 4, 9, self.context_analysis)

    def pipeline(self, **overrides):
        return AnalyzerPipeline(
            language=overrides.get("language", "xx"),
            context_analyzer=overrides.get("context", SyntheticContext((self.token,))),
            morphology_analyzer=overrides.get(
                "morphology",
                SyntheticMorphology({"laden": (self.noun, self.verb)}),
            ),
            fuser=overrides.get("fuser", SyntheticFuser()),
            limits=overrides.get("limits", AnalyzerLimits()),
        )

    def test_composes_synthetic_providers_and_preserves_source_offsets(self):
        pipeline = self.pipeline()
        result = pipeline.analyze_sentence("Wir laden.", source_offset=100)
        self.assertEqual(pipeline.language, "xx")
        self.assertEqual(
            result,
            (
                AnalyzedToken(
                    surface="laden",
                    start=104,
                    end=109,
                    context=self.context_analysis,
                    analyses=(
                        RankedAnalysis(self.noun, 1000, AnalysisProvenance.PRIMARY_MORPHOLOGY),
                        RankedAnalysis(self.verb, 999, AnalysisProvenance.PRIMARY_MORPHOLOGY),
                    ),
                ),
            ),
        )

    def test_german_shell_satisfies_generic_protocol(self):
        analyzer = GermanLanguageAnalyzer(
            SyntheticContext((self.token,)),
            SyntheticMorphology({"laden": (self.verb,)}),
            SyntheticFuser(),
        )
        self.assertIsInstance(analyzer, LanguageAnalyzer)
        self.assertEqual(analyzer.language, "de")

    def test_rejects_context_offsets_and_surface_mismatch(self):
        overlap = (
            ContextToken("Wir", 0, 3, self.context_analysis),
            ContextToken("laden", 2, 7, self.context_analysis),
        )
        with self.assertRaisesRegex(AnalysisPipelineError, "invalid offsets"):
            self.pipeline(context=SyntheticContext(overlap)).analyze_sentence("Wir laden.")
        mismatch = ContextToken("Laden", 4, 9, self.context_analysis)
        with self.assertRaisesRegex(AnalysisPipelineError, "surface does not match"):
            self.pipeline(context=SyntheticContext((mismatch,))).analyze_sentence("Wir laden.")

    def test_enforces_sentence_token_and_morphology_caps(self):
        limits = AnalyzerLimits(max_sentence_bytes=8, max_tokens=1, max_morphology_analyses=1)
        with self.assertRaisesRegex(AnalysisPipelineError, "UTF-8 bytes"):
            self.pipeline(limits=limits).analyze_sentence("Wir laden.")
        with self.assertRaisesRegex(AnalysisPipelineError, "context: result exceeds cap"):
            self.pipeline(
                context=SyntheticContext((self.token, self.token)),
                limits=AnalyzerLimits(max_tokens=1),
            ).analyze_sentence("Wir laden.")
        with self.assertRaisesRegex(AnalysisPipelineError, "morphology: result exceeds cap"):
            self.pipeline(
                limits=AnalyzerLimits(max_morphology_analyses=1),
            ).analyze_sentence("Wir laden.")

    def test_fuser_cannot_invent_or_duplicate_candidates(self):
        source = AnalysisProvenance.PRIMARY_MORPHOLOGY
        invented = CanonicalAnalysis("erfinden", CanonicalPos.VERB)
        with self.assertRaisesRegex(AnalysisPipelineError, "introduced a new analysis"):
            self.pipeline(fuser=SyntheticFuser((RankedAnalysis(invented, 1, source),))).analyze_sentence("Wir laden.")
        duplicate = (RankedAnalysis(self.verb, 2, source), RankedAnalysis(self.verb, 1, source))
        with self.assertRaisesRegex(AnalysisPipelineError, "duplicate analyses"):
            self.pipeline(fuser=SyntheticFuser(duplicate)).analyze_sentence("Wir laden.")

    def test_provider_failures_report_the_stage(self):
        with self.assertRaisesRegex(AnalysisPipelineError, "context: model failed"):
            self.pipeline(context=SyntheticContext(error=RuntimeError("model failed"))).analyze_sentence("Wir laden.")
        with self.assertRaisesRegex(AnalysisPipelineError, "morphology: fst failed"):
            self.pipeline(morphology=SyntheticMorphology(error=RuntimeError("fst failed"))).analyze_sentence(
                "Wir laden."
            )


if __name__ == "__main__":
    unittest.main()

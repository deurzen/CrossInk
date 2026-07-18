from pathlib import Path
from types import SimpleNamespace
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.analysis_policy import CanonicalPos  # noqa: E402
from dictionary.contextual.dwdsmor_adapter import (  # noqa: E402
    DwdsmorAdapterError,
    DwdsmorLimits,
    DwdsmorMorphologyAnalyzer,
)


class FakeAnalyzer:
    def __init__(self, traversals=(), error=None):
        self.traversals = traversals
        self.error = error
        self.surfaces = []

    def analyze(self, surface):
        self.surfaces.append(surface)
        if self.error:
            raise self.error
        return iter(self.traversals)


def traversal(lemma="laden", pos="V", **features):
    return SimpleNamespace(analysis=lemma, pos=pos, **features)


class DwdsmorAdapterTest(unittest.TestCase):
    def test_maps_ambiguous_traversals_and_deduplicates_exact_results(self):
        source = FakeAnalyzer(
            (
                traversal("Liebe", "NN", case="Nom", number="Sg"),
                traversal("lieben", "V", person="1", number="Sg", tense="Pres"),
                traversal("Liebe", "NN", case="Nom", number="Sg"),
            )
        )
        analyses = tuple(DwdsmorMorphologyAnalyzer(source).analyze_surface("Liebe"))
        self.assertEqual(len(analyses), 2)
        self.assertEqual(
            {(item.lemma, item.part_of_speech) for item in analyses},
            {("Liebe", CanonicalPos.NOUN), ("lieben", CanonicalPos.VERB)},
        )
        noun = next(item for item in analyses if item.part_of_speech == CanonicalPos.NOUN)
        self.assertEqual(noun.features.case, "nominative")
        self.assertEqual(noun.features.number, "singular")

    def test_normalizes_surface_and_lemma_to_nfc(self):
        source = FakeAnalyzer((traversal("Ha\u0308user", "NN"),))
        analysis = tuple(DwdsmorMorphologyAnalyzer(source).analyze_surface("Ha\u0308user"))[0]
        self.assertEqual(source.surfaces, ["Häuser"])
        self.assertEqual(analysis.lemma, "Häuser")

    def test_enforces_surface_traversal_and_unique_caps(self):
        with self.assertRaisesRegex(DwdsmorAdapterError, "surface has"):
            DwdsmorMorphologyAnalyzer(
                FakeAnalyzer(), DwdsmorLimits(max_surface_bytes=4)
            ).analyze_surface("Häuser")
        with self.assertRaisesRegex(DwdsmorAdapterError, "traversal cap 1"):
            DwdsmorMorphologyAnalyzer(
                FakeAnalyzer((traversal("eins"), traversal("zwei"))),
                DwdsmorLimits(max_traversals=1),
            ).analyze_surface("eins")
        with self.assertRaisesRegex(DwdsmorAdapterError, "unique-analysis cap 1"):
            DwdsmorMorphologyAnalyzer(
                FakeAnalyzer((traversal("eins"), traversal("zwei"))),
                DwdsmorLimits(max_unique_analyses=1),
            ).analyze_surface("eins")

    def test_rejects_malformed_or_oversized_fields(self):
        cases = (
            (SimpleNamespace(analysis=None, pos="V"), "invalid lemma"),
            (SimpleNamespace(analysis="laden", pos=None), "invalid POS"),
            (traversal("laden", "V", number=3), "feature number is not text"),
        )
        for malformed, message in cases:
            with self.subTest(message=message):
                with self.assertRaisesRegex(DwdsmorAdapterError, message):
                    DwdsmorMorphologyAnalyzer(FakeAnalyzer((malformed,))).analyze_surface("laden")
        with self.assertRaisesRegex(DwdsmorAdapterError, "lemma has"):
            DwdsmorMorphologyAnalyzer(
                FakeAnalyzer((traversal("überlang", "V"),)),
                DwdsmorLimits(max_lemma_bytes=4),
            ).analyze_surface("kurz")
        with self.assertRaisesRegex(DwdsmorAdapterError, "feature number exceeds"):
            DwdsmorMorphologyAnalyzer(
                FakeAnalyzer((traversal("laden", "V", number="oversized"),)),
                DwdsmorLimits(max_tag_bytes=4),
            ).analyze_surface("laden")

    def test_wraps_analyzer_and_iterator_failures(self):
        with self.assertRaisesRegex(DwdsmorAdapterError, "analysis failed: fst failed"):
            DwdsmorMorphologyAnalyzer(FakeAnalyzer(error=RuntimeError("fst failed"))).analyze_surface("laden")

        def broken_output():
            yield traversal()
            raise RuntimeError("decode failed")

        with self.assertRaisesRegex(DwdsmorAdapterError, "cannot iterate.*decode failed"):
            DwdsmorMorphologyAnalyzer(FakeAnalyzer(broken_output())).analyze_surface("laden")


if __name__ == "__main__":
    unittest.main()

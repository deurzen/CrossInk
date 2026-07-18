import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.compiler import compile_bundle  # noqa: E402
from dictionary.contextual.analysis_policy import CanonicalAnalysis, CanonicalPos  # noqa: E402
from dictionary.contextual.form_inventory import (  # noqa: E402
    AugmentedMorphologyAnalyzer,
    DeDeFormInventoryAnalyzer,
    FormInventoryError,
    FormInventoryLimits,
)
from dictionary.contextual.pipeline import (  # noqa: E402
    AnalysisProvenance,
    MorphologyCandidate,
)


SOURCE = {
    "bundleUuid": "12345678-1234-5678-9abc-def012345678",
    "sourceLanguage": "de",
    "targetLanguage": "de",
    "license": {
        "spdx": "CC0-1.0",
        "sourceUrl": "https://crossink.example/form-inventory-test",
        "attribution": "Synthetic form inventory.",
    },
    "lexemes": [
        {
            "headword": "schreiben",
            "partOfSpeech": "verb",
            "forms": ["geschrieben"],
            "fields": [{"type": "definition", "text": "synthetic"}],
        },
        {
            "headword": "geschrieben",
            "partOfSpeech": "adjective",
            "forms": ["geschriebene", "geschrieben"],
            "fields": [{"type": "definition", "text": "synthetic"}],
        },
        {
            "headword": "Goethe",
            "partOfSpeech": "proper-noun",
            "forms": [],
            "fields": [{"type": "definition", "text": "synthetic"}],
        },
        {
            "headword": "heben",
            "partOfSpeech": "verb",
            "forms": ["hub"],
            "fields": [{"type": "definition", "text": "synthetic"}],
        },
        {
            "headword": "Hub",
            "partOfSpeech": "noun",
            "forms": [],
            "fields": [{"type": "definition", "text": "synthetic"}],
        },
    ],
}


class SyntheticMorphology:
    def __init__(self, candidates):
        self.candidates = candidates

    def analyze_surface(self, surface):
        return self.candidates


def write_bundle(path, source=SOURCE):
    path.write_bytes(compile_bundle(source).archive_bytes)


class FormInventoryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.path = Path(self.temporary.name) / "de.cpdict"
        write_bundle(self.path)
        self.inventory = DeDeFormInventoryAnalyzer.from_cpdict(self.path)

    def tearDown(self):
        self.temporary.cleanup()

    def test_loads_verified_identity_and_common_missing_forms(self):
        self.assertEqual(self.inventory.identity.lexeme_count, 5)
        self.assertEqual(self.inventory.identity.license_spdx, "CC0-1.0")
        self.assertEqual(
            self.inventory.identity.source_sha256,
            hashlib.sha256(self.path.read_bytes()).hexdigest(),
        )
        cases = {
            "geschrieben": {
                ("geschrieben", CanonicalPos.ADJECTIVE),
                ("schreiben", CanonicalPos.VERB),
            },
            "geschriebene": {("geschrieben", CanonicalPos.ADJECTIVE)},
            "Goethe": {("Goethe", CanonicalPos.PROPER_NOUN)},
            "hub": {
                ("heben", CanonicalPos.VERB),
                ("Hub", CanonicalPos.NOUN),
            },
        }
        for surface, expected in cases.items():
            with self.subTest(surface=surface):
                result = self.inventory.analyze_surface(surface)
                self.assertEqual(
                    {(item.analysis.lemma, item.analysis.part_of_speech) for item in result},
                    expected,
                )
                self.assertTrue(
                    any(
                        item.provenance == AnalysisProvenance.EXACT_FORM_INVENTORY
                        for item in result
                    )
                )

    def test_exact_forms_precede_case_folded_supplements(self):
        result = self.inventory.analyze_surface("Hub")
        by_key = {
            (item.analysis.lemma, item.analysis.part_of_speech): item.provenance
            for item in result
        }
        self.assertEqual(by_key[("Hub", CanonicalPos.NOUN)], AnalysisProvenance.EXACT_FORM_INVENTORY)
        self.assertEqual(by_key[("heben", CanonicalPos.VERB)], AnalysisProvenance.FOLDED_FORM_INVENTORY)

    def test_augmented_analyzer_unions_and_merges_provenance(self):
        primary_analysis = CanonicalAnalysis("schreiben", CanonicalPos.VERB)
        primary = SyntheticMorphology(
            (
                MorphologyCandidate(
                    primary_analysis,
                    AnalysisProvenance.PRIMARY_MORPHOLOGY,
                ),
            )
        )
        result = AugmentedMorphologyAnalyzer(primary, self.inventory).analyze_surface("geschrieben")
        by_key = {
            (item.analysis.lemma, item.analysis.part_of_speech): item.provenance
            for item in result
        }
        self.assertEqual(
            by_key[("schreiben", CanonicalPos.VERB)],
            AnalysisProvenance.PRIMARY_MORPHOLOGY
            | AnalysisProvenance.EXACT_FORM_INVENTORY,
        )
        self.assertEqual(
            by_key[("geschrieben", CanonicalPos.ADJECTIVE)],
            AnalysisProvenance.EXACT_FORM_INVENTORY,
        )
        self.assertTrue(result[0].provenance & AnalysisProvenance.PRIMARY_MORPHOLOGY)

    def test_rejects_wrong_direction_corruption_and_caps(self):
        wrong_direction = copy.deepcopy(SOURCE)
        wrong_direction["targetLanguage"] = "en"
        wrong_path = Path(self.temporary.name) / "wrong.cpdict"
        write_bundle(wrong_path, wrong_direction)
        with self.assertRaisesRegex(FormInventoryError, "de-DE"):
            DeDeFormInventoryAnalyzer.from_cpdict(wrong_path)

        corrupt_path = Path(self.temporary.name) / "corrupt.cpdict"
        with zipfile.ZipFile(self.path) as source, zipfile.ZipFile(corrupt_path, "w") as target:
            for name in source.namelist():
                data = source.read(name)
                if name == "compiler/forms.bin":
                    data = data[:-1] + bytes((data[-1] ^ 0xFF,))
                target.writestr(name, data)
        with self.assertRaisesRegex(FormInventoryError, "hash mismatch"):
            DeDeFormInventoryAnalyzer.from_cpdict(corrupt_path)

        limited = DeDeFormInventoryAnalyzer.from_cpdict(
            self.path,
            FormInventoryLimits(max_unique_analyses=1),
        )
        with self.assertRaisesRegex(FormInventoryError, "inventory-analysis cap 1"):
            limited.analyze_surface("geschrieben")


if __name__ == "__main__":
    unittest.main()

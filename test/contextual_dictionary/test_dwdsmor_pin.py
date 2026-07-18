import json
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.verify_dwdsmor_open import verify_reference  # noqa: E402

CONTEXTUAL = ROOT / "scripts" / "dictionary" / "contextual"
MANIFEST_PATH = CONTEXTUAL / "dwdsmor-open.json"
FIXTURE_PATH = ROOT / "test" / "data" / "contextual" / "dwdsmor-open-reference.json"
LOCK_PATH = CONTEXTUAL / "requirements-dwdsmor.lock"


class FixtureAnalyzer:
    def __init__(self, cases):
        self.analyses = {
            case["surface"]: [SimpleNamespace(analysis=item["lemma"], pos=item["pos"]) for item in case["required"]]
            for case in cases
        }

    def analyze(self, surface):
        return self.analyses[surface]


class DwdsmorPinTest(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        self.fixture = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))

    def test_package_and_lock_pin_published_digest(self):
        package = self.manifest["package"]
        digest = package["sha256"]
        self.assertEqual(len(digest), 64)
        self.assertNotIn("#sha256=", package["url"])
        lock = LOCK_PATH.read_text(encoding="utf-8")
        self.assertIn(f"{package['url']}#sha256={digest}", lock)
        self.assertIn("sfst-transduce==1.3.1", lock)

    def test_manifest_pins_open_edition_automata(self):
        self.assertEqual(self.manifest["edition"], "open")
        self.assertEqual(self.manifest["license"], "GPL-2.0-only")
        automata = self.manifest["automata"]
        self.assertIn("lemma.ca", automata)
        self.assertIn("index.a", automata)
        for expected in automata.values():
            self.assertGreater(expected["bytes"], 0)
            self.assertEqual(len(expected["sha256"]), 64)

    def test_reference_contains_contextually_important_ambiguities(self):
        by_surface = {case["surface"]: case["required"] for case in self.fixture["cases"]}
        self.assertEqual(
            {(item["lemma"], item["pos"]) for item in by_surface["Liebe"]},
            {("Liebe", "NN"), ("lieben", "V")},
        )
        self.assertEqual(
            {(item["lemma"], item["pos"]) for item in by_surface["gebildet"]},
            {("bilden", "V"), ("gebildet", "ADJ")},
        )

    def test_reference_verifier_accepts_required_analyses(self):
        analyzer = FixtureAnalyzer(self.fixture["cases"])
        self.assertEqual(verify_reference(analyzer, FIXTURE_PATH), (6, 9))


if __name__ == "__main__":
    unittest.main()

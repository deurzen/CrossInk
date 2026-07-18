import json
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.verify_zdl_model import verify_reference  # noqa: E402

CONTEXTUAL = ROOT / "scripts" / "dictionary" / "contextual"
MANIFEST_PATH = CONTEXTUAL / "zdl-model.json"
FIXTURE_PATH = ROOT / "test" / "data" / "contextual" / "zdl-reference.json"
LOCK_PATH = CONTEXTUAL / "requirements-zdl.lock"


class FixtureNlp:
    def __init__(self, sentences):
        self.sentences = {sentence["text"]: sentence["tokens"] for sentence in sentences}

    def __call__(self, text):
        return [
            SimpleNamespace(
                idx=token["start"],
                lemma_=token["lemma"],
                pos_=token["pos"],
                tag_=token["tag"],
                text=token["text"],
            )
            for token in self.sentences[text]
        ]


class ZdlModelPinTest(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        self.fixture = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))

    def test_model_url_and_lock_pin_published_digest(self):
        model = self.manifest["model"]
        digest = model["sha256"]
        self.assertEqual(len(digest), 64)
        self.assertNotIn("#sha256=", model["url"])

        lock = LOCK_PATH.read_text(encoding="utf-8")
        self.assertIn(f"{model['url']}#sha256={digest}", lock)
        self.assertIn(f"spacy=={self.manifest['spacyVersion']}", lock)
        self.assertIn("click==8.3.1", lock)

    def test_reference_offsets_select_token_text(self):
        for sentence in self.fixture["sentences"]:
            text = sentence["text"]
            for token in sentence["tokens"]:
                start = token["start"]
                surface = token["text"]
                self.assertEqual(text[start : start + len(surface)], surface)

    def test_reference_verifier_accepts_pinned_snapshot(self):
        nlp = FixtureNlp(self.fixture["sentences"])
        self.assertEqual(verify_reference(nlp, FIXTURE_PATH), 4)


if __name__ == "__main__":
    unittest.main()

import base64
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.book_compiler import compile_book, load_compiler_dictionary  # noqa: E402
from dictionary.compiler import compile_bundle  # noqa: E402

FIXTURE = ROOT / "test" / "data" / "dictionary-sources" / "german-small.json"
NODE_RUNNER = ROOT / "test" / "dictionary_worker" / "compile_fixture.js"


@unittest.skipUnless(shutil.which("node"), "Node.js is not installed")
class DictionaryWorkerTest(unittest.TestCase):
    def test_web_scripts_have_valid_javascript_syntax(self):
        for script in (ROOT / "web" / "assets" / "dictionary-worker.js", ROOT / "web" / "pages" / "files.js"):
            subprocess.run(["node", "--check", str(script)], capture_output=True, text=True, check=True)

    def test_worker_matches_host_reference_artifact(self):
        source = json.loads(FIXTURE.read_text(encoding="utf-8"))
        source = copy.deepcopy(source)
        source["lexemes"].extend(
            [
                {
                    "headword": "Krankenhaus",
                    "partOfSpeech": "noun",
                    "forms": [],
                    "fields": [{"type": "definition", "text": "hospital"}],
                },
                {
                    "headword": "Aufnahme",
                    "partOfSpeech": "noun",
                    "forms": [],
                    "fields": [{"type": "definition", "text": "admission; recording"}],
                },
            ]
        )
        bundle = compile_bundle(source)
        spines = [
            {
                "path": "OPS/chapter1.xhtml",
                "content": "<html><body><p>Die Ha\u0308u<em>sern</em>, Gingen! liebe Krankenhausaufnahme.</p></body></html>",
            },
            {"path": "OPS/chapter2.xhtml", "content": "<p>" + " ".join(["gingen"] * 65) + "</p>"},
        ]
        request = {
            "meta": base64.b64encode(bundle.files["device/meta.bin"]).decode("ascii"),
            "forms": base64.b64encode(bundle.files["compiler/forms.bin"]).decode("ascii"),
            "spines": spines,
        }
        completed = subprocess.run(
            ["node", str(NODE_RUNNER)],
            input=json.dumps(request),
            text=True,
            capture_output=True,
            check=True,
        )
        worker_result = json.loads(completed.stdout)
        dictionary = load_compiler_dictionary(bundle.files["device/meta.bin"], bundle.files["compiler/forms.bin"])
        host_result = compile_book([spine["content"] for spine in spines], dictionary)

        self.assertEqual(base64.b64decode(worker_result["artifact"]), host_result.language_artifact)
        self.assertEqual(
            worker_result["spines"],
            [{"path": spine["path"], "content": content} for spine, content in zip(spines, host_result.xhtml_spines)],
        )

    def test_worker_rejects_corrupt_forms(self):
        source = json.loads(FIXTURE.read_text(encoding="utf-8"))
        bundle = compile_bundle(source)
        forms = bytearray(bundle.files["compiler/forms.bin"])
        forms[-1] ^= 1
        request = {
            "meta": base64.b64encode(bundle.files["device/meta.bin"]).decode("ascii"),
            "forms": base64.b64encode(forms).decode("ascii"),
            "spines": [{"path": "chapter.xhtml", "content": "<p>gehen</p>"}],
        }
        completed = subprocess.run(
            ["node", str(NODE_RUNNER)], input=json.dumps(request), text=True, capture_output=True
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("corrupt forms", completed.stderr)


if __name__ == "__main__":
    unittest.main()

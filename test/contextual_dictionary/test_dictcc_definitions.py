import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.analysis_policy import CanonicalPos  # noqa: E402
from dictionary.contextual.canonical_lexicon import (  # noqa: E402
    CanonicalLexemeInput,
    compile_canonical_bundle,
    load_canonical_lexicon_index,
)
from dictionary.contextual.dictcc_definitions import (  # noqa: E402
    DictCcError,
    compile_dictcc_definition_source,
)


HEADER = """# DE-EN vocabulary database\tcompiled by dict.cc
# License\tTHIS WORK IS PROTECTED BY INTERNATIONAL COPYRIGHT LAWS!
# License\tPrivate use is allowed as long as the data, or parts of it, are not published or given away.
# License\thttps://www.dict.cc/translation_file_request.php
"""


class DictCcDefinitionsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        bundle = compile_canonical_bundle(
            (
                CanonicalLexemeInput("Goethe", CanonicalPos.PROPER_NOUN),
                CanonicalLexemeInput("Laden", CanonicalPos.NOUN),
                CanonicalLexemeInput("laden", CanonicalPos.VERB),
            ),
            {"fixture": "dictcc"},
            "license",
        )
        path = Path(self.temporary.name) / "canonical.cplex"
        path.write_bytes(bundle.archive_bytes)
        self.canonical = load_canonical_lexicon_index(path)

    def tearDown(self):
        self.temporary.cleanup()

    def source(self, lines, name="dict.txt"):
        path = Path(self.temporary.name) / name
        path.write_text(HEADER + "".join(lines), encoding="utf-8")
        return path

    def test_streams_annotations_pos_and_unique_unlabeled_headwords(self):
        path = self.source(
            (
                "Laden {m}\tshop\tnoun\t[comm.] \n",
                "Laden {n}\tloading\tnoun\t[transp.] \n",
                "laden\tto load\tverb\t\n",
                "Goethe\tJohann Wolfgang von Goethe\t\t\n",
                "(abends) ausgehen\tto go out\tverb\t\n",
                "bad\trow\tonly-three\n",
            )
        )
        compiled, stats = compile_dictcc_definition_source(path, self.canonical)
        self.assertEqual(compiled.coverage_count, 3)
        self.assertEqual(stats.data_lines, 6)
        self.assertEqual(stats.malformed_lines, 1)
        self.assertEqual(stats.aligned_lines, 4)
        self.assertEqual(stats.unaligned_lines, 1)
        self.assertEqual(stats.emitted_entries, 4)
        report = json.loads(compiled.files["compiler/coverage.json"])
        self.assertEqual(report["provenance"]["license"], "LicenseRef-dict.cc-private-use")
        self.assertEqual(report["provenance"]["import"], stats.as_dict())
        self.assertIn(b"shop", compiled.files["device/entries.bin"])
        self.assertIn(b"[comm.]", compiled.files["device/entries.bin"])

    def test_caps_large_translation_sets_deterministically(self):
        lines = tuple(
            f"laden\ttranslation {index:03d}\tverb\t\n"
            for index in range(140)
        )
        first_path = self.source(lines, "first.txt")
        second_path = self.source(tuple(reversed(lines)), "second.txt")
        first, _ = compile_dictcc_definition_source(first_path, self.canonical)
        second, _ = compile_dictcc_definition_source(second_path, self.canonical)
        self.assertEqual(first.source_uuid, second.source_uuid)
        self.assertEqual(first.files["device/entry-index.bin"], second.files["device/entry-index.bin"])
        self.assertEqual(first.files["device/entries.bin"], second.files["device/entries.bin"])
        self.assertEqual(first.truncated_field_count, 13)
        self.assertNotIn(b"translation 139", first.files["device/entries.bin"])
        self.assertIn(b"translation 000", first.files["device/entries.bin"])

    def test_same_private_export_builds_byte_identically(self):
        path = self.source(("laden\tto load\tverb\t\n",))
        first, _ = compile_dictcc_definition_source(path, self.canonical)
        second, _ = compile_dictcc_definition_source(path, self.canonical)
        self.assertEqual(first.archive_bytes, second.archive_bytes)

    def test_rejects_missing_private_license_header(self):
        path = Path(self.temporary.name) / "unlicensed.txt"
        path.write_text("laden\tto load\tverb\t\n", encoding="utf-8")
        with self.assertRaisesRegex(DictCcError, "license header"):
            compile_dictcc_definition_source(path, self.canonical)


if __name__ == "__main__":
    unittest.main()

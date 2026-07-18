import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.compiler import compile_bundle  # noqa: E402
from dictionary.contextual.analysis_policy import CanonicalPos  # noqa: E402
from dictionary.contextual.canonical_lexicon import (  # noqa: E402
    compile_de_de_canonical_bundle,
    load_canonical_lexicon_index,
)
from dictionary.contextual.de_de_definitions import (  # noqa: E402
    DeDeDefinitionError,
    compile_de_de_definition_source,
)
from dictionary.contextual.definition_source import validate_definition_files  # noqa: E402


SOURCE = {
    "bundleUuid": "12345678-1234-5678-9abc-def012345678",
    "sourceLanguage": "de",
    "targetLanguage": "de",
    "license": {
        "spdx": "CC-BY-SA-4.0",
        "sourceUrl": "https://crossink.example/de-definition-test",
        "attribution": "Synthetic German definitions.",
    },
    "lexemes": [
        {
            "headword": "Laden",
            "partOfSpeech": "noun",
            "forms": ["Läden"],
            "fields": [
                {"type": "definition", "text": "Geschäft"},
                {"type": "example", "text": "Der Laden ist offen."},
            ],
        },
        {
            "headword": "laden",
            "partOfSpeech": "verb",
            "forms": ["lädt"],
            "fields": [{"type": "definition", "text": "mit Fracht versehen"}],
        },
        {
            "headword": "Goethe",
            "partOfSpeech": "proper-noun",
            "forms": [],
            "fields": [{"type": "definition", "text": "deutscher Dichter"}],
        },
    ],
}


class DeDeDefinitionsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.dictionary_path = Path(self.temporary.name) / "de.cpdict"
        self.dictionary_path.write_bytes(compile_bundle(SOURCE).archive_bytes)
        canonical_bundle = compile_de_de_canonical_bundle(self.dictionary_path)
        self.canonical_path = Path(self.temporary.name) / "canonical.cplex"
        self.canonical_path.write_bytes(canonical_bundle.archive_bytes)
        self.canonical = load_canonical_lexicon_index(self.canonical_path)

    def tearDown(self):
        self.temporary.cleanup()

    def test_aligns_every_definition_to_same_canonical_identity(self):
        compiled = compile_de_de_definition_source(self.dictionary_path, self.canonical)
        self.assertEqual(compiled.coverage_count, 3)
        self.assertEqual(compiled.unmatched_count, 0)
        meta = compiled.files["device/meta.bin"]
        index = compiled.files["device/entry-index.bin"]
        entries = compiled.files["device/entries.bin"]
        self.assertEqual(meta[28:44], self.canonical.canonical_uuid.bytes)
        self.assertEqual(struct.unpack_from("<I", meta, 116)[0], 3)
        self.assertEqual(
            validate_definition_files(
                meta,
                index,
                entries,
                self.canonical.canonical_uuid,
                self.canonical.lexeme_count,
            ),
            compiled.source_uuid,
        )
        report = json.loads(compiled.files["compiler/coverage.json"])
        self.assertEqual(report["coveragePercent"], 100.0)
        self.assertEqual(report["provenance"]["sourceBundleUuid"], SOURCE["bundleUuid"])
        self.assertEqual(report["provenance"]["license"]["spdx"], "CC-BY-SA-4.0")

        laden_id = self.canonical.resolve(
            SOURCE["lexemes"][0]["headword"],
            CanonicalPos.NOUN,
        )
        offset, length = struct.unpack_from("<II", index, laden_id * 8)
        entry = entries[offset : offset + length]
        self.assertIn("Geschäft".encode(), entry)
        self.assertIn("Der Laden ist offen.".encode(), entry)

    def test_build_is_byte_identical(self):
        first = compile_de_de_definition_source(self.dictionary_path, self.canonical)
        second = compile_de_de_definition_source(self.dictionary_path, self.canonical)
        self.assertEqual(first.archive_bytes, second.archive_bytes)

    def test_rejects_corrupt_definition_payload_before_alignment(self):
        corrupt = Path(self.temporary.name) / "corrupt.cpdict"
        with zipfile.ZipFile(self.dictionary_path) as source, zipfile.ZipFile(corrupt, "w") as target:
            for name in source.namelist():
                data = source.read(name)
                if name == "device/entries.bin":
                    data = data[:-1] + bytes((data[-1] ^ 0xFF,))
                target.writestr(name, data)
        with self.assertRaisesRegex(DeDeDefinitionError, "manifest hash mismatch"):
            compile_de_de_definition_source(corrupt, self.canonical)


if __name__ == "__main__":
    unittest.main()

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
from dictionary.contextual.kaikki_definitions import (  # noqa: E402
    compile_kaikki_definition_source,
)


class KaikkiDefinitionsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        bundle = compile_canonical_bundle(
            (
                CanonicalLexemeInput("Goethe", CanonicalPos.PROPER_NOUN),
                CanonicalLexemeInput("Laden", CanonicalPos.NOUN),
                CanonicalLexemeInput("frei", CanonicalPos.ADJECTIVE),
                CanonicalLexemeInput("laden", CanonicalPos.VERB),
            ),
            {"fixture": "kaikki"},
            "license",
        )
        path = Path(self.temporary.name) / "canonical.cplex"
        path.write_bytes(bundle.archive_bytes)
        self.canonical = load_canonical_lexicon_index(path)

    def tearDown(self):
        self.temporary.cleanup()

    def source(self, records, name="kaikki.jsonl"):
        path = Path(self.temporary.name) / name
        with path.open("wb") as output:
            for record in records:
                if isinstance(record, bytes):
                    output.write(record + b"\n")
                else:
                    output.write(json.dumps(record, ensure_ascii=False).encode("utf-8") + b"\n")
        return path

    def test_maps_senses_qualifiers_examples_etymology_and_pos(self):
        records = (
            {
                "word": "Laden",
                "lang": "German",
                "lang_code": "de",
                "pos": "noun",
                "etymology_text": "From Middle High German laden.",
                "senses": [
                    {
                        "glosses": ["shop, store"],
                        "tags": ["masculine", "informal"],
                        "topics": ["commerce"],
                        "examples": [
                            {
                                "text": "Der Laden ist offen.",
                                "translation": "The shop is open.",
                            }
                        ],
                    }
                ],
            },
            {
                "word": "Goethe",
                "lang_code": "de",
                "pos": "name",
                "senses": [{"glosses": ["a German surname"]}],
            },
            {
                "word": "laden",
                "lang_code": "de",
                "pos": "verb",
                "senses": [{"raw_glosses": ["(transitive) to load"]}],
            },
            {"word": "missing", "lang_code": "de", "pos": "noun", "senses": [{"glosses": ["x"]}]},
            {"word": "Laden", "lang_code": "de", "pos": "verb", "senses": [{"glosses": ["to load"]}]},
            {"word": "bad", "lang_code": "fr", "pos": "noun", "senses": [{"glosses": ["x"]}]},
            b"{not-json",
        )
        compiled, stats = compile_kaikki_definition_source(self.source(records), self.canonical)
        self.assertEqual(compiled.coverage_count, 3)
        self.assertEqual(stats.records, 7)
        self.assertEqual(stats.aligned_records, 3)
        self.assertEqual(stats.unaligned_records, 2)
        self.assertEqual(stats.pos_conflict_records, 1)
        self.assertEqual(stats.malformed_records, 2)
        self.assertEqual(stats.senses, 5)
        self.assertEqual(stats.glosses, 5)
        self.assertEqual(stats.examples, 1)
        payload = compiled.files["device/entries.bin"]
        self.assertIn(b"shop, store", payload)
        self.assertIn("Der Laden ist offen. — The shop is open.".encode(), payload)
        self.assertIn(b"commerce, informal, masculine", payload)
        self.assertIn(b"From Middle High German laden.", payload)
        report = json.loads(compiled.files["compiler/coverage.json"])
        self.assertEqual(report["provenance"]["import"], stats.as_dict())
        self.assertEqual(report["provenance"]["license"], "CC-BY-SA-4.0")

    def test_caps_pathological_sense_sets_independent_of_record_order(self):
        records = [
            {
                "word": "laden",
                "lang_code": "de",
                "pos": "verb",
                "senses": [{"glosses": [f"gloss {index:03d}"]}],
            }
            for index in range(140)
        ]
        first, _ = compile_kaikki_definition_source(self.source(records, "first.jsonl"), self.canonical)
        second, _ = compile_kaikki_definition_source(
            self.source(tuple(reversed(records)), "second.jsonl"),
            self.canonical,
        )
        self.assertEqual(first.source_uuid, second.source_uuid)
        self.assertEqual(first.files["device/entries.bin"], second.files["device/entries.bin"])
        self.assertEqual(first.truncated_field_count, 12)
        self.assertIn(b"gloss 000", first.files["device/entries.bin"])
        self.assertNotIn(b"gloss 139", first.files["device/entries.bin"])

    def test_same_jsonl_builds_byte_identically(self):
        path = self.source(
            (
                {
                    "word": "frei",
                    "lang_code": "de",
                    "pos": "adj",
                    "senses": [{"glosses": ["free"]}],
                },
            )
        )
        first, _ = compile_kaikki_definition_source(path, self.canonical)
        second, _ = compile_kaikki_definition_source(path, self.canonical)
        self.assertEqual(first.archive_bytes, second.archive_bytes)

    def test_oversized_and_empty_records_are_counted_not_fatal(self):
        oversized = b'{"word":"' + b"x" * (256 * 1024) + b'"}'
        path = self.source(
            (
                oversized,
                {"word": "frei", "lang_code": "de", "pos": "adj", "senses": []},
                {"word": "frei", "lang_code": "de", "pos": "adj", "senses": [{"glosses": ["free"]}]},
            )
        )
        compiled, stats = compile_kaikki_definition_source(path, self.canonical)
        self.assertEqual(compiled.coverage_count, 1)
        self.assertEqual(stats.malformed_records, 2)


if __name__ == "__main__":
    unittest.main()

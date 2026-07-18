import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.analysis_policy import CanonicalPos  # noqa: E402
from dictionary.contextual.canonical_lexicon import (  # noqa: E402
    CanonicalLexemeInput,
    compile_canonical_bundle,
    load_canonical_lexicon_index,
)
from dictionary.contextual.coverage_report import (  # noqa: E402
    CoverageReportError,
    build_coverage_report,
)
from dictionary.contextual.definition_source import (  # noqa: E402
    DefinitionEntryInput,
    DefinitionFieldInput,
    compile_definition_source,
)


PRODUCTION_REPORT = ROOT / "test" / "data" / "contextual" / "definition-coverage.json"


class CoverageReportTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        bundle = compile_canonical_bundle(
            (
                CanonicalLexemeInput("Goethe", CanonicalPos.PROPER_NOUN),
                CanonicalLexemeInput("Laden", CanonicalPos.NOUN),
                CanonicalLexemeInput("laden", CanonicalPos.VERB),
                CanonicalLexemeInput("stehen", CanonicalPos.VERB),
            ),
            {"fixture": "coverage"},
            "license",
        )
        path = Path(self.temporary.name) / "canonical.cplex"
        path.write_bytes(bundle.archive_bytes)
        self.canonical = load_canonical_lexicon_index(path)

    def tearDown(self):
        self.temporary.cleanup()

    def source(self, name, keys, import_stats=None):
        entries = tuple(
            DefinitionEntryInput(
                headword,
                pos,
                (DefinitionFieldInput(1, text),),
            )
            for headword, pos, text in keys
        )
        provenance = {"fixture": name}
        if import_stats is not None:
            provenance["import"] = import_stats
        compiled = compile_definition_source(
            self.canonical,
            entries,
            "de",
            "en",
            name,
            "license",
            provenance,
        )
        path = Path(self.temporary.name) / f"{name}.cpdef"
        path.write_bytes(compiled.archive_bytes)
        return path

    def test_reports_union_intersections_pos_conflicts_and_entry_sizes(self):
        sources = {
            "a": self.source(
                "a",
                (
                    ("Goethe", CanonicalPos.PROPER_NOUN, "surname"),
                    ("Laden", CanonicalPos.NOUN, "shop"),
                ),
            ),
            "b": self.source(
                "b",
                (
                    ("Laden", CanonicalPos.NOUN, "a much longer store definition"),
                    ("laden", CanonicalPos.VERB, "to load"),
                ),
                {"unalignedLines": 9, "posConflictLines": 3},
            ),
            "c": self.source(
                "c",
                (("laden", CanonicalPos.VERB, "load"),),
                {"unalignedRecords": 7, "posConflictRecords": 2},
            ),
        }
        report = build_coverage_report(self.canonical, sources)
        self.assertEqual(report["canonicalLexemeCount"], 4)
        self.assertEqual(report["union"]["covered"], 3)
        self.assertEqual(report["union"]["percent"], 75.0)
        self.assertEqual(report["union"]["allSources"], 0)
        self.assertEqual(
            report["union"]["intersections"],
            {"a&b": 1, "a&c": 0, "b&c": 1},
        )
        self.assertEqual(report["union"]["sourceOnly"], {"a": 1, "b": 0, "c": 0})
        self.assertEqual(report["sources"]["b"]["unalignedInputCount"], 9)
        self.assertEqual(report["sources"]["b"]["posConflictInputCount"], 3)
        self.assertEqual(report["sources"]["c"]["posConflictInputCount"], 2)
        self.assertEqual(report["sources"]["b"]["byPartOfSpeech"]["verb"]["covered"], 1)
        self.assertEqual(report["sources"]["b"]["largestEntries"][0]["headword"], "Laden")
        self.assertGreater(report["sources"]["b"]["entrySizeBytes"]["max"], 4)

    def test_checked_in_production_report_records_three_source_conflicts(self):
        report = json.loads(PRODUCTION_REPORT.read_text(encoding="utf-8"))
        self.assertEqual(report["canonicalLexemeCount"], 181609)
        self.assertEqual(
            {name: source["coverage"] for name, source in report["sources"].items()},
            {"de-de": 181609, "dictcc": 105036, "kaikki": 62710},
        )
        self.assertEqual(report["sources"]["dictcc"]["posConflictInputCount"], 20621)
        self.assertEqual(report["sources"]["kaikki"]["posConflictInputCount"], 5488)
        self.assertEqual(report["union"]["covered"], 181609)
        self.assertEqual(report["union"]["allSources"], 52491)
        self.assertEqual(report["union"]["intersections"]["dictcc&kaikki"], 52491)

    def test_rejects_source_payload_corruption(self):
        source = self.source("valid", (("Laden", CanonicalPos.NOUN, "shop"),))
        corrupt = Path(self.temporary.name) / "corrupt.cpdef"
        with zipfile.ZipFile(source) as archive, zipfile.ZipFile(corrupt, "w") as output:
            for name in archive.namelist():
                data = archive.read(name)
                if name == "device/entries.bin":
                    data += b"x"
                output.writestr(name, data)
        with self.assertRaisesRegex(CoverageReportError, "hash mismatch"):
            build_coverage_report(self.canonical, {"bad": corrupt})


if __name__ == "__main__":
    unittest.main()

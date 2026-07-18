import hashlib
import json
from pathlib import Path
import random
import struct
import sys
import tempfile
import unittest
import uuid
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.analysis_policy import CanonicalPos  # noqa: E402
from dictionary.contextual.canonical_lexicon import (  # noqa: E402
    CanonicalLexemeInput,
    compile_canonical_bundle,
    load_canonical_lexicon_index,
)
from dictionary.contextual.definition_source import (  # noqa: E402
    SOURCE_NAMESPACE,
    DefinitionEntryInput,
    DefinitionFieldInput,
    DefinitionSourceError,
    compile_definition_source,
    validate_definition_files,
)


class DefinitionSourceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        canonical_bundle = compile_canonical_bundle(
            (
                CanonicalLexemeInput("Goethe", CanonicalPos.PROPER_NOUN),
                CanonicalLexemeInput("Laden", CanonicalPos.NOUN),
                CanonicalLexemeInput("laden", CanonicalPos.VERB),
                CanonicalLexemeInput("stehen", CanonicalPos.VERB),
            ),
            {"fixture": "definition-source"},
            "SPDX-License-Identifier: CC0-1.0\n",
        )
        canonical_path = Path(self.temporary.name) / "canonical.cplex"
        canonical_path.write_bytes(canonical_bundle.archive_bytes)
        self.canonical = load_canonical_lexicon_index(canonical_path)
        self.entries = [
            DefinitionEntryInput(
                "laden",
                CanonicalPos.VERB,
                (
                    DefinitionFieldInput(1, "beladen"),
                    DefinitionFieldInput(3, "Wir laden die Kisten."),
                ),
            ),
            DefinitionEntryInput(
                "Laden",
                CanonicalPos.NOUN,
                (DefinitionFieldInput(1, "Geschäft"),),
            ),
            DefinitionEntryInput(
                "Fehlen",
                CanonicalPos.NOUN,
                (DefinitionFieldInput(1, "nicht vorhanden"),),
            ),
            DefinitionEntryInput(
                "Laden",
                CanonicalPos.NOUN,
                (
                    DefinitionFieldInput(1, "Geschäft"),
                    DefinitionFieldInput(3, "Der Laden ist offen."),
                ),
            ),
        ]

    def tearDown(self):
        self.temporary.cleanup()

    def compile(self, entries=None):
        return compile_definition_source(
            self.canonical,
            self.entries if entries is None else entries,
            "de",
            "en",
            "Fixture de-en",
            "SPDX-License-Identifier: CC0-1.0\n",
            {"fixture": "definitions"},
        )

    def test_fixed_index_entries_crc_and_uuid_follow_contract(self):
        compiled = self.compile()
        meta = compiled.files["device/meta.bin"]
        index = compiled.files["device/entry-index.bin"]
        entries = compiled.files["device/entries.bin"]
        self.assertEqual((meta[:4], len(meta)), (b"CXDS", 144))
        self.assertEqual(struct.unpack_from("<HHI", meta, 4), (1, 144, 0))
        self.assertEqual(meta[28:44], self.canonical.canonical_uuid.bytes)
        self.assertEqual(struct.unpack_from("<IHH", meta, 92), (4, 8, 1))
        self.assertEqual(struct.unpack_from("<II", meta, 100), (len(index), len(entries)))
        self.assertEqual(struct.unpack_from("<II", meta, 108), (
            zlib.crc32(index) & 0xFFFFFFFF,
            zlib.crc32(entries) & 0xFFFFFFFF,
        ))
        self.assertEqual(struct.unpack_from("<I", meta, 116)[0], 2)
        self.assertEqual(struct.unpack_from("<I", meta, 140)[0], zlib.crc32(meta[:140]) & 0xFFFFFFFF)
        self.assertEqual(len(index), self.canonical.lexeme_count * 8)

        fingerprint = hashlib.sha256(index + entries).hexdigest()
        expected_uuid = uuid.uuid5(
            SOURCE_NAMESPACE,
            f"{self.canonical.canonical_uuid.hex}:{fingerprint}",
        )
        self.assertEqual(compiled.source_uuid, expected_uuid)
        self.assertEqual(meta[12:28], expected_uuid.bytes)
        self.assertEqual(
            validate_definition_files(
                meta,
                index,
                entries,
                self.canonical.canonical_uuid,
                self.canonical.lexeme_count,
            ),
            expected_uuid,
        )

        records = [struct.unpack_from("<II", index, offset) for offset in range(0, len(index), 8)]
        self.assertEqual(records[0], (0, 0))
        self.assertNotEqual(records[1], (0, 0))
        self.assertNotEqual(records[2], (0, 0))
        self.assertEqual(records[3], (0, 0))
        laden_offset, laden_length = records[1]
        laden_entry = entries[laden_offset : laden_offset + laden_length]
        self.assertEqual(struct.unpack_from("<BBH", laden_entry), (1, 0, 2))

    def test_input_order_and_duplicate_fields_are_byte_deterministic(self):
        first = self.compile()
        shuffled = list(self.entries)
        random.Random(19).shuffle(shuffled)
        second = self.compile(shuffled)
        self.assertEqual(first.archive_bytes, second.archive_bytes)
        self.assertEqual(first.coverage_count, 2)
        self.assertEqual(first.unmatched_count, 1)
        report = json.loads(first.files["compiler/coverage.json"])
        self.assertEqual(report["coverageCount"], 2)
        self.assertEqual(report["unmatchedCount"], 1)
        self.assertEqual(report["unmatchedExamples"][0]["headword"], "Fehlen")

    def test_archive_manifest_hashes_every_distribution_file(self):
        compiled = self.compile()
        path = Path(self.temporary.name) / "source.cpdef"
        path.write_bytes(compiled.archive_bytes)
        with zipfile.ZipFile(path) as archive:
            self.assertEqual(archive.namelist(), sorted(archive.namelist()))
            manifest = json.loads(archive.read("manifest.json"))
            self.assertEqual(manifest["sourceUuid"], str(compiled.source_uuid))
            self.assertEqual(manifest["canonicalUuid"], str(self.canonical.canonical_uuid))
            self.assertEqual(manifest["coverageCount"], 2)
            self.assertEqual(set(manifest["files"]), set(archive.namelist()) - {"manifest.json"})
            for name, expected in manifest["files"].items():
                data = archive.read(name)
                self.assertEqual(expected["bytes"], len(data))
                self.assertEqual(expected["sha256"], hashlib.sha256(data).hexdigest())

    def test_validator_rejects_crc_uuid_index_and_entry_corruption(self):
        compiled = self.compile()
        original = compiled.files
        meta = bytearray(original["device/meta.bin"])
        meta[60] ^= 1
        with self.assertRaisesRegex(DefinitionSourceError, "metadata CRC"):
            validate_definition_files(
                bytes(meta),
                original["device/entry-index.bin"],
                original["device/entries.bin"],
                self.canonical.canonical_uuid,
                self.canonical.lexeme_count,
            )

        meta = bytearray(original["device/meta.bin"])
        meta[12] ^= 1
        struct.pack_into("<I", meta, 140, zlib.crc32(meta[:140]) & 0xFFFFFFFF)
        with self.assertRaisesRegex(DefinitionSourceError, "fingerprint UUID"):
            validate_definition_files(
                bytes(meta),
                original["device/entry-index.bin"],
                original["device/entries.bin"],
                self.canonical.canonical_uuid,
                self.canonical.lexeme_count,
            )

        index = bytearray(original["device/entry-index.bin"])
        struct.pack_into("<II", index, 0, 1, 0)
        meta = bytearray(original["device/meta.bin"])
        struct.pack_into("<I", meta, 108, zlib.crc32(index) & 0xFFFFFFFF)
        fingerprint = hashlib.sha256(index + original["device/entries.bin"]).hexdigest()
        source_uuid = uuid.uuid5(
            SOURCE_NAMESPACE,
            f"{self.canonical.canonical_uuid.hex}:{fingerprint}",
        )
        meta[12:28] = source_uuid.bytes
        struct.pack_into("<I", meta, 140, zlib.crc32(meta[:140]) & 0xFFFFFFFF)
        with self.assertRaisesRegex(DefinitionSourceError, "missing definition"):
            validate_definition_files(
                bytes(meta),
                bytes(index),
                original["device/entries.bin"],
                self.canonical.canonical_uuid,
                self.canonical.lexeme_count,
            )

        entries = bytearray(original["device/entries.bin"])
        entries[0] = 2
        meta = bytearray(original["device/meta.bin"])
        struct.pack_into("<I", meta, 112, zlib.crc32(entries) & 0xFFFFFFFF)
        fingerprint = hashlib.sha256(original["device/entry-index.bin"] + entries).hexdigest()
        source_uuid = uuid.uuid5(
            SOURCE_NAMESPACE,
            f"{self.canonical.canonical_uuid.hex}:{fingerprint}",
        )
        meta[12:28] = source_uuid.bytes
        struct.pack_into("<I", meta, 140, zlib.crc32(meta[:140]) & 0xFFFFFFFF)
        with self.assertRaisesRegex(DefinitionSourceError, "entry header"):
            validate_definition_files(
                bytes(meta),
                original["device/entry-index.bin"],
                bytes(entries),
                self.canonical.canonical_uuid,
                self.canonical.lexeme_count,
            )

    def test_rejects_caps_and_malformed_inputs(self):
        with self.assertRaisesRegex(DefinitionSourceError, "source label exceeds"):
            compile_definition_source(
                self.canonical,
                self.entries,
                "de",
                "en",
                "x" * 32,
                "license",
                {},
            )
        oversized = DefinitionEntryInput(
            "Laden",
            CanonicalPos.NOUN,
            (DefinitionFieldInput(1, "x" * 65_536),),
        )
        with self.assertRaisesRegex(DefinitionSourceError, "exceeds 65535"):
            self.compile((oversized,))
        malformed = DefinitionEntryInput(
            "Laden",
            CanonicalPos.NOUN,
            (DefinitionFieldInput(9, "invalid"),),
        )
        with self.assertRaisesRegex(DefinitionSourceError, "invalid field type"):
            self.compile((malformed,))


if __name__ == "__main__":
    unittest.main()

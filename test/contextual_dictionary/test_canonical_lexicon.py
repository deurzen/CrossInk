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

from dictionary.compiler import compile_bundle  # noqa: E402
from dictionary.contextual.analysis_policy import CanonicalPos  # noqa: E402
from dictionary.contextual.canonical_lexicon import (  # noqa: E402
    CANONICAL_NAMESPACE,
    CanonicalLexemeInput,
    CanonicalLexiconError,
    compile_canonical_bundle,
    compile_de_de_canonical_bundle,
    load_de_de_seed,
)


SEED_SOURCE = {
    "bundleUuid": "12345678-1234-5678-9abc-def012345678",
    "sourceLanguage": "de",
    "targetLanguage": "de",
    "license": {
        "spdx": "CC0-1.0",
        "sourceUrl": "https://crossink.example/canonical-test",
        "attribution": "Synthetic canonical seed.",
    },
    "lexemes": [
        {
            "headword": "laden",
            "partOfSpeech": "verb",
            "forms": ["lädt"],
            "fields": [{"type": "definition", "text": "synthetic"}],
        },
        {
            "headword": "Laden",
            "partOfSpeech": "noun",
            "forms": ["Läden"],
            "fields": [{"type": "definition", "text": "synthetic"}],
        },
        {
            "headword": "Goethe",
            "partOfSpeech": "proper-noun",
            "forms": [],
            "fields": [{"type": "definition", "text": "synthetic"}],
        },
    ],
}


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


class CanonicalLexiconTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.seed_path = Path(self.temporary.name) / "seed.cpdict"
        self.seed_path.write_bytes(compile_bundle(SEED_SOURCE).archive_bytes)

    def tearDown(self):
        self.temporary.cleanup()

    def test_loads_only_verified_de_de_lexical_identity(self):
        seed = load_de_de_seed(self.seed_path)
        self.assertEqual(len(seed.lexemes), 3)
        self.assertEqual(seed.provenance["bundleUuid"], SEED_SOURCE["bundleUuid"])
        self.assertEqual(seed.provenance["license"]["spdx"], "CC0-1.0")
        self.assertEqual(
            seed.provenance["archiveSha256"],
            hashlib.sha256(self.seed_path.read_bytes()).hexdigest(),
        )
        self.assertEqual(
            [(item.headword, item.part_of_speech) for item in seed.lexemes],
            [
                ("Goethe", CanonicalPos.PROPER_NOUN),
                ("Laden", CanonicalPos.NOUN),
                ("laden", CanonicalPos.VERB),
            ],
        )

    def test_compiler_is_byte_identical_across_input_order_and_duplicates(self):
        values = [
            CanonicalLexemeInput("laden", CanonicalPos.VERB),
            CanonicalLexemeInput("Laden", CanonicalPos.NOUN),
            CanonicalLexemeInput("Ähre", CanonicalPos.NOUN, 0x01),
            CanonicalLexemeInput("A\u0308hre", CanonicalPos.NOUN, 0x02),
        ]
        provenance = {"fixture": "canonical-order"}
        first = compile_canonical_bundle(values, provenance, "SPDX-License-Identifier: CC0-1.0\n")
        random.Random(7).shuffle(values)
        second = compile_canonical_bundle(values, provenance, "SPDX-License-Identifier: CC0-1.0\n")
        self.assertEqual(first.archive_bytes, second.archive_bytes)
        self.assertEqual(first.lexeme_count, 3)

        records = first.files["runtime/lexemes.bin"]
        headwords = first.files["runtime/headwords.bin"]
        decoded = []
        for offset in range(0, len(records), 16):
            headword_offset, _, length, pos, flags = struct.unpack_from("<IQHBB", records, offset)
            decoded.append((headwords[headword_offset : headword_offset + length].decode(), pos, flags))
        self.assertEqual(
            decoded,
            [
                ("Laden", int(CanonicalPos.NOUN), 0),
                ("laden", int(CanonicalPos.VERB), 0),
                ("Ähre", int(CanonicalPos.NOUN), 0x03),
            ],
        )

    def test_runtime_header_uuid_and_compiler_provenance_match_contract(self):
        bundle = compile_de_de_canonical_bundle(self.seed_path)
        meta = bundle.files["runtime/meta.bin"]
        records = bundle.files["runtime/lexemes.bin"]
        headwords = bundle.files["runtime/headwords.bin"]
        self.assertEqual((meta[:4], len(meta)), (b"CXCL", 112))
        self.assertEqual(struct.unpack_from("<HHI", meta, 4), (1, 112, 0))
        self.assertEqual(meta[28:36].rstrip(b"\0"), b"de")
        self.assertEqual(struct.unpack_from("<IHH", meta, 36), (3, 16, 1))
        self.assertEqual(struct.unpack_from("<II", meta, 44), (len(records), len(headwords)))
        self.assertEqual(struct.unpack_from("<II", meta, 52), (crc32(records), crc32(headwords)))
        fingerprint = hashlib.sha256(records + headwords).digest()
        self.assertEqual(meta[60:92], fingerprint)
        self.assertEqual(struct.unpack_from("<I", meta, 108)[0], crc32(meta[:108]))
        expected_uuid = uuid.uuid5(CANONICAL_NAMESPACE, fingerprint.hex())
        self.assertEqual(bundle.canonical_uuid, expected_uuid)
        self.assertEqual(meta[12:28], expected_uuid.bytes)

        analyzer = json.loads(bundle.files["compiler/analyzer.json"])
        self.assertEqual(analyzer["canonicalPosVersion"], 1)
        self.assertEqual(analyzer["analysisPolicyVersion"], 2)
        self.assertEqual(analyzer["seed"], bundle_manifest(bundle)["seed"])
        dwdsmor = json.loads(bundle.files["compiler/dwdsmor-open.json"])
        zdl = json.loads(bundle.files["compiler/zdl-model.json"])
        self.assertEqual(dwdsmor["package"]["version"], "0.18.0")
        self.assertEqual(zdl["model"]["version"], "4.0.0")

    def test_archive_manifest_covers_all_files_and_has_reproducible_metadata(self):
        bundle = compile_de_de_canonical_bundle(self.seed_path)
        with zipfile.ZipFile(self.seed_path) as seed_archive:
            self.assertIn("compiler/forms.bin", seed_archive.namelist())
        archive_path = Path(self.temporary.name) / "canonical.cplex"
        archive_path.write_bytes(bundle.archive_bytes)
        with zipfile.ZipFile(archive_path) as archive:
            self.assertEqual(
                archive.namelist(),
                sorted(archive.namelist()),
            )
            self.assertNotIn("compiler/forms.bin", archive.namelist())
            self.assertTrue(all(info.date_time == (1980, 1, 1, 0, 0, 0) for info in archive.infolist()))
            manifest = json.loads(archive.read("manifest.json"))
            self.assertEqual(manifest["canonicalUuid"], str(bundle.canonical_uuid))
            self.assertEqual(manifest["payloadSha256"], bundle.payload_sha256)
            self.assertEqual(manifest["analysisPolicyVersion"], 2)
            self.assertEqual(set(manifest["files"]), set(archive.namelist()) - {"manifest.json"})
            for name, expected in manifest["files"].items():
                data = archive.read(name)
                self.assertEqual(expected["bytes"], len(data))
                self.assertEqual(expected["sha256"], hashlib.sha256(data).hexdigest())

    def test_rejects_bad_inputs_direction_and_manifest_hash(self):
        bad_values = (
            (CanonicalLexemeInput("", CanonicalPos.NOUN), "invalid headword"),
            (CanonicalLexemeInput("x" * 97, CanonicalPos.NOUN), "exceeds 96"),
            (CanonicalLexemeInput("wort", CanonicalPos.NOUN, 0x80), "invalid flags"),
        )
        for value, message in bad_values:
            with self.subTest(message=message):
                with self.assertRaisesRegex(CanonicalLexiconError, message):
                    compile_canonical_bundle((value,), {}, "license")

        wrong_source = dict(SEED_SOURCE)
        wrong_source["targetLanguage"] = "en"
        wrong_path = Path(self.temporary.name) / "wrong.cpdict"
        wrong_path.write_bytes(compile_bundle(wrong_source).archive_bytes)
        with self.assertRaisesRegex(CanonicalLexiconError, "de-DE"):
            load_de_de_seed(wrong_path)

        corrupt_path = Path(self.temporary.name) / "corrupt.cpdict"
        with zipfile.ZipFile(self.seed_path) as source, zipfile.ZipFile(corrupt_path, "w") as target:
            for name in source.namelist():
                data = source.read(name)
                if name == "device/headwords.bin":
                    data += b"x"
                target.writestr(name, data)
        with self.assertRaisesRegex(CanonicalLexiconError, "size mismatch"):
            load_de_de_seed(corrupt_path)


def bundle_manifest(bundle):
    return json.loads(bundle.files["manifest.json"])


if __name__ == "__main__":
    unittest.main()

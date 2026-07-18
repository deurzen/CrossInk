import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import unittest
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.build_format_fixtures import (  # noqa: E402
    CANONICAL_NAMESPACE,
    SOURCE_NAMESPACE,
)

FIXTURES = ROOT / "test" / "data" / "contextual" / "formats"
BUILDER = ROOT / "scripts" / "dictionary" / "contextual" / "build_format_fixtures.py"


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


class ContextualFormatTest(unittest.TestCase):
    def test_checked_in_fixtures_are_reproducible(self):
        subprocess.run(
            [sys.executable, str(BUILDER), "--check"],
            check=True,
            capture_output=True,
            text=True,
        )

    def test_canonical_header_payload_and_identity(self):
        meta = (FIXTURES / "canonical-meta.bin").read_bytes()
        lexemes = (FIXTURES / "canonical-lexemes.bin").read_bytes()
        headwords = (FIXTURES / "canonical-headwords.bin").read_bytes()
        self.assertEqual((meta[:4], len(meta)), (b"CXCL", 112))
        self.assertEqual(struct.unpack_from("<HH", meta, 4), (1, 112))
        self.assertEqual(struct.unpack_from("<I", meta, 36)[0], 3)
        self.assertEqual(struct.unpack_from("<H", meta, 40)[0], 16)
        self.assertEqual(struct.unpack_from("<II", meta, 44), (len(lexemes), len(headwords)))
        self.assertEqual(struct.unpack_from("<II", meta, 52), (crc32(lexemes), crc32(headwords)))
        fingerprint = hashlib.sha256(lexemes + headwords).digest()
        self.assertEqual(meta[60:92], fingerprint)
        self.assertEqual(struct.unpack_from("<I", meta, 108)[0], crc32(meta[:108]))
        self.assertEqual(uuid.UUID(bytes=meta[12:28]), uuid.uuid5(CANONICAL_NAMESPACE, fingerprint.hex()))

        last_key = None
        for offset in range(0, len(lexemes), 16):
            headword_offset, key_hash, length, pos, flags = struct.unpack_from("<IQHBB", lexemes, offset)
            headword = headwords[headword_offset : headword_offset + length]
            self.assertEqual(flags, 0)
            key = (headword, pos)
            if last_key is not None:
                self.assertLess(last_key, key)
            last_key = key
            self.assertNotEqual(key_hash, 0)

    def test_definition_header_index_and_identity(self):
        canonical_meta = (FIXTURES / "canonical-meta.bin").read_bytes()
        meta = (FIXTURES / "definition-meta.bin").read_bytes()
        index = (FIXTURES / "definition-index.bin").read_bytes()
        entries = (FIXTURES / "definition-entries.bin").read_bytes()
        self.assertEqual((meta[:4], len(meta)), (b"CXDS", 144))
        self.assertEqual(meta[28:44], canonical_meta[12:28])
        self.assertEqual(struct.unpack_from("<I", meta, 92)[0], 3)
        self.assertEqual(struct.unpack_from("<H", meta, 96)[0], 8)
        self.assertEqual(struct.unpack_from("<II", meta, 100), (len(index), len(entries)))
        self.assertEqual(struct.unpack_from("<II", meta, 108), (crc32(index), crc32(entries)))
        self.assertEqual(struct.unpack_from("<I", meta, 116)[0], 2)
        self.assertEqual(struct.unpack_from("<I", meta, 140)[0], crc32(meta[:140]))

        canonical_uuid = uuid.UUID(bytes=canonical_meta[12:28])
        fingerprint = hashlib.sha256(index + entries).hexdigest()
        expected_uuid = uuid.uuid5(SOURCE_NAMESPACE, f"{canonical_uuid.hex}:{fingerprint}")
        self.assertEqual(uuid.UUID(bytes=meta[12:28]), expected_uuid)
        records = [struct.unpack_from("<II", index, offset) for offset in range(0, len(index), 8)]
        self.assertEqual(records[1], (0, 0))
        for offset, length in (records[0], records[2]):
            self.assertGreaterEqual(length, 4)
            self.assertLessEqual(offset + length, len(entries))
            self.assertEqual(entries[offset], 1)

    def test_attachment_and_language_reference_canonical_identity(self):
        canonical_uuid = (FIXTURES / "canonical-meta.bin").read_bytes()[12:28]
        source_uuid = (FIXTURES / "definition-meta.bin").read_bytes()[12:28]
        attachments = (FIXTURES / "attachments.bin").read_bytes()
        self.assertEqual((attachments[:4], len(attachments)), (b"CXAT", 88))
        self.assertEqual(attachments[12:28], canonical_uuid)
        self.assertEqual(attachments[32], 1)
        self.assertEqual(attachments[36:52], source_uuid)
        self.assertEqual(attachments[52:84], bytes(32))
        self.assertEqual(struct.unpack_from("<I", attachments, 84)[0], crc32(attachments[:84]))

        language = (FIXTURES / "language-v4.bin").read_bytes()
        self.assertEqual(language[:4], b"CXLG")
        self.assertEqual(struct.unpack_from("<HH", language, 4), (4, 108))
        self.assertEqual(language[16:32], canonical_uuid)
        self.assertEqual(language[40:48].rstrip(b"\0"), b"und")
        self.assertEqual(struct.unpack_from("<I", language, 96)[0], len(language))
        self.assertEqual(struct.unpack_from("<I", language, 100)[0], crc32(language[108:]))
        self.assertEqual(struct.unpack_from("<I", language, 104)[0], crc32(language[:104]))

        metadata_offset = struct.unpack_from("<I", language, 84)[0]
        self.assertEqual(language[metadata_offset : metadata_offset + 4], b"CXLM")
        json_length = struct.unpack_from("<I", language, metadata_offset + 8)[0]
        metadata = json.loads(language[metadata_offset + 16 : metadata_offset + 16 + json_length])
        self.assertEqual(metadata["canonicalUuid"], str(uuid.UUID(bytes=canonical_uuid)))
        self.assertEqual(metadata["analysisPolicyVersion"], 2)

    def test_checksum_manifest_covers_every_binary(self):
        manifest = json.loads((FIXTURES / "checksums.json").read_text(encoding="utf-8"))
        binaries = sorted(path.name for path in FIXTURES.glob("*.bin"))
        self.assertEqual(sorted(manifest), binaries)
        for name, expected in manifest.items():
            data = (FIXTURES / name).read_bytes()
            self.assertEqual(expected["bytes"], len(data))
            self.assertEqual(expected["sha256"], hashlib.sha256(data).hexdigest())

    def test_corruption_cases_are_bounded_and_deterministic(self):
        fixture = json.loads((FIXTURES / "corruption-cases.json").read_text(encoding="utf-8"))
        self.assertEqual(fixture["schemaVersion"], 1)
        names = set()
        for case in fixture["cases"]:
            self.assertNotIn(case["name"], names)
            names.add(case["name"])
            original = (FIXTURES / case["file"]).read_bytes()
            mutated = self.apply_mutation(original, case)
            self.assertNotEqual(mutated, original)
            self.assertTrue(case["expected"])

    @staticmethod
    def apply_mutation(original, case):
        data = bytearray(original)
        operation = case["operation"]
        if operation == "truncate":
            data = data[: case["size"]]
        elif operation == "xor-u8":
            data[case["offset"]] ^= case["value"]
        elif operation == "write-u8":
            data[case["offset"]] = case["value"]
        elif operation == "write-u16":
            struct.pack_into("<H", data, case["offset"], case["value"])
        elif operation == "write-u32":
            struct.pack_into("<I", data, case["offset"], case["value"])
        elif operation == "zero":
            data[case["offset"] : case["offset"] + case["length"]] = bytes(case["length"])
        elif operation == "copy":
            start = case["sourceOffset"]
            data[case["offset"] : case["offset"] + case["length"]] = original[start : start + case["length"]]
        else:
            raise AssertionError(f"unsupported fixture mutation: {operation}")

        secondary = case.get("secondaryWrite")
        if secondary:
            if secondary["operation"] != "write-u8":
                raise AssertionError("unsupported secondary fixture mutation")
            data[secondary["offset"]] = secondary["value"]
        if case.get("recomputeHeaderCrc"):
            crc_offset = {
                "canonical-meta.bin": 108,
                "definition-meta.bin": 140,
                "attachments.bin": 84,
                "language-v4.bin": 104,
            }[case["file"]]
            struct.pack_into("<I", data, crc_offset, crc32(data[:crc_offset]))
        return bytes(data)


if __name__ == "__main__":
    unittest.main()

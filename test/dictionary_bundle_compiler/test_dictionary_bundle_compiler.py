import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.compiler import CompileError, compile_bundle, compile_file  # noqa: E402
from convert_wiktextract import extract_forms  # noqa: E402

FIXTURE = ROOT / "test" / "data" / "dictionary-sources" / "german-small.json"


def load_fixture():
    return json.loads(FIXTURE.read_text(encoding="utf-8"))


def decoded_lexemes(bundle):
    records = bundle.files["device/lexemes.bin"]
    headwords = bundle.files["device/headwords.bin"]
    entries = bundle.files["device/entries.bin"]
    result = []
    for offset in range(0, len(records), 24):
        head_offset, entry_offset, entry_length, key_hash, head_length, pos, flags = struct.unpack_from(
            "<IIIQHBB", records, offset
        )
        result.append(
            {
                "headword": headwords[head_offset : head_offset + head_length].decode("utf-8"),
                "entry": entries[entry_offset : entry_offset + entry_length],
                "keyHash": key_hash,
                "partOfSpeech": pos,
                "flags": flags,
            }
        )
    return result


def decoded_forms(bundle):
    data = bundle.files["compiler/forms.bin"]
    (
        magic,
        version,
        header_size,
        _bundle_uuid,
        form_count,
        analysis_count,
        directory_offset,
        analysis_offset,
        strings_offset,
        strings_size,
        file_size,
        _reserved,
    ) = struct.unpack_from("<4sHH16sIIIIIIII", data, 0)
    assert magic == b"CXDF"
    assert version == 2
    assert header_size == 64
    assert file_size == len(data)
    assert strings_offset + strings_size == len(data)

    forms = {}
    counted_analyses = 0
    for index in range(form_count):
        record_offset = directory_offset + index * 20
        _, string_offset, first_analysis, string_length, count, flags = struct.unpack_from(
            "<QIIHBB", data, record_offset
        )
        assert 0 <= flags <= 255
        form = data[strings_offset + string_offset : strings_offset + string_offset + string_length].decode("utf-8")
        ids = []
        for analysis_index in range(first_analysis, first_analysis + count):
            lexeme_id, confidence, analysis_flags = struct.unpack_from(
                "<IHH", data, analysis_offset + analysis_index * 8
            )
            assert confidence == 1000
            assert analysis_flags == 0
            ids.append(lexeme_id)
        forms[form] = ids
        counted_analyses += count
    assert counted_analyses == analysis_count
    return forms


class DictionaryBundleCompilerTest(unittest.TestCase):
    def test_output_is_deterministic_across_source_order(self):
        source = load_fixture()
        first = compile_bundle(source)
        reordered = copy.deepcopy(source)
        reordered["lexemes"].reverse()
        second = compile_bundle(reordered)

        self.assertEqual(first.archive_bytes, second.archive_bytes)
        self.assertEqual(first.files, second.files)

    def test_runtime_metadata_matches_device_files(self):
        bundle = compile_bundle(load_fixture())
        meta = bundle.files["device/meta.bin"]
        (
            magic,
            version,
            meta_size,
            flags,
            _bundle_uuid,
            source_language,
            target_language,
            lexeme_count,
            record_size,
            reserved,
            lexemes_size,
            headwords_size,
            entries_size,
            lexemes_crc,
            headwords_crc,
            entries_crc,
            meta_crc,
        ) = struct.unpack("<4sHHI16s8s8sIHHIIIIIII", meta)

        self.assertEqual((magic, version, meta_size, flags), (b"CXDM", 1, 80, 0))
        self.assertEqual((source_language.rstrip(b"\0"), target_language.rstrip(b"\0")), (b"de", b"en"))
        self.assertEqual((lexeme_count, record_size, reserved), (4, 24, 0))
        for path, size, crc in (
            ("device/lexemes.bin", lexemes_size, lexemes_crc),
            ("device/headwords.bin", headwords_size, headwords_crc),
            ("device/entries.bin", entries_size, entries_crc),
        ):
            self.assertEqual(size, len(bundle.files[path]))
            self.assertEqual(crc, zlib.crc32(bundle.files[path]) & 0xFFFFFFFF)
        self.assertEqual(meta_crc, zlib.crc32(meta[:76]) & 0xFFFFFFFF)

    def test_merges_duplicate_lemmas_but_preserves_pos_ambiguity(self):
        bundle = compile_bundle(load_fixture())
        lexemes = decoded_lexemes(bundle)

        self.assertEqual([item["headword"] for item in lexemes], ["Haus", "Liebe", "gehen", "lieben"])
        haus_entry = lexemes[0]["entry"]
        version, flags, field_count = struct.unpack_from("<BBH", haus_entry)
        self.assertEqual((version, flags, field_count), (1, 0, 2))
        self.assertNotEqual(lexemes[1]["keyHash"], lexemes[3]["keyHash"])

        forms = decoded_forms(bundle)
        self.assertEqual(forms["liebe"], [1, 3])
        self.assertEqual(forms["Häusern"], [0])
        self.assertEqual(forms["gingen"], [2])

    def test_embeds_language_neutral_frequency_difficulty(self):
        scores = {"Haus": 6.0, "Häusern": 3.0, "Liebe": 5.0, "liebe": 4.0, "gehen": 5.5,
                  "gingen": 4.5, "lieben": 4.0}
        bundle = compile_bundle(load_fixture(), lambda surface, _language: scores.get(surface, 0.0))
        data = bundle.files["compiler/forms.bin"]
        form_count, directory_offset, strings_offset = struct.unpack_from("<I4xI4xI", data, 24)
        priorities = {}
        for index in range(form_count):
            offset = directory_offset + index * 20
            _, string_offset, _, string_length, _, difficulty = struct.unpack_from("<QIIHBB", data, offset)
            surface = data[strings_offset + string_offset:strings_offset + string_offset + string_length].decode("utf-8")
            priorities[surface] = difficulty
        self.assertGreater(priorities["Häusern"], priorities["Haus"])
        self.assertEqual(priorities["Haus"], 65)

    def test_normalizes_forms_to_nfc(self):
        source = load_fixture()
        source["lexemes"][0]["forms"].append("Ha\u0308usern")
        forms = decoded_forms(compile_bundle(source))

        self.assertIn("Häusern", forms)
        self.assertNotIn("Ha\u0308usern", forms)

    def test_emits_license_and_hashed_manifest(self):
        bundle = compile_bundle(load_fixture())
        manifest = json.loads(bundle.files["manifest.json"])

        self.assertEqual(manifest["license"]["spdx"], "CC0-1.0")
        self.assertIn(b"Synthetic German dictionary fixture", bundle.files["device/licenses.txt"])
        self.assertEqual(manifest["lexemeCount"], 4)
        self.assertEqual(manifest["frequencyRanking"], "none")
        self.assertEqual(set(manifest["files"]), set(bundle.files) - {"manifest.json"})
        self.assertTrue(all(len(file_info["sha256"]) == 64 for file_info in manifest["files"].values()))

    def test_writes_valid_deterministic_archive_atomically(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "german-small.cpdict"
            compile_file(FIXTURE, output)
            first = output.read_bytes()
            compile_file(FIXTURE, output)

            self.assertEqual(first, output.read_bytes())
            self.assertFalse(output.with_name(output.name + ".tmp").exists())
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.testzip(), None)
                self.assertEqual(archive.namelist(), sorted(archive.namelist()))
                self.assertIn("compiler/forms.bin", archive.namelist())
                self.assertIn("device/meta.bin", archive.namelist())

    def test_wiktextract_metadata_rows_are_not_compiled_as_surface_forms(self):
        entry = {
            "forms": [
                {"form": "fiel", "tags": ["past"]},
                {"form": "sein", "tags": ["auxiliary"]},
                {"form": "7 strong", "tags": ["class"]},
                {"form": "de-conj", "tags": ["inflection-template"]},
                {"form": "broken", "tags": ["error-unrecognized-form"]},
            ]
        }
        self.assertEqual(extract_forms(entry), {"fiel"})

    def test_rejects_missing_license_and_oversized_headword(self):
        source = load_fixture()
        del source["license"]
        with self.assertRaisesRegex(CompileError, "license must be an object"):
            compile_bundle(source)

        source = load_fixture()
        source["lexemes"][0]["headword"] = "ä" * 49
        with self.assertRaisesRegex(CompileError, "headword exceeds 96 UTF-8 bytes"):
            compile_bundle(source)


if __name__ == "__main__":
    unittest.main()

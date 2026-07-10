import copy
import json
from pathlib import Path
import struct
import sys
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.book_compiler import (  # noqa: E402
    CANDIDATE_AMBIGUOUS,
    CANDIDATE_COMPOUND,
    CANDIDATE_NORMALIZED_FALLBACK,
    compile_book,
    load_compiler_dictionary,
    tokenize_xhtml,
)
from dictionary.compiler import compile_bundle  # noqa: E402

FIXTURE = ROOT / "test" / "data" / "dictionary-sources" / "german-small.json"


def compiler_dictionary():
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
    return load_compiler_dictionary(bundle.files["device/meta.bin"], bundle.files["compiler/forms.bin"])


def artifact_header(data):
    values = struct.unpack_from("<4sHHIHH16s8s8sHHIIIIIIIIIIIII", data, 0)
    return {
        "magic": values[0],
        "version": values[1],
        "headerSize": values[2],
        "spineCount": values[9],
        "shardCount": values[11],
        "recordCount": values[12],
        "lemmaCount": values[13],
        "surfaceCount": values[14],
        "spineOffset": values[15],
        "shardOffset": values[16],
        "recordsOffset": values[17],
        "lemmasOffset": values[18],
        "globalOffset": values[19],
        "surfaceOffset": values[20],
        "metadataOffset": values[21],
        "fileSize": values[22],
        "payloadCrc": values[23],
    }


def decoded_surfaces(data):
    header = artifact_header(data)
    global_ids = [struct.unpack_from("<I", data, header["lemmasOffset"] + index * 4)[0]
                  for index in range(header["lemmaCount"])]
    surface_offset = header["surfaceOffset"]
    (
        magic,
        version,
        section_header_size,
        surface_count,
        _analysis_count,
        _component_count,
        record_offset,
        analysis_offset,
        component_offset,
        string_offset,
        section_size,
    ) = struct.unpack_from("<4sHHIIIIIIII", data, surface_offset)
    assert (magic, version, section_header_size) == (b"CXSD", 1, 40)
    assert surface_count == header["surfaceCount"]
    assert surface_offset + section_size == header["metadataOffset"]

    surfaces = {}
    for index in range(surface_count):
        record = struct.unpack_from("<IIIHBBHH", data, surface_offset + record_offset + index * 20)
        (
            string_relative,
            first_analysis,
            first_component,
            length,
            analysis_count,
            component_count,
            confidence,
            flags,
        ) = record
        surface = data[surface_offset + string_offset + string_relative:
                       surface_offset + string_offset + string_relative + length].decode("utf-8")
        analyses = [global_ids[struct.unpack_from("<H", data, surface_offset + analysis_offset +
                                                  (first_analysis + item) * 2)[0]]
                    for item in range(analysis_count)]
        components = [global_ids[struct.unpack_from("<H", data, surface_offset + component_offset +
                                                    (first_component + item) * 2)[0]]
                      for item in range(component_count)]
        surfaces[surface] = {
            "analyses": analyses,
            "components": components,
            "confidence": confidence,
            "flags": flags,
        }
    return surfaces


class GermanBookCompilerTest(unittest.TestCase):
    def test_tokenizer_joins_inline_markup_and_normalizes_nfc(self):
        tokens = tokenize_xhtml("<p>Die Ha\u0308u<em>sern</em> &amp; gehen.</p><script>lieben</script>")
        self.assertEqual([token.surface for token in tokens], ["Die", "Häusern", "gehen"])

    def test_compiles_inflections_ambiguity_stopwords_and_compounds(self):
        dictionary = compiler_dictionary()
        xhtml = "<html><body><p>Die Ha\u0308u<em>sern</em>, Gingen! liebe Krankenhausaufnahme.</p></body></html>"
        compiled = compile_book([xhtml], dictionary)

        self.assertIn('<span data-crossink-lang-shard="0"></span>Die', compiled.xhtml_spines[0])
        self.assertNotIn("data-crossink-lang-shard=\"1\"", compiled.xhtml_spines[0])
        surfaces = decoded_surfaces(compiled.language_artifact)
        self.assertEqual(set(surfaces), {"Häusern", "Gingen", "liebe", "Krankenhausaufnahme"})
        self.assertEqual(len(surfaces["liebe"]["analyses"]), 2)
        self.assertTrue(surfaces["liebe"]["flags"] & CANDIDATE_AMBIGUOUS)
        self.assertTrue(surfaces["Gingen"]["flags"] & CANDIDATE_NORMALIZED_FALLBACK)
        self.assertEqual(surfaces["Gingen"]["confidence"], 900)
        self.assertEqual(len(surfaces["Krankenhausaufnahme"]["components"]), 2)
        self.assertTrue(surfaces["Krankenhausaufnahme"]["flags"] & CANDIDATE_COMPOUND)

    def test_shards_every_64_source_tokens_and_deduplicates_per_shard(self):
        dictionary = compiler_dictionary()
        compiled = compile_book(["<p>" + " ".join(["gingen"] * 65) + "</p>"], dictionary)
        header = artifact_header(compiled.language_artifact)

        self.assertEqual(header["shardCount"], 2)
        self.assertEqual(header["recordCount"], 2)
        self.assertEqual(header["surfaceCount"], 1)
        self.assertEqual(compiled.xhtml_spines[0].count("data-crossink-lang-shard"), 2)
        first = struct.unpack_from("<IHHII", compiled.language_artifact, header["shardOffset"])
        second = struct.unpack_from("<IHHII", compiled.language_artifact, header["shardOffset"] + 16)
        self.assertEqual((first[0], first[1], first[3], first[4]), (0, 1, 0, 64))
        self.assertEqual((second[0], second[1], second[3], second[4]), (1, 1, 64, 65))

    def test_artifact_header_offsets_and_crcs_are_self_consistent(self):
        compiled = compile_book(["<p>Häusern gingen.</p>", "<p>liebe</p>"], compiler_dictionary())
        data = compiled.language_artifact
        header = artifact_header(data)

        self.assertEqual((header["magic"], header["version"], header["headerSize"]), (b"CXLG", 1, 108))
        self.assertEqual(header["spineCount"], 2)
        self.assertEqual(header["fileSize"], len(data))
        self.assertEqual(header["payloadCrc"], zlib.crc32(data[108:]) & 0xFFFFFFFF)
        self.assertEqual(struct.unpack_from("<I", data, 104)[0], zlib.crc32(data[:104]) & 0xFFFFFFFF)
        offsets = [header[name] for name in ("spineOffset", "shardOffset", "recordsOffset", "lemmasOffset",
                                             "globalOffset", "surfaceOffset", "metadataOffset")]
        self.assertEqual(offsets, sorted(offsets))
        self.assertTrue(all(offset % 4 == 0 for offset in offsets))

    def test_output_is_deterministic(self):
        dictionary = compiler_dictionary()
        xhtml = ["<p>Häusern gingen liebe.</p>"]
        first = compile_book(xhtml, dictionary)
        second = compile_book(xhtml, dictionary)
        self.assertEqual(first, second)


if __name__ == "__main__":
    unittest.main()

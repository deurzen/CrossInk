import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.analysis_policy import CanonicalAnalysis, CanonicalPos  # noqa: E402
from dictionary.contextual.canonical_lexicon import (  # noqa: E402
    CanonicalLexemeInput,
    compile_canonical_bundle,
    load_canonical_lexicon_index,
)
from dictionary.contextual.compiler_support import tokenize_xhtml  # noqa: E402
from dictionary.contextual.epub_compiler import compile_contextual_book  # noqa: E402
from dictionary.contextual.pipeline import (  # noqa: E402
    AnalysisProvenance,
    AnalyzedToken,
    RankedAnalysis,
)

FIXTURE = ROOT / "test" / "data" / "contextual" / "compiler-differential.json"
HEADER_FORMAT = "<4sHHIHH16s8s8sHHIIIIIIIIIIIII"


def canonical_analysis(value):
    return CanonicalAnalysis(value[0], CanonicalPos[value[1]])


def provenance(names):
    value = AnalysisProvenance(0)
    for name in names:
        value |= AnalysisProvenance[name]
    return value


def ranked_analyses(values):
    return tuple(
        RankedAnalysis(canonical_analysis(value[:2]), value[2], provenance(value[3]))
        for value in values
    )


def fnv1a64(data):
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


class FixtureAnalyzer:
    language = "de"

    def __init__(self, calls):
        self.calls = calls
        self.index = 0

    def analyze_sentence(self, sentence, source_offset=0):
        if self.index >= len(self.calls):
            raise AssertionError(f"unexpected analyzer call: {sentence!r} at {source_offset}")
        call = self.calls[self.index]
        self.index += 1
        repeated = call.get("repeat")
        if repeated is not None:
            surface = repeated["surface"]
            expected = repeated["separator"].join([surface] * repeated["count"])
            if sentence != expected or source_offset != repeated["sourceOffset"]:
                raise AssertionError(
                    f"analyzer call differs: {(sentence, source_offset)!r} != "
                    f"{(expected, repeated['sourceOffset'])!r}"
                )
            step = len(surface) + len(repeated["separator"])
            return tuple(
                AnalyzedToken(
                    surface,
                    source_offset + index * step,
                    source_offset + index * step + len(surface),
                    canonical_analysis(repeated["context"]),
                    ranked_analyses(repeated["analyses"]),
                )
                for index in range(repeated["count"])
            )

        if sentence != call["sentence"] or source_offset != call["sourceOffset"]:
            raise AssertionError(
                f"analyzer call differs: {(sentence, source_offset)!r} != "
                f"{(call['sentence'], call['sourceOffset'])!r}"
            )
        return tuple(
            AnalyzedToken(
                token["surface"],
                token["start"],
                token["start"] + len(token["surface"]),
                canonical_analysis(token["context"]),
                ranked_analyses(token["analyses"]),
            )
            for token in call["tokens"]
        )

    def assert_complete(self):
        if self.index != len(self.calls):
            raise AssertionError(f"only consumed {self.index}/{len(self.calls)} analyzer calls")


def fixture_spines(fixture):
    output = []
    for spine in fixture["spines"]:
        repeated = spine.get("repeat")
        if repeated is None:
            output.append(spine["xhtml"])
        else:
            text = repeated["separator"].join([repeated["surface"]] * repeated["count"])
            output.append(repeated["wrapperPrefix"] + text + repeated["wrapperSuffix"])
    return output


def identity(value):
    return value[0], value[1]


class ContextualCompilerDifferentialTest(unittest.TestCase):
    def test_offsets_markers_ids_and_binary_records_match_checked_in_fixture(self):
        fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        self.assertEqual(fixture["schemaVersion"], 1)
        xhtml_spines = fixture_spines(fixture)

        for spine_index, (spine, expected) in enumerate(zip(xhtml_spines, fixture["spines"])):
            tokens = tokenize_xhtml(spine)
            if "tokens" in expected:
                actual = [[item.surface, item.raw_offset, item.text_offset] for item in tokens]
                self.assertEqual(actual, expected["tokens"], f"spine {spine_index} token offsets")
            else:
                boundary = [tokens[0], tokens[-2], tokens[-1]]
                actual = [[item.surface, item.raw_offset, item.text_offset] for item in boundary]
                self.assertEqual(actual, expected["boundaryTokens"], f"spine {spine_index} boundary offsets")

        lexemes = [
            CanonicalLexemeInput(item[0], CanonicalPos[item[1]])
            for item in fixture["canonicalLexemes"]
        ]
        bundle = compile_canonical_bundle(
            lexemes,
            {"fixture": "compiler-differential-v1"},
            "SPDX-License-Identifier: CC0-1.0\n",
        )
        with tempfile.TemporaryDirectory() as temporary:
            canonical_path = Path(temporary) / "canonical.cplex"
            canonical_path.write_bytes(bundle.archive_bytes)
            canonical = load_canonical_lexicon_index(canonical_path)

            analyzer = FixtureAnalyzer(fixture["analysisCalls"])
            compiled = compile_contextual_book(xhtml_spines, analyzer, canonical)
            analyzer.assert_complete()

            next_shard = 0
            for spine_index, (source, transformed, expected) in enumerate(
                zip(xhtml_spines, compiled.xhtml_spines, fixture["spines"])
            ):
                insertions = []
                source_tokens = tokenize_xhtml(source)
                for token_index in expected["markerTokenIndexes"]:
                    insertions.append(
                        (
                            source_tokens[token_index].raw_offset,
                            f'<span data-crossink-lang-shard="{next_shard}"></span>',
                        )
                    )
                    next_shard += 1
                expected_xhtml = source
                for offset, marker in reversed(insertions):
                    expected_xhtml = expected_xhtml[:offset] + marker + expected_xhtml[offset:]
                self.assertEqual(transformed, expected_xhtml, f"spine {spine_index} shard markers")

            data = compiled.language_artifact
            values = struct.unpack_from(HEADER_FORMAT, data, 0)
            expected_header = fixture["expected"]["header"]
            self.assertEqual(values[0:6], (b"CXLG", 5, 108, 0, 1, 1))
            self.assertEqual(values[6], canonical.canonical_uuid.bytes)
            self.assertEqual(
                {"spines": values[9], "shards": values[11], "records": values[12], "lemmas": values[13]},
                expected_header,
            )
            self.assertEqual(values[22], len(data))
            self.assertEqual(values[23], zlib.crc32(data[108:]) & 0xFFFFFFFF)
            self.assertEqual(struct.unpack_from("<I", data, 104)[0], zlib.crc32(data[:104]) & 0xFFFFFFFF)

            spine_directory = [
                list(struct.unpack_from("<II", data, values[15] + index * 8))
                for index in range(values[9])
            ]
            self.assertEqual(spine_directory, fixture["expected"]["spineDirectory"])

            global_to_identity = {
                canonical.resolve(lemma, CanonicalPos[pos]): (lemma, pos)
                for lemma, pos in fixture["canonicalLexemes"]
            }
            local_globals = [
                struct.unpack_from("<I", data, values[18] + index * 4)[0]
                for index in range(values[13])
            ]
            self.assertEqual(
                [global_to_identity[item] for item in local_globals],
                [identity(item) for item in fixture["expected"]["localLemmas"]],
            )

            decoded_shards = []
            decoded_record_count = 0
            for shard_index in range(values[11]):
                blob_offset, blob_length, record_count, token_start, token_end, reserved = struct.unpack_from(
                    "<IHHIII", data, values[16] + shard_index * 20
                )
                self.assertEqual(reserved, 0)
                cursor = values[17] + blob_offset
                end = cursor + blob_length
                records = []
                previous_key = None
                for _ in range(record_count):
                    record_start = cursor
                    surface_hash, record_size, surface_length, analysis_count, flags, difficulty, confidence, grammar = (
                        struct.unpack_from("<QHBBBBHI", data, cursor)
                    )
                    self.assertEqual(record_size % 4, 0)
                    self.assertGreaterEqual(record_size, 20 + analysis_count * 2 + surface_length)
                    self.assertEqual(grammar, 0)
                    local_ids = struct.unpack_from(f"<{analysis_count}H", data, cursor + 20)
                    text_start = cursor + 20 + analysis_count * 2
                    surface_bytes = data[text_start : text_start + surface_length]
                    surface = surface_bytes.decode("utf-8")
                    self.assertEqual(surface_hash, fnv1a64(surface_bytes))
                    key = (surface_hash, surface_bytes)
                    if previous_key is not None:
                        self.assertLess(previous_key, key)
                    previous_key = key
                    padding_start = text_start + surface_length
                    self.assertEqual(data[padding_start : record_start + record_size], bytes(record_start + record_size - padding_start))
                    records.append(
                        {
                            "surface": surface,
                            "lemmas": [global_to_identity[local_globals[item]] for item in local_ids],
                            "flags": flags,
                            "difficulty": difficulty,
                            "confidence": confidence,
                        }
                    )
                    cursor += record_size
                    decoded_record_count += 1
                self.assertEqual(cursor, end)
                decoded_shards.append({"tokenRange": [token_start, token_end], "records": records})

            expected_shards = fixture["expected"]["shards"]
            for shard in expected_shards:
                shard["records"] = [
                    {**record, "lemmas": [identity(item) for item in record["lemmas"]]}
                    for record in shard["records"]
                ]
            self.assertEqual(decoded_shards, expected_shards)
            self.assertEqual(decoded_record_count, values[12])
            self.assertEqual(compiled.missing_canonical_analyses, 0)
            self.assertEqual(hashlib.sha256(data).hexdigest(), fixture["expected"]["artifactSha256"])


if __name__ == "__main__":
    unittest.main()

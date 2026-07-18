import json
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.analysis_policy import (  # noqa: E402
    CanonicalAnalysis,
    CanonicalFeatures,
    CanonicalPos,
)
from dictionary.contextual.canonical_lexicon import (  # noqa: E402
    CanonicalLexemeInput,
    compile_canonical_bundle,
    load_canonical_lexicon_index,
)
from dictionary.contextual.epub_compiler import (  # noqa: E402
    ContextualEpubError,
    FLAG_AMBIGUOUS,
    FLAG_CONTEXTUAL,
    GrammarDiagnostics,
    LANGUAGE_PATH,
    _EncodedCandidate,
    _encode_candidate_record,
    _finalize_surface,
    _merge_surface,
    compile_contextual_book,
    compile_contextual_epub,
)
from dictionary.contextual.grammar_descriptor import encode_features  # noqa: E402
from dictionary.contextual.pipeline import (  # noqa: E402
    AnalysisProvenance,
    AnalyzedToken,
    RankedAnalysis,
)


WORD = re.compile(r"[^\W\d_]+", re.UNICODE)
PRIMARY = AnalysisProvenance.PRIMARY_MORPHOLOGY
EXACT = AnalysisProvenance.EXACT_FORM_INVENTORY
RECOMBINED = AnalysisProvenance.CONTEXT_RECOMBINATION


class SyntheticAnalyzer:
    language = "de"

    def __init__(self, error=None):
        self.error = error

    def analyze_sentence(self, sentence, source_offset=0):
        if self.error:
            raise self.error
        output = []
        for match in WORD.finditer(sentence):
            surface = match.group(0)
            values = {
                "Laden": (
                    CanonicalAnalysis("laden", CanonicalPos.VERB),
                    (("laden", CanonicalPos.VERB, 1000, PRIMARY),),
                ),
                "laden": (
                    CanonicalAnalysis("laden", CanonicalPos.VERB),
                    (("laden", CanonicalPos.VERB, 1000, PRIMARY),),
                ),
                "steht": (
                    CanonicalAnalysis(
                        "stehen",
                        CanonicalPos.VERB,
                        CanonicalFeatures(
                            mood="indicative",
                            number="singular",
                            person="third",
                            tense="present",
                            verb_form="finite",
                        ),
                    ),
                    (
                        ("aufstehen", CanonicalPos.VERB, 1150, EXACT | RECOMBINED),
                        ("stehen", CanonicalPos.VERB, 1000, PRIMARY),
                    ),
                ),
                "Goethe": (
                    CanonicalAnalysis("Goethe", CanonicalPos.PROPER_NOUN),
                    (("Goethe", CanonicalPos.PROPER_NOUN, 1000, EXACT),),
                ),
            }.get(surface)
            if values is None:
                context = CanonicalAnalysis(surface.casefold(), CanonicalPos.OTHER)
                ranked = ()
            else:
                context, raw_ranked = values
                ranked = tuple(
                    RankedAnalysis(CanonicalAnalysis(lemma, pos), score, provenance)
                    for lemma, pos, score, provenance in raw_ranked
                )
            output.append(
                AnalyzedToken(
                    surface,
                    source_offset + match.start(),
                    source_offset + match.end(),
                    context,
                    ranked,
                )
            )
        return tuple(output)


def artifact_header(data):
    values = struct.unpack_from("<4sHHIHH16s8s8sHHIIIIIIIIIIIII", data, 0)
    return {
        "version": values[1],
        "uuid": values[6],
        "target": values[8].rstrip(b"\0"),
        "spines": values[9],
        "shards": values[11],
        "records": values[12],
        "lemmas": values[13],
        "shardOffset": values[16],
        "recordsOffset": values[17],
        "lemmasOffset": values[18],
        "metadataOffset": values[19],
        "fileSize": values[22],
        "payloadCrc": values[23],
    }


def decoded_surfaces(data):
    header = artifact_header(data)
    global_ids = [
        struct.unpack_from("<I", data, header["lemmasOffset"] + index * 4)[0]
        for index in range(header["lemmas"])
    ]
    surfaces = {}
    for shard_index in range(header["shards"]):
        blob_offset, blob_length, count, _, _, _ = struct.unpack_from(
            "<IHHIII", data, header["shardOffset"] + shard_index * 20
        )
        cursor = header["recordsOffset"] + blob_offset
        end = cursor + blob_length
        for _ in range(count):
            _, record_size, length, analysis_count, flags, difficulty, confidence, grammar = struct.unpack_from(
                "<QHBBBBHI", data, cursor
            )
            local_ids = struct.unpack_from(f"<{analysis_count}H", data, cursor + 20)
            text_offset = cursor + 20 + analysis_count * 2
            surface = data[text_offset : text_offset + length].decode("utf-8")
            surfaces[surface] = {
                "ids": tuple(global_ids[value] for value in local_ids),
                "recordSize": record_size,
                "flags": flags,
                "difficulty": difficulty,
                "confidence": confidence,
                "grammar": grammar,
            }
            cursor += record_size
        assert cursor == end
    return surfaces


class ContextualEpubCompilerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        lexemes = (
            CanonicalLexemeInput("Goethe", CanonicalPos.PROPER_NOUN),
            CanonicalLexemeInput("Laden", CanonicalPos.NOUN),
            CanonicalLexemeInput("aufstehen", CanonicalPos.VERB),
            CanonicalLexemeInput("laden", CanonicalPos.VERB),
            CanonicalLexemeInput("stehen", CanonicalPos.VERB),
        )
        bundle = compile_canonical_bundle(
            lexemes,
            {"fixture": "contextual-epub"},
            "SPDX-License-Identifier: CC0-1.0\n",
        )
        self.canonical_path = Path(self.temporary.name) / "canonical.cplex"
        self.canonical_path.write_bytes(bundle.archive_bytes)
        self.canonical = load_canonical_lexicon_index(self.canonical_path)

    def tearDown(self):
        self.temporary.cleanup()

    def test_compiles_v5_contextual_candidates_markers_grammar_and_metadata(self):
        xhtml = "<html><body><p>Wir <em>laden</em>. Er steht auf. Goethe.</p></body></html>"
        compiled = compile_contextual_book([xhtml], SyntheticAnalyzer(), self.canonical)
        self.assertIn('data-crossink-lang-shard="0"', compiled.xhtml_spines[0])
        header = artifact_header(compiled.language_artifact)
        self.assertEqual(header["version"], 5)
        self.assertEqual(header["uuid"], self.canonical.canonical_uuid.bytes)
        self.assertEqual(header["target"], b"und")
        self.assertEqual(header["fileSize"], len(compiled.language_artifact))
        self.assertEqual(
            header["payloadCrc"],
            zlib.crc32(compiled.language_artifact[108:]) & 0xFFFFFFFF,
        )
        surfaces = decoded_surfaces(compiled.language_artifact)
        self.assertEqual(set(surfaces), {"laden", "steht", "Goethe"})
        self.assertEqual(
            surfaces["steht"]["ids"],
            (
                self.canonical.resolve("aufstehen", CanonicalPos.VERB),
                self.canonical.resolve("stehen", CanonicalPos.VERB),
            ),
        )
        self.assertTrue(surfaces["steht"]["flags"] & FLAG_CONTEXTUAL)
        self.assertTrue(surfaces["steht"]["flags"] & FLAG_AMBIGUOUS)
        self.assertEqual(
            surfaces["steht"]["grammar"],
            encode_features(
                CanonicalFeatures(
                    mood="indicative",
                    number="singular",
                    person="third",
                    tense="present",
                    verb_form="finite",
                )
            ),
        )
        self.assertEqual(compiled.missing_canonical_analyses, 0)
        self.assertEqual(
            compiled.grammar_diagnostics,
            GrammarDiagnostics(1, 2, 0, 0),
        )
        for surface, record in surfaces.items():
            expected_size = 20 + len(record["ids"]) * 2 + len(surface.encode("utf-8"))
            self.assertEqual(record["recordSize"], (expected_size + 3) & ~3)

        offset = header["metadataOffset"]
        self.assertEqual(compiled.language_artifact[offset : offset + 4], b"CXLM")
        length = struct.unpack_from("<I", compiled.language_artifact, offset + 8)[0]
        metadata = json.loads(compiled.language_artifact[offset + 16 : offset + 16 + length])
        self.assertEqual(metadata["canonicalUuid"], str(self.canonical.canonical_uuid))
        self.assertEqual(metadata["analysisPolicyVersion"], 2)
        self.assertEqual(metadata["canonicalPosVersion"], 1)
        self.assertEqual(metadata["compilerVersion"], 2)
        self.assertEqual(metadata["grammarDescriptorVersion"], 1)
        self.assertEqual(
            metadata["grammarDiagnostics"],
            {
                "availableOccurrences": 1,
                "noContextualFeatureOccurrences": 2,
                "posMismatchOccurrences": 0,
                "withinShardConflicts": 0,
            },
        )

    def test_surface_aggregation_keeps_grammar_with_occurrence_primary_only(self):
        singular = encode_features(CanonicalFeatures(number="singular"))
        plural = encode_features(CanonicalFeatures(number="plural"))
        first = _EncodedCandidate("Sie", (10, 20), 1000, 7, FLAG_CONTEXTUAL, singular)
        alternative = _EncodedCandidate("Sie", (20, 10), 900, 8, FLAG_CONTEXTUAL, plural)

        evidence = _merge_surface(None, first)
        evidence = _merge_surface(evidence, alternative)
        finalized = _finalize_surface("Sie", evidence)
        self.assertEqual(finalized.global_ids, (10, 20))
        self.assertEqual(finalized.grammar_descriptor, singular)
        self.assertFalse(finalized.grammar_conflict)

        evidence = _merge_surface(evidence, first)
        finalized = _finalize_surface("Sie", evidence)
        self.assertEqual(finalized.grammar_descriptor, singular)
        self.assertFalse(finalized.grammar_conflict)

    def test_surface_aggregation_clears_conflicting_primary_grammar(self):
        singular = encode_features(CanonicalFeatures(number="singular"))
        plural = encode_features(CanonicalFeatures(number="plural"))
        candidates = (
            _EncodedCandidate("Sie", (10,), 1000, 7, FLAG_CONTEXTUAL, singular),
            _EncodedCandidate("Sie", (10,), 1000, 8, FLAG_CONTEXTUAL, plural),
            _EncodedCandidate("Sie", (10,), 1000, 9, FLAG_CONTEXTUAL, singular),
        )
        for ordered in (candidates, tuple(reversed(candidates))):
            evidence = None
            for candidate in ordered:
                evidence = _merge_surface(evidence, candidate)
            finalized = _finalize_surface("Sie", evidence)
            self.assertEqual(finalized.grammar_descriptor, 0)
            self.assertTrue(finalized.grammar_conflict)

    def test_candidate_encoder_rejects_malformed_grammar_before_packing(self):
        base = _EncodedCandidate("Sie", (10,), 1000, 7, FLAG_CONTEXTUAL, 0)
        self.assertTrue(_encode_candidate_record(base, {10: 0}))
        for descriptor, message in (
            (1 << 17, "reserved bits"),
            (1 << 32, "fit uint32"),
            (0x00010001, "infinitive descriptor"),
        ):
            candidate = _EncodedCandidate(
                base.surface,
                base.global_ids,
                base.confidence,
                base.difficulty,
                base.flags,
                descriptor,
            )
            with self.subTest(descriptor=descriptor):
                with self.assertRaisesRegex(ContextualEpubError, message):
                    _encode_candidate_record(candidate, {10: 0})

    def test_shards_by_rendered_word_tokens_and_is_deterministic(self):
        xhtml = "<p>" + " ".join(["laden"] * 65) + "</p>"
        first = compile_contextual_book([xhtml], SyntheticAnalyzer(), self.canonical)
        second = compile_contextual_book([xhtml], SyntheticAnalyzer(), self.canonical)
        self.assertEqual(first, second)
        self.assertEqual(first.xhtml_spines[0].count("data-crossink-lang-shard"), 2)
        header = artifact_header(first.language_artifact)
        self.assertEqual((header["shards"], header["records"]), (2, 2))

    def test_normalizes_decomposed_visible_text_for_analysis_without_moving_markers(self):
        class UmlautAnalyzer(SyntheticAnalyzer):
            def analyze_sentence(self, sentence, source_offset=0):
                self.asserted = sentence
                return super().analyze_sentence(sentence, source_offset)

        analyzer = UmlautAnalyzer()
        compiled = compile_contextual_book(
            ["<p>Ha\u0308user laden.</p>"],
            analyzer,
            self.canonical,
        )
        self.assertEqual(analyzer.asserted, "Häuser laden.")
        self.assertIn('<span data-crossink-lang-shard="0"></span>Ha\u0308user', compiled.xhtml_spines[0])
        self.assertIn("laden", decoded_surfaces(compiled.language_artifact))

    def test_reports_unmapped_analyses_without_inventing_ids(self):
        class MissingAnalyzer(SyntheticAnalyzer):
            def analyze_sentence(self, sentence, source_offset=0):
                token = next(WORD.finditer(sentence))
                return (
                    AnalyzedToken(
                        token.group(0),
                        source_offset + token.start(),
                        source_offset + token.end(),
                        CanonicalAnalysis("fehlen", CanonicalPos.VERB),
                        (
                            RankedAnalysis(
                                CanonicalAnalysis("fehlen", CanonicalPos.VERB),
                                1000,
                                PRIMARY,
                            ),
                        ),
                    ),
                )

        compiled = compile_contextual_book(["<p>fehlt</p>"], MissingAnalyzer(), self.canonical)
        self.assertEqual(compiled.missing_canonical_analyses, 1)
        self.assertEqual(decoded_surfaces(compiled.language_artifact), {})

    def test_full_epub_preserves_opf_and_is_transactional_and_byte_identical(self):
        input_path = Path(self.temporary.name) / "input.epub"
        first_path = Path(self.temporary.name) / "first.epub"
        second_path = Path(self.temporary.name) / "second.epub"
        opf = (
            b'<?xml version="1.0" encoding="UTF-8"?>\n'
            b'<package xmlns="http://www.idpf.org/2007/opf" version="3.0">'
            b'<manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>'
            b'</manifest><spine><itemref idref="chapter"/></spine></package>'
        )
        container = (
            b'<?xml version="1.0"?>'
            b'<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">'
            b'<rootfiles><rootfile full-path="OPS/package.opf" media-type="application/oebps-package+xml"/>'
            b'</rootfiles></container>'
        )
        with zipfile.ZipFile(input_path, "w") as archive:
            mimetype = zipfile.ZipInfo("mimetype", date_time=(2001, 2, 3, 4, 5, 6))
            mimetype.compress_type = zipfile.ZIP_STORED
            archive.writestr(mimetype, b"application/epub+zip")
            archive.writestr("META-INF/container.xml", container)
            archive.writestr("OPS/package.opf", opf)
            archive.writestr("OPS/chapter.xhtml", b"<html><body><p>Wir laden.</p></body></html>")
            archive.writestr("OPS/asset.bin", b"unchanged")
            archive.writestr(LANGUAGE_PATH, b"stale")

        compile_contextual_epub(input_path, first_path, SyntheticAnalyzer(), self.canonical)
        compile_contextual_epub(input_path, second_path, SyntheticAnalyzer(), self.canonical)
        self.assertEqual(first_path.read_bytes(), second_path.read_bytes())
        with zipfile.ZipFile(first_path) as archive:
            self.assertEqual(archive.read("OPS/package.opf"), opf)
            self.assertEqual(archive.read("OPS/asset.bin"), b"unchanged")
            self.assertEqual(archive.namelist().count(LANGUAGE_PATH), 1)
            self.assertEqual(artifact_header(archive.read(LANGUAGE_PATH))["version"], 5)
            self.assertIn(b"data-crossink-lang-shard", archive.read("OPS/chapter.xhtml"))

        protected = Path(self.temporary.name) / "protected.epub"
        protected.write_bytes(b"previous-good-output")
        with self.assertRaisesRegex(ContextualEpubError, "analysis failed"):
            compile_contextual_epub(
                input_path,
                protected,
                SyntheticAnalyzer(error=RuntimeError("analysis failed")),
                self.canonical,
            )
        self.assertEqual(protected.read_bytes(), b"previous-good-output")
        self.assertEqual(list(Path(self.temporary.name).glob("protected.epub.tmp-*")), [])

    def test_rejects_previously_marked_xhtml(self):
        with self.assertRaisesRegex(ContextualEpubError, "already contains"):
            compile_contextual_book(
                ['<p><span data-crossink-lang-shard="0"></span>laden</p>'],
                SyntheticAnalyzer(),
                self.canonical,
            )


if __name__ == "__main__":
    unittest.main()

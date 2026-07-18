from itertools import permutations
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.analysis_policy import (  # noqa: E402
    CanonicalAnalysis,
    CanonicalFeatures,
    CanonicalPos,
)
from dictionary.contextual.grammar_descriptor import (  # noqa: E402
    GRAMMAR_DESCRIPTOR_VERSION,
    GrammarDescriptorError,
    GrammarEvidence,
    decode_features,
    encode_contextual_grammar,
    encode_features,
    merge_grammar_evidence,
    validate_descriptor,
)

VECTORS = ROOT / "test" / "data" / "contextual" / "grammar-descriptor-v1.json"


class GrammarDescriptorTest(unittest.TestCase):
    def test_checked_in_vectors_encode_decode_and_reject_deterministically(self):
        fixture = json.loads(VECTORS.read_text(encoding="utf-8"))
        self.assertEqual((fixture["schemaVersion"], fixture["layoutVersion"]), (1, GRAMMAR_DESCRIPTOR_VERSION))
        for case in fixture["valid"]:
            with self.subTest(case=case["name"]):
                expected = int(case["word"], 16)
                features = CanonicalFeatures(**case["features"])
                self.assertEqual(encode_features(features), expected)
                self.assertEqual(decode_features(expected), features)
                validate_descriptor(expected)
        for case in fixture["invalid"]:
            with self.subTest(case=case["name"]):
                with self.assertRaisesRegex(GrammarDescriptorError, case["error"]):
                    validate_descriptor(int(case["word"], 16))

    def test_exhausts_layout_payload_and_reserved_bits(self):
        valid_count = 0
        for descriptor in range(1 << 17):
            try:
                decoded = decode_features(descriptor)
            except GrammarDescriptorError:
                continue
            valid_count += 1
            self.assertEqual(encode_features(decoded), descriptor)
        self.assertEqual(valid_count, 2113)

        for bit in range(17, 32):
            with self.subTest(reserved_bit=bit):
                with self.assertRaisesRegex(GrammarDescriptorError, "reserved bits"):
                    validate_descriptor(1 << bit)

    def test_rejects_non_uint32_and_unknown_canonical_values(self):
        for value in (None, True, -1, 1 << 32, "0"):
            with self.subTest(value=value):
                with self.assertRaises(GrammarDescriptorError):
                    validate_descriptor(value)

        fields = tuple(CanonicalFeatures().__dict__)
        for field in fields:
            with self.subTest(field=field):
                with self.assertRaisesRegex(GrammarDescriptorError, "unsupported grammar"):
                    encode_features(CanonicalFeatures(**{field: "unversioned"}))
        with self.assertRaisesRegex(GrammarDescriptorError, "invalid type"):
            encode_features(object())

    def test_same_surface_merge_is_order_independent_and_sticky(self):
        partial = (
            encode_features(CanonicalFeatures(mood="indicative", verb_form="finite")),
            encode_features(CanonicalFeatures(number="singular", person="third")),
            encode_features(CanonicalFeatures(tense="past", verb_form="finite")),
            0,
        )
        expected = encode_features(
            CanonicalFeatures(
                mood="indicative",
                number="singular",
                person="third",
                tense="past",
                verb_form="finite",
            )
        )
        for ordered in permutations(partial):
            evidence = GrammarEvidence()
            for descriptor in ordered:
                evidence = merge_grammar_evidence(evidence, descriptor)
            self.assertEqual(evidence, GrammarEvidence(expected))

        singular = encode_features(CanonicalFeatures(number="singular"))
        plural = encode_features(CanonicalFeatures(number="plural"))
        for ordered in permutations((singular, plural, singular)):
            evidence = GrammarEvidence()
            for descriptor in ordered:
                evidence = merge_grammar_evidence(evidence, descriptor)
            self.assertEqual(evidence, GrammarEvidence(0, True))
            self.assertEqual(
                merge_grammar_evidence(evidence, singular),
                GrammarEvidence(0, True),
            )

    def test_merge_clears_structurally_incompatible_unions(self):
        finite = encode_features(CanonicalFeatures(verb_form="finite"))
        nominal = encode_features(CanonicalFeatures(case="dative"))
        self.assertEqual(
            merge_grammar_evidence(GrammarEvidence(finite), nominal),
            GrammarEvidence(0, True),
        )
        with self.assertRaisesRegex(GrammarDescriptorError, "invalid type"):
            merge_grammar_evidence(object(), 0)
        with self.assertRaisesRegex(GrammarDescriptorError, "reserved bits"):
            merge_grammar_evidence(GrammarEvidence(0, True), 1 << 17)
        with self.assertRaisesRegex(GrammarDescriptorError, "must be unavailable"):
            GrammarEvidence(finite, True)

    def test_contextual_pos_policy_uses_context_features_only(self):
        finite_past = CanonicalFeatures(
            mood="indicative",
            number="singular",
            person="third",
            tense="past",
            verb_form="finite",
        )
        context = CanonicalAnalysis("knipsen", CanonicalPos.VERB, finite_past)
        primary = CanonicalAnalysis(
            "anknipsen",
            CanonicalPos.VERB,
            CanonicalFeatures(verb_form="infinitive"),
        )
        self.assertEqual(encode_contextual_grammar(context, primary), 0x0000DA80)

        noun_context = CanonicalAnalysis(
            "Berlin",
            CanonicalPos.PROPER_NOUN,
            CanonicalFeatures(case="dative", number="singular"),
        )
        noun_primary = CanonicalAnalysis("Berlin", CanonicalPos.NOUN)
        self.assertEqual(
            encode_contextual_grammar(noun_context, noun_primary),
            encode_features(noun_context.features),
        )

        adjective_primary = CanonicalAnalysis("knipsend", CanonicalPos.ADJECTIVE)
        self.assertEqual(encode_contextual_grammar(context, adjective_primary), 0)
        self.assertEqual(
            encode_contextual_grammar(
                CanonicalAnalysis("x", CanonicalPos.UNKNOWN, CanonicalFeatures(number="singular")),
                CanonicalAnalysis("x", CanonicalPos.UNKNOWN),
            ),
            0,
        )
        self.assertEqual(
            encode_contextual_grammar(
                CanonicalAnalysis("x", CanonicalPos.VERB),
                CanonicalAnalysis("x", CanonicalPos.VERB),
            ),
            0,
        )

    def test_analyzer_contract_drift_is_not_hidden_by_pos_mismatch(self):
        invalid_context = CanonicalAnalysis(
            "x",
            CanonicalPos.NOUN,
            CanonicalFeatures(mood="indicative"),
        )
        mismatched_primary = CanonicalAnalysis("x", CanonicalPos.ADJECTIVE)
        with self.assertRaisesRegex(GrammarDescriptorError, "requires a verb form"):
            encode_contextual_grammar(invalid_context, mismatched_primary)

        with self.assertRaisesRegex(GrammarDescriptorError, "canonical analyses"):
            encode_contextual_grammar(object(), mismatched_primary)


if __name__ == "__main__":
    unittest.main()

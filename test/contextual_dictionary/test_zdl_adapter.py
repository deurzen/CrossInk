from pathlib import Path
from types import SimpleNamespace
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from dictionary.contextual.analysis_policy import CanonicalPos  # noqa: E402
from dictionary.contextual.zdl_adapter import (  # noqa: E402
    ZdlAdapterError,
    ZdlContextAnalyzer,
    ZdlLimits,
)


class Morph:
    def __init__(self, values=None, result=None):
        self.values = values or {}
        self.result = result

    def to_dict(self):
        return self.values if self.result is None else self.result


def token(text, idx, lemma, pos, tag, morph=None, is_space=False):
    return SimpleNamespace(
        text=text,
        idx=idx,
        lemma_=lemma,
        pos_=pos,
        tag_=tag,
        morph=Morph(morph),
        is_space=is_space,
    )


class FakeNlp:
    def __init__(self, tokens=(), error=None):
        self.tokens = tokens
        self.error = error
        self.sentences = []

    def __call__(self, sentence):
        self.sentences.append(sentence)
        if self.error:
            raise self.error
        return iter(self.tokens)


class ZdlAdapterTest(unittest.TestCase):
    def test_maps_contextual_tokens_and_round_trips_unicode_offsets(self):
        sentence = "Über Liebe und liebe."
        source = FakeNlp(
            (
                token("Über", 0, "über", "ADP", "APPR", {"Case": "Acc"}),
                token("Liebe", 5, "Liebe", "NOUN", "NN", {"Number": "Sing"}),
                token("und", 11, "und", "CCONJ", "KON"),
                token("liebe", 15, "lieben", "VERB", "VVFIN", {"Tense": "Pres"}),
                token(".", 20, ".", "PUNCT", "$."),
            )
        )
        result = tuple(ZdlContextAnalyzer(source).analyze_sentence(sentence))
        self.assertEqual(source.sentences, [sentence])
        self.assertEqual([(item.surface, item.start, item.end) for item in result], [
            ("Über", 0, 4), ("Liebe", 5, 10), ("und", 11, 14), ("liebe", 15, 20), (".", 20, 21)
        ])
        self.assertEqual(result[1].analysis.part_of_speech, CanonicalPos.NOUN)
        self.assertEqual(result[3].analysis.part_of_speech, CanonicalPos.VERB)
        self.assertEqual(result[3].analysis.lemma, "lieben")
        self.assertEqual(result[3].provider_tag, "VVFIN")

    def test_skips_space_tokens_without_collapsing_offsets(self):
        sentence = "eins zwei"
        source = FakeNlp(
            (
                token("eins", 0, "eins", "NUM", "CARD"),
                token(" ", 4, " ", "", "", is_space=True),
                token("zwei", 5, "zwei", "NUM", "CARD"),
            )
        )
        result = tuple(ZdlContextAnalyzer(source).analyze_sentence(sentence))
        self.assertEqual([(item.surface, item.start) for item in result], [("eins", 0), ("zwei", 5)])

    def test_rejects_non_nfc_and_source_offset_mismatch(self):
        with self.assertRaisesRegex(ZdlAdapterError, "must be NFC"):
            ZdlContextAnalyzer(FakeNlp()).analyze_sentence("Ha\u0308user")
        mismatch = FakeNlp((token("Laden", 0, "Laden", "NOUN", "NN"),))
        with self.assertRaisesRegex(ZdlAdapterError, "round-trip"):
            ZdlContextAnalyzer(mismatch).analyze_sentence("laden")
        overlap = FakeNlp(
            (
                token("eins", 0, "eins", "NUM", "CARD"),
                token("ins", 1, "ins", "ADP", "APPR"),
            )
        )
        with self.assertRaisesRegex(ZdlAdapterError, "overlaps"):
            ZdlContextAnalyzer(overlap).analyze_sentence("eins")

    def test_enforces_sentence_token_and_field_caps(self):
        with self.assertRaisesRegex(ZdlAdapterError, "sentence has"):
            ZdlContextAnalyzer(FakeNlp(), ZdlLimits(max_sentence_bytes=3)).analyze_sentence("vier")
        two_tokens = FakeNlp(
            (token("a", 0, "a", "X", "XY"), token("b", 1, "b", "X", "XY"))
        )
        with self.assertRaisesRegex(ZdlAdapterError, "token cap 1"):
            ZdlContextAnalyzer(two_tokens, ZdlLimits(max_tokens=1)).analyze_sentence("ab")
        with self.assertRaisesRegex(ZdlAdapterError, "lemma exceeds"):
            ZdlContextAnalyzer(
                FakeNlp((token("a", 0, "long", "X", "XY"),)),
                ZdlLimits(max_lemma_bytes=3),
            ).analyze_sentence("a")
        with self.assertRaisesRegex(ZdlAdapterError, "morphology cap 1"):
            ZdlContextAnalyzer(
                FakeNlp((token("a", 0, "a", "X", "XY", {"Case": "Nom", "Number": "Sing"}),)),
                ZdlLimits(max_morph_features=1),
            ).analyze_sentence("a")

    def test_rejects_malformed_model_fields(self):
        malformed = (
            (token("a", 0, "", "X", "XY"), "invalid lemma"),
            (token("a", 0, "a", None, "XY"), "invalid universal POS"),
            (SimpleNamespace(text="a", idx=True), "invalid offset"),
            (SimpleNamespace(text="a", idx=0, lemma_="a", pos_="X", tag_="XY", morph=object()), "invalid morphology"),
            (SimpleNamespace(text="a", idx=0, lemma_="a", pos_="X", tag_="XY", morph=Morph(result=[])), "not a mapping"),
        )
        for bad_token, message in malformed:
            with self.subTest(message=message):
                with self.assertRaisesRegex(ZdlAdapterError, message):
                    ZdlContextAnalyzer(FakeNlp((bad_token,))).analyze_sentence("a")

    def test_wraps_inference_and_iteration_failures(self):
        with self.assertRaisesRegex(ZdlAdapterError, "model inference failed"):
            ZdlContextAnalyzer(FakeNlp(error=RuntimeError("model failed"))).analyze_sentence("Text")

        def broken_document():
            yield token("a", 0, "a", "X", "XY")
            raise RuntimeError("decode failed")

        with self.assertRaisesRegex(ZdlAdapterError, "cannot iterate.*decode failed"):
            ZdlContextAnalyzer(FakeNlp(broken_document())).analyze_sentence("a")


if __name__ == "__main__":
    unittest.main()

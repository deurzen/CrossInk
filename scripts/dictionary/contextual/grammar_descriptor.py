"""Contextual grammar descriptor layout 1."""

from __future__ import annotations

from .analysis_policy import CanonicalAnalysis, CanonicalFeatures, CanonicalPos

GRAMMAR_DESCRIPTOR_VERSION = 1
RESERVED_MASK = 0xFFFE0000

_CASE_SHIFT = 0
_DEGREE_SHIFT = 3
_GENDER_SHIFT = 5
_MOOD_SHIFT = 7
_NUMBER_SHIFT = 9
_PERSON_SHIFT = 11
_TENSE_SHIFT = 13
_VERB_FORM_SHIFT = 15

_CASE_MASK = 0x7
_FIELD_MASK = 0x3

_CASE_VALUES = (None, "nominative", "accusative", "dative", "genitive")
_DEGREE_VALUES = (None, "positive", "comparative", "superlative")
_GENDER_VALUES = (None, "masculine", "feminine", "neuter")
_MOOD_VALUES = (None, "indicative", "subjunctive", "imperative")
_NUMBER_VALUES = (None, "singular", "plural")
_PERSON_VALUES = (None, "first", "second", "third")
_TENSE_VALUES = (None, "present", "past", "perfect")
_VERB_FORM_VALUES = (None, "finite", "infinitive", "participle")


class GrammarDescriptorError(ValueError):
    pass


def _encode_value(field: str, value: str | None, values: tuple[str | None, ...]) -> int:
    try:
        return values.index(value)
    except ValueError as error:
        raise GrammarDescriptorError(f"unsupported grammar {field}: {value!r}") from error


def _decode_value(field: str, code: int, values: tuple[str | None, ...]) -> str | None:
    if code >= len(values):
        raise GrammarDescriptorError(f"reserved grammar {field} code: {code}")
    return values[code]


def _codes(descriptor: int) -> dict[str, int]:
    return {
        "case": (descriptor >> _CASE_SHIFT) & _CASE_MASK,
        "degree": (descriptor >> _DEGREE_SHIFT) & _FIELD_MASK,
        "gender": (descriptor >> _GENDER_SHIFT) & _FIELD_MASK,
        "mood": (descriptor >> _MOOD_SHIFT) & _FIELD_MASK,
        "number": (descriptor >> _NUMBER_SHIFT) & _FIELD_MASK,
        "person": (descriptor >> _PERSON_SHIFT) & _FIELD_MASK,
        "tense": (descriptor >> _TENSE_SHIFT) & _FIELD_MASK,
        "verb_form": (descriptor >> _VERB_FORM_SHIFT) & _FIELD_MASK,
    }


def validate_descriptor(descriptor: int) -> None:
    if not isinstance(descriptor, int) or isinstance(descriptor, bool):
        raise GrammarDescriptorError("grammar descriptor must be an integer")
    if descriptor < 0 or descriptor > 0xFFFFFFFF:
        raise GrammarDescriptorError("grammar descriptor must fit uint32")
    if descriptor & RESERVED_MASK:
        raise GrammarDescriptorError("grammar descriptor reserved bits are nonzero")

    values = _codes(descriptor)
    _decode_value("case", values["case"], _CASE_VALUES)
    _decode_value("number", values["number"], _NUMBER_VALUES)

    verb_form = values["verb_form"]
    if verb_form == 2 and any(values[name] for name in values if name != "verb_form"):
        raise GrammarDescriptorError("infinitive descriptor has incompatible features")
    if verb_form == 3 and (values["mood"] or values["person"]):
        raise GrammarDescriptorError("participle descriptor has mood or person")
    if verb_form == 1 and (values["case"] or values["degree"] or values["gender"]):
        raise GrammarDescriptorError("finite descriptor has nominal or degree features")
    if verb_form == 0 and (values["mood"] or values["tense"]):
        raise GrammarDescriptorError("mood or tense requires a verb form")


def encode_features(features: CanonicalFeatures) -> int:
    if not isinstance(features, CanonicalFeatures):
        raise GrammarDescriptorError("grammar features have invalid type")
    descriptor = (
        _encode_value("case", features.case, _CASE_VALUES) << _CASE_SHIFT
        | _encode_value("degree", features.degree, _DEGREE_VALUES) << _DEGREE_SHIFT
        | _encode_value("gender", features.gender, _GENDER_VALUES) << _GENDER_SHIFT
        | _encode_value("mood", features.mood, _MOOD_VALUES) << _MOOD_SHIFT
        | _encode_value("number", features.number, _NUMBER_VALUES) << _NUMBER_SHIFT
        | _encode_value("person", features.person, _PERSON_VALUES) << _PERSON_SHIFT
        | _encode_value("tense", features.tense, _TENSE_VALUES) << _TENSE_SHIFT
        | _encode_value("verb form", features.verb_form, _VERB_FORM_VALUES) << _VERB_FORM_SHIFT
    )
    validate_descriptor(descriptor)
    return descriptor


def decode_features(descriptor: int) -> CanonicalFeatures:
    validate_descriptor(descriptor)
    values = _codes(descriptor)
    return CanonicalFeatures(
        case=_decode_value("case", values["case"], _CASE_VALUES),
        degree=_decode_value("degree", values["degree"], _DEGREE_VALUES),
        gender=_decode_value("gender", values["gender"], _GENDER_VALUES),
        mood=_decode_value("mood", values["mood"], _MOOD_VALUES),
        number=_decode_value("number", values["number"], _NUMBER_VALUES),
        person=_decode_value("person", values["person"], _PERSON_VALUES),
        tense=_decode_value("tense", values["tense"], _TENSE_VALUES),
        verb_form=_decode_value("verb form", values["verb_form"], _VERB_FORM_VALUES),
    )


def encode_contextual_grammar(context: CanonicalAnalysis, primary: CanonicalAnalysis) -> int:
    if not isinstance(context, CanonicalAnalysis) or not isinstance(primary, CanonicalAnalysis):
        raise GrammarDescriptorError("context and primary must be canonical analyses")

    # Validate the versioned contextual contract even when POS later suppresses
    # presentation; analyzer drift must not hide behind an ordinary mismatch.
    descriptor = encode_features(context.features)
    if descriptor == 0 or context.part_of_speech in (CanonicalPos.UNKNOWN, CanonicalPos.OTHER):
        return 0
    if context.part_of_speech == primary.part_of_speech:
        return descriptor
    if {context.part_of_speech, primary.part_of_speech} == {CanonicalPos.NOUN, CanonicalPos.PROPER_NOUN}:
        return descriptor
    return 0

"""Versioned German DWDSmor/ZDL canonical mapping and scoring policy."""

from __future__ import annotations

import unicodedata
from dataclasses import dataclass
from enum import IntEnum
from typing import Mapping

POLICY_VERSION = 1
MAX_ALTERNATIVES = 8
ALTERNATIVE_SCORE_WINDOW = 180


class CanonicalPos(IntEnum):
    """Canonical lexical classes; numeric values are part of the file format."""

    UNKNOWN = 0
    NOUN = 1
    VERB = 2
    ADJECTIVE = 3
    ADVERB = 4
    PRONOUN = 5
    DETERMINER = 6
    ADPOSITION = 7
    CONJUNCTION = 8
    NUMERAL = 9
    PARTICLE = 10
    INTERJECTION = 11
    PROPER_NOUN = 12
    PHRASE = 13
    ABBREVIATION = 14
    OTHER = 15


DWDSMOR_POS_MAP = {
    "ADJ": CanonicalPos.ADJECTIVE,
    "ADV": CanonicalPos.ADVERB,
    "ART": CanonicalPos.DETERMINER,
    "CARD": CanonicalPos.NUMERAL,
    "CONJ": CanonicalPos.CONJUNCTION,
    "DEM": CanonicalPos.DETERMINER,
    "FRAC": CanonicalPos.NUMERAL,
    "INDEF": CanonicalPos.DETERMINER,
    "INTJ": CanonicalPos.INTERJECTION,
    "NN": CanonicalPos.NOUN,
    "NPROP": CanonicalPos.PROPER_NOUN,
    "ORD": CanonicalPos.NUMERAL,
    "POSS": CanonicalPos.DETERMINER,
    "POSTP": CanonicalPos.ADPOSITION,
    "PPRO": CanonicalPos.PRONOUN,
    "PREP": CanonicalPos.ADPOSITION,
    "PREPART": CanonicalPos.ADPOSITION,
    "PROADV": CanonicalPos.ADVERB,
    "PTCL": CanonicalPos.PARTICLE,
    "PUNCT": CanonicalPos.OTHER,
    "REL": CanonicalPos.PRONOUN,
    "V": CanonicalPos.VERB,
    "WPRO": CanonicalPos.PRONOUN,
}

ZDL_UPOS_MAP = {
    "ADJ": CanonicalPos.ADJECTIVE,
    "ADP": CanonicalPos.ADPOSITION,
    "ADV": CanonicalPos.ADVERB,
    "AUX": CanonicalPos.VERB,
    "CCONJ": CanonicalPos.CONJUNCTION,
    "DET": CanonicalPos.DETERMINER,
    "INTJ": CanonicalPos.INTERJECTION,
    "NOUN": CanonicalPos.NOUN,
    "NUM": CanonicalPos.NUMERAL,
    "PART": CanonicalPos.PARTICLE,
    "PRON": CanonicalPos.PRONOUN,
    "PROPN": CanonicalPos.PROPER_NOUN,
    "PUNCT": CanonicalPos.OTHER,
    "SCONJ": CanonicalPos.CONJUNCTION,
    "SYM": CanonicalPos.OTHER,
    "VERB": CanonicalPos.VERB,
    "X": CanonicalPos.OTHER,
}

ZDL_STTS_MAP = {
    "$(": CanonicalPos.OTHER,
    "$.": CanonicalPos.OTHER,
    "$,": CanonicalPos.OTHER,
    "ADJA": CanonicalPos.ADJECTIVE,
    "ADJD": CanonicalPos.ADJECTIVE,
    "ADV": CanonicalPos.ADVERB,
    "APPO": CanonicalPos.ADPOSITION,
    "APPR": CanonicalPos.ADPOSITION,
    "APPR_ART": CanonicalPos.ADPOSITION,
    "APZR": CanonicalPos.ADPOSITION,
    "ART": CanonicalPos.DETERMINER,
    "CARD": CanonicalPos.NUMERAL,
    "FM": CanonicalPos.OTHER,
    "ITJ": CanonicalPos.INTERJECTION,
    "KOKOM": CanonicalPos.CONJUNCTION,
    "KON": CanonicalPos.CONJUNCTION,
    "KOUI": CanonicalPos.CONJUNCTION,
    "KOUS": CanonicalPos.CONJUNCTION,
    "NE": CanonicalPos.PROPER_NOUN,
    "NN": CanonicalPos.NOUN,
    "PDAT": CanonicalPos.DETERMINER,
    "PDS": CanonicalPos.PRONOUN,
    "PIAT": CanonicalPos.DETERMINER,
    "PIDAT": CanonicalPos.DETERMINER,
    "PIS": CanonicalPos.PRONOUN,
    "PPER": CanonicalPos.PRONOUN,
    "PPOSAT": CanonicalPos.DETERMINER,
    "PPOSS": CanonicalPos.PRONOUN,
    "PRELAT": CanonicalPos.DETERMINER,
    "PRELS": CanonicalPos.PRONOUN,
    "PRF": CanonicalPos.PRONOUN,
    "PROAV": CanonicalPos.ADVERB,
    "PTKA": CanonicalPos.PARTICLE,
    "PTKANT": CanonicalPos.PARTICLE,
    "PTKNEG": CanonicalPos.PARTICLE,
    "PTKVZ": CanonicalPos.PARTICLE,
    "PTKZU": CanonicalPos.PARTICLE,
    "PWAT": CanonicalPos.DETERMINER,
    "PWAV": CanonicalPos.ADVERB,
    "PWS": CanonicalPos.PRONOUN,
    "TRUNC": CanonicalPos.OTHER,
    "VAFIN": CanonicalPos.VERB,
    "VAIMP": CanonicalPos.VERB,
    "VAINF": CanonicalPos.VERB,
    "VAPP": CanonicalPos.VERB,
    "VMFIN": CanonicalPos.VERB,
    "VMINF": CanonicalPos.VERB,
    "VMPP": CanonicalPos.VERB,
    "VVFIN": CanonicalPos.VERB,
    "VVIMP": CanonicalPos.VERB,
    "VVINF": CanonicalPos.VERB,
    "VVIZU": CanonicalPos.VERB,
    "VVPP": CanonicalPos.VERB,
    "XY": CanonicalPos.OTHER,
}

_DWDSMOR_FEATURE_VALUES = {
    "case": {"Nom": "nominative", "Acc": "accusative", "Dat": "dative", "Gen": "genitive"},
    "degree": {"Pos": "positive", "Comp": "comparative", "Sup": "superlative"},
    "gender": {"Masc": "masculine", "Fem": "feminine", "Neut": "neuter"},
    "mood": {"Ind": "indicative", "Subj": "subjunctive", "Imp": "imperative"},
    "nonfinite": {"Inf": "infinitive", "Part": "participle"},
    "number": {"Sg": "singular", "Pl": "plural"},
    "person": {"1": "first", "2": "second", "3": "third"},
    "tense": {"Pres": "present", "Past": "past", "Perf": "perfect"},
}

_ZDL_FEATURE_VALUES = {
    "Case": {"Nom": "nominative", "Acc": "accusative", "Dat": "dative", "Gen": "genitive"},
    "Degree": {"Pos": "positive", "Cmp": "comparative", "Sup": "superlative"},
    "Gender": {"Masc": "masculine", "Fem": "feminine", "Neut": "neuter"},
    "Mood": {"Ind": "indicative", "Sub": "subjunctive", "Imp": "imperative"},
    "Number": {"Sing": "singular", "Plur": "plural"},
    "Person": {"1": "first", "2": "second", "3": "third"},
    "Tense": {"Pres": "present", "Past": "past"},
    "VerbForm": {"Fin": "finite", "Inf": "infinitive", "Part": "participle"},
}


@dataclass(frozen=True)
class CanonicalFeatures:
    case: str | None = None
    degree: str | None = None
    gender: str | None = None
    mood: str | None = None
    number: str | None = None
    person: str | None = None
    tense: str | None = None
    verb_form: str | None = None

    def populated(self) -> dict[str, str]:
        return {name: value for name, value in self.__dict__.items() if value is not None}


@dataclass(frozen=True)
class CanonicalAnalysis:
    lemma: str
    part_of_speech: CanonicalPos
    features: CanonicalFeatures = CanonicalFeatures()


@dataclass(frozen=True)
class AnalysisScore:
    total: int
    lemma_agrees: bool
    pos_agrees: bool
    feature_matches: int
    feature_conflicts: int


def _normalize_lemma(lemma: str) -> str:
    return unicodedata.normalize("NFC", lemma.strip())


def _canonical_features(
    values: Mapping[str, str | None],
    mappings: Mapping[str, Mapping[str, str]],
) -> CanonicalFeatures:
    normalized: dict[str, str] = {}
    for source_name, value_map in mappings.items():
        value = values.get(source_name)
        if value in value_map:
            target_name = "verb_form" if source_name in ("nonfinite", "VerbForm") else source_name.lower()
            normalized[target_name] = value_map[value]
    return CanonicalFeatures(**normalized)


def map_dwdsmor_analysis(lemma: str, pos: str | None, **features: str | None) -> CanonicalAnalysis:
    return CanonicalAnalysis(
        lemma=_normalize_lemma(lemma),
        part_of_speech=DWDSMOR_POS_MAP.get(pos or "", CanonicalPos.UNKNOWN),
        features=_canonical_features(features, _DWDSMOR_FEATURE_VALUES),
    )


def map_zdl_analysis(
    lemma: str,
    upos: str | None,
    tag: str | None,
    morph: Mapping[str, str],
) -> CanonicalAnalysis:
    part_of_speech = ZDL_UPOS_MAP.get(upos or "")
    if part_of_speech is None:
        part_of_speech = ZDL_STTS_MAP.get(tag or "", CanonicalPos.UNKNOWN)
    return CanonicalAnalysis(
        lemma=_normalize_lemma(lemma),
        part_of_speech=part_of_speech,
        features=_canonical_features(morph, _ZDL_FEATURE_VALUES),
    )


def _pos_compatible(left: CanonicalPos, right: CanonicalPos) -> bool:
    return {left, right} == {CanonicalPos.NOUN, CanonicalPos.PROPER_NOUN}


def score_analysis(context: CanonicalAnalysis, morphology: CanonicalAnalysis) -> AnalysisScore:
    """Score a DWDSmor candidate without removing any candidate."""
    pos_agrees = context.part_of_speech == morphology.part_of_speech
    if pos_agrees:
        total = 600
    elif CanonicalPos.UNKNOWN in (context.part_of_speech, morphology.part_of_speech):
        total = 0
    elif _pos_compatible(context.part_of_speech, morphology.part_of_speech):
        total = 350
    else:
        total = -600

    lemma_agrees = context.lemma.casefold() == morphology.lemma.casefold()
    total += 400 if lemma_agrees else -100

    context_features = context.features.populated()
    morphology_features = morphology.features.populated()
    feature_matches = 0
    feature_conflicts = 0
    for name in context_features.keys() & morphology_features.keys():
        if context_features[name] == morphology_features[name]:
            feature_matches += 1
        else:
            feature_conflicts += 1
    total += feature_matches * 30
    total -= feature_conflicts * 20

    return AnalysisScore(
        total=total,
        lemma_agrees=lemma_agrees,
        pos_agrees=pos_agrees,
        feature_matches=feature_matches,
        feature_conflicts=feature_conflicts,
    )

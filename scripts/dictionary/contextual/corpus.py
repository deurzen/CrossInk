"""Versioned ambiguity corpus loading and provider-level evaluation."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any

from .analysis_policy import CanonicalAnalysis, CanonicalPos
from .pipeline import (
    AnalysisFuser,
    ContextAnalyzer,
    ContextToken,
    MorphologyAnalyzer,
    SentenceCandidateAugmenter,
)

MAX_CORPUS_CASES = 1024
MAX_CORPUS_SENTENCE_BYTES = 64 * 1024
_POS_BY_LABEL = {
    pos.name.lower().replace("_", "-"): pos
    for pos in CanonicalPos
}


class CorpusError(ValueError):
    pass


@dataclass(frozen=True)
class GoldTarget:
    surface: str
    occurrence: int
    lemma: str
    part_of_speech: CanonicalPos
    linked_surface: str | None = None


@dataclass(frozen=True)
class GoldCase:
    case_id: str
    category: str
    sentence: str
    target: GoldTarget


@dataclass(frozen=True)
class CaseEvaluation:
    case_id: str
    category: str
    expected: CanonicalAnalysis
    context: CanonicalAnalysis
    morphology: tuple[CanonicalAnalysis, ...]
    fused: tuple[CanonicalAnalysis, ...]
    context_correct: bool
    morphology_covered: bool
    fused_primary_correct: bool


@dataclass(frozen=True)
class CorpusEvaluation:
    cases: tuple[CaseEvaluation, ...]

    @property
    def context_correct(self) -> int:
        return sum(case.context_correct for case in self.cases)

    @property
    def morphology_covered(self) -> int:
        return sum(case.morphology_covered for case in self.cases)

    @property
    def fused_primary_correct(self) -> int:
        return sum(case.fused_primary_correct for case in self.cases)

    def summary(self) -> dict[str, int]:
        return {
            "cases": len(self.cases),
            "contextCorrect": self.context_correct,
            "morphologyCovered": self.morphology_covered,
            "fusedPrimaryCorrect": self.fused_primary_correct,
        }


def _required_text(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value:
        raise CorpusError(f"{name} must be non-empty text")
    return value


def load_corpus(path: Path) -> tuple[GoldCase, ...]:
    try:
        root = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CorpusError(f"cannot read corpus: {error}") from error
    if not isinstance(root, dict) or root.get("schemaVersion") != 1:
        raise CorpusError("unsupported corpus schema")
    if root.get("language") != "de":
        raise CorpusError("corpus language must be de")
    values = root.get("cases")
    if not isinstance(values, list) or not values:
        raise CorpusError("corpus cases must be a non-empty array")
    if len(values) > MAX_CORPUS_CASES:
        raise CorpusError(f"corpus exceeds {MAX_CORPUS_CASES} cases")

    cases = []
    seen_ids = set()
    for index, value in enumerate(values):
        if not isinstance(value, dict):
            raise CorpusError(f"cases[{index}] must be an object")
        case_id = _required_text(value.get("id"), f"cases[{index}].id")
        if case_id in seen_ids:
            raise CorpusError(f"duplicate case id: {case_id}")
        seen_ids.add(case_id)
        category = _required_text(value.get("category"), f"cases[{index}].category")
        sentence = _required_text(value.get("sentence"), f"cases[{index}].sentence")
        if len(sentence.encode("utf-8")) > MAX_CORPUS_SENTENCE_BYTES:
            raise CorpusError(f"cases[{index}].sentence exceeds byte cap")
        target = value.get("target")
        if not isinstance(target, dict):
            raise CorpusError(f"cases[{index}].target must be an object")
        occurrence = target.get("occurrence")
        if not isinstance(occurrence, int) or isinstance(occurrence, bool) or occurrence < 0:
            raise CorpusError(f"cases[{index}].target.occurrence must be nonnegative")
        pos_label = _required_text(target.get("pos"), f"cases[{index}].target.pos")
        part_of_speech = _POS_BY_LABEL.get(pos_label)
        if part_of_speech is None:
            raise CorpusError(f"cases[{index}].target.pos is unsupported: {pos_label}")
        linked_surface = target.get("linkedSurface")
        if linked_surface is not None and not isinstance(linked_surface, str):
            raise CorpusError(f"cases[{index}].target.linkedSurface must be text")
        gold_target = GoldTarget(
            surface=_required_text(target.get("surface"), f"cases[{index}].target.surface"),
            occurrence=occurrence,
            lemma=_required_text(target.get("lemma"), f"cases[{index}].target.lemma"),
            part_of_speech=part_of_speech,
            linked_surface=linked_surface,
        )
        if sentence.count(gold_target.surface) <= gold_target.occurrence:
            raise CorpusError(f"cases[{index}] target occurrence is absent from sentence")
        if linked_surface is not None and linked_surface not in sentence:
            raise CorpusError(f"cases[{index}] linked surface is absent from sentence")
        cases.append(GoldCase(case_id, category, sentence, gold_target))
    return tuple(cases)


def _semantic_key(analysis: CanonicalAnalysis) -> tuple[str, CanonicalPos]:
    return analysis.lemma.casefold(), analysis.part_of_speech


def _unique_lexical_analyses(values) -> tuple[CanonicalAnalysis, ...]:
    by_key = {}
    for value in values:
        by_key.setdefault(_semantic_key(value), value)
    return tuple(
        by_key[key]
        for key in sorted(by_key, key=lambda item: (item[0].encode("utf-8"), int(item[1])))
    )


def evaluate_corpus(
    cases: tuple[GoldCase, ...],
    context_analyzer: ContextAnalyzer,
    morphology_analyzer: MorphologyAnalyzer,
    fuser: AnalysisFuser,
    candidate_augmenter: SentenceCandidateAugmenter | None = None,
) -> CorpusEvaluation:
    evaluations = []
    for case in cases:
        tokens = tuple(context_analyzer.analyze_sentence(case.sentence))
        matching = [token for token in tokens if token.surface == case.target.surface]
        if case.target.occurrence >= len(matching):
            raise CorpusError(f"{case.case_id}: contextual tokenizer did not return target")
        token = matching[case.target.occurrence]
        morphology_rows = tuple(
            tuple(morphology_analyzer.analyze_surface(item.surface))
            for item in tokens
        )
        if candidate_augmenter is not None:
            morphology_rows = tuple(
                tuple(row)
                for row in candidate_augmenter.augment_sentence(
                    case.sentence,
                    tokens,
                    morphology_rows,
                )
            )
            if len(morphology_rows) != len(tokens):
                raise CorpusError(f"{case.case_id}: augmentation row count mismatch")
        token_index = next(index for index, item in enumerate(tokens) if item is token)
        morphology_candidates = morphology_rows[token_index]
        ranked = tuple(fuser.rank(token, morphology_candidates))
        expected = CanonicalAnalysis(case.target.lemma, case.target.part_of_speech)
        expected_key = _semantic_key(expected)
        lexical_morphology = _unique_lexical_analyses(
            candidate.analysis for candidate in morphology_candidates
        )
        fused = tuple(item.analysis for item in ranked)
        evaluations.append(
            CaseEvaluation(
                case_id=case.case_id,
                category=case.category,
                expected=expected,
                context=token.analysis,
                morphology=lexical_morphology,
                fused=fused,
                context_correct=_semantic_key(token.analysis) == expected_key,
                morphology_covered=any(
                    _semantic_key(candidate) == expected_key
                    for candidate in lexical_morphology
                ),
                fused_primary_correct=bool(fused) and _semantic_key(fused[0]) == expected_key,
            )
        )
    return CorpusEvaluation(tuple(evaluations))

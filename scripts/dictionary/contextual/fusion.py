"""Deterministic fusion of contextual evidence and morphology candidates."""

from __future__ import annotations

from dataclasses import dataclass

from .analysis_policy import (
    ALTERNATIVE_SCORE_WINDOW,
    MAX_ALTERNATIVES,
    AnalysisScore,
    CanonicalAnalysis,
    score_analysis,
)
from .pipeline import AnalysisProvenance, ContextToken, MorphologyCandidate, RankedAnalysis

MIN_NORMALIZED_SCORE = -1000
MAX_NORMALIZED_SCORE = 1250


@dataclass(frozen=True)
class _ScoredCandidate:
    analysis: CanonicalAnalysis
    evidence: AnalysisScore
    provenance: AnalysisProvenance


def _lexical_key(analysis: CanonicalAnalysis) -> tuple[bytes, int]:
    return analysis.lemma.encode("utf-8"), int(analysis.part_of_speech)


def _feature_key(analysis: CanonicalAnalysis) -> tuple[tuple[str, str], ...]:
    return tuple(sorted(analysis.features.populated().items()))


def normalize_score(score: int) -> int:
    """Map the bounded policy score domain to language.bin confidence 0..1000."""
    clamped = min(MAX_NORMALIZED_SCORE, max(MIN_NORMALIZED_SCORE, score))
    numerator = (clamped - MIN_NORMALIZED_SCORE) * 1000
    denominator = MAX_NORMALIZED_SCORE - MIN_NORMALIZED_SCORE
    return (numerator + denominator // 2) // denominator


class GermanAnalysisFuser:
    """Rank morphology candidates and collapse inflection variants by lexical key."""

    def rank(
        self,
        token: ContextToken,
        candidates: tuple[MorphologyCandidate, ...],
    ) -> tuple[RankedAnalysis, ...]:
        if not candidates:
            return ()

        best_by_lexical_key: dict[tuple[bytes, int], _ScoredCandidate] = {}
        for candidate in candidates:
            scored = _ScoredCandidate(
                candidate.analysis,
                score_analysis(token.analysis, candidate.analysis),
                candidate.provenance,
            )
            key = _lexical_key(candidate.analysis)
            previous = best_by_lexical_key.get(key)
            if previous is None:
                best_by_lexical_key[key] = scored
                continue
            combined_provenance = previous.provenance | scored.provenance
            if self._variant_order(scored) < self._variant_order(previous):
                best_by_lexical_key[key] = _ScoredCandidate(
                    scored.analysis,
                    scored.evidence,
                    combined_provenance,
                )
            else:
                best_by_lexical_key[key] = _ScoredCandidate(
                    previous.analysis,
                    previous.evidence,
                    combined_provenance,
                )

        ordered = sorted(best_by_lexical_key.values(), key=self._candidate_order)
        top_score = ordered[0].evidence.total
        minimum_score = top_score - ALTERNATIVE_SCORE_WINDOW
        retained = [candidate for candidate in ordered if candidate.evidence.total >= minimum_score]
        retained = retained[:MAX_ALTERNATIVES]
        return tuple(
            RankedAnalysis(
                candidate.analysis,
                candidate.evidence.total,
                candidate.provenance,
            )
            for candidate in retained
        )

    @staticmethod
    def _variant_order(candidate: _ScoredCandidate):
        return -candidate.evidence.total, _feature_key(candidate.analysis)

    @staticmethod
    def _candidate_order(candidate: _ScoredCandidate):
        return (
            -candidate.evidence.total,
            _lexical_key(candidate.analysis),
            _feature_key(candidate.analysis),
        )

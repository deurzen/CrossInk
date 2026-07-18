"""Finite German separable-verb recombination from bounded sentence context."""

from __future__ import annotations

from dataclasses import dataclass
import unicodedata

from .analysis_policy import CanonicalAnalysis, CanonicalPos
from .pipeline import (
    AnalysisProvenance,
    ContextToken,
    MorphologyAnalyzer,
    MorphologyCandidate,
)


class SeparableVerbError(RuntimeError):
    pass


@dataclass(frozen=True)
class SeparableVerbLimits:
    max_particles: int = 32
    max_lookback_tokens: int = 64
    max_base_lemmas: int = 32
    max_lexical_analyses: int = 256
    max_lemma_bytes: int = 96

    def __post_init__(self) -> None:
        for name, value in self.__dict__.items():
            if not isinstance(value, int) or value <= 0:
                raise ValueError(f"{name} must be a positive integer")


_BOUNDARY_SURFACES = frozenset((".", ";", ":", "!", "?"))
_SEPARABLE_PARTICLE_TAG = "PTKVZ"


def _analysis_key(analysis: CanonicalAnalysis):
    return (
        analysis.lemma.encode("utf-8"),
        int(analysis.part_of_speech),
        tuple(sorted(analysis.features.populated().items())),
    )


class GermanSeparableVerbRecombiner:
    """Add lexicon-validated particle+base lemmas to the finite verb token."""

    def __init__(
        self,
        lexical_analyzer: MorphologyAnalyzer,
        limits: SeparableVerbLimits = SeparableVerbLimits(),
    ):
        self._lexical_analyzer = lexical_analyzer
        self._limits = limits

    def augment_sentence(
        self,
        sentence: str,
        tokens: tuple[ContextToken, ...],
        candidates: tuple[tuple[MorphologyCandidate, ...], ...],
    ) -> tuple[tuple[MorphologyCandidate, ...], ...]:
        del sentence
        if len(tokens) != len(candidates):
            raise SeparableVerbError("candidate rows do not match contextual tokens")
        rows = [list(row) for row in candidates]
        particle_count = 0
        for particle_index, particle in enumerate(tokens):
            if particle.provider_tag != _SEPARABLE_PARTICLE_TAG:
                continue
            particle_count += 1
            if particle_count > self._limits.max_particles:
                raise SeparableVerbError(
                    f"sentence exceeds particle cap {self._limits.max_particles}"
                )
            finite_index = self._nearest_finite_verb(tokens, particle_index)
            if finite_index is None:
                continue
            additions = self._validated_additions(
                particle,
                tokens[finite_index],
                candidates[finite_index],
            )
            by_analysis = {candidate.analysis: candidate.provenance for candidate in rows[finite_index]}
            for candidate in additions:
                by_analysis[candidate.analysis] = (
                    by_analysis.get(candidate.analysis, AnalysisProvenance(0))
                    | candidate.provenance
                )
            rows[finite_index] = [
                MorphologyCandidate(analysis, provenance)
                for analysis, provenance in sorted(
                    by_analysis.items(),
                    key=lambda item: _analysis_key(item[0]),
                )
            ]
        return tuple(tuple(row) for row in rows)

    def _nearest_finite_verb(
        self,
        tokens: tuple[ContextToken, ...],
        particle_index: int,
    ) -> int | None:
        first_index = max(0, particle_index - self._limits.max_lookback_tokens)
        for index in range(particle_index - 1, first_index - 1, -1):
            token = tokens[index]
            if token.surface in _BOUNDARY_SURFACES:
                return None
            if (
                token.analysis.part_of_speech == CanonicalPos.VERB
                and token.analysis.features.verb_form == "finite"
            ):
                return index
        return None

    def _validated_additions(
        self,
        particle: ContextToken,
        finite: ContextToken,
        base_candidates: tuple[MorphologyCandidate, ...],
    ) -> tuple[MorphologyCandidate, ...]:
        base_lemmas = {
            candidate.analysis.lemma
            for candidate in base_candidates
            if candidate.analysis.part_of_speech == CanonicalPos.VERB
        }
        if not base_lemmas and finite.analysis.part_of_speech == CanonicalPos.VERB:
            base_lemmas.add(finite.analysis.lemma)
        if len(base_lemmas) > self._limits.max_base_lemmas:
            raise SeparableVerbError(
                f"finite token exceeds base-lemma cap {self._limits.max_base_lemmas}"
            )

        additions: dict[CanonicalAnalysis, AnalysisProvenance] = {}
        prefix = particle.analysis.lemma.casefold()
        for base_lemma in sorted(base_lemmas, key=lambda value: value.encode("utf-8")):
            combined = unicodedata.normalize("NFC", prefix + base_lemma.casefold())
            if len(combined.encode("utf-8")) > self._limits.max_lemma_bytes:
                continue
            lexical = []
            for candidate in self._lexical_analyzer.analyze_surface(combined):
                if len(lexical) == self._limits.max_lexical_analyses:
                    raise SeparableVerbError(
                        "combined lemma exceeds lexical-analysis cap "
                        f"{self._limits.max_lexical_analyses}"
                    )
                if not isinstance(candidate, MorphologyCandidate):
                    raise SeparableVerbError("lexical analyzer returned an invalid candidate")
                lexical.append(candidate)
            for candidate in lexical:
                analysis = candidate.analysis
                if (
                    analysis.part_of_speech == CanonicalPos.VERB
                    and analysis.lemma.casefold() == combined.casefold()
                ):
                    additions[analysis] = (
                        additions.get(analysis, AnalysisProvenance(0))
                        | candidate.provenance
                        | AnalysisProvenance.CONTEXT_RECOMBINATION
                    )
        return tuple(
            MorphologyCandidate(analysis, provenance)
            for analysis, provenance in sorted(
                additions.items(),
                key=lambda item: _analysis_key(item[0]),
            )
        )

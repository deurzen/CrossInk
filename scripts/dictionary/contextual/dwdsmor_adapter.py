"""Bounded adapter from DWDSmor traversals to canonical analyses."""

from __future__ import annotations

from dataclasses import dataclass
import unicodedata
from typing import Any, Iterable

from .analysis_policy import CanonicalAnalysis, map_dwdsmor_analysis
from .pipeline import AnalysisProvenance, MorphologyCandidate


class DwdsmorAdapterError(RuntimeError):
    pass


@dataclass(frozen=True)
class DwdsmorLimits:
    max_surface_bytes: int = 255
    max_traversals: int = 256
    max_lemma_bytes: int = 96
    max_tag_bytes: int = 32
    max_unique_analyses: int = 256

    def __post_init__(self) -> None:
        for name, value in self.__dict__.items():
            if not isinstance(value, int) or value <= 0:
                raise ValueError(f"{name} must be a positive integer")


_MAPPED_FEATURES = (
    "case",
    "degree",
    "gender",
    "mood",
    "nonfinite",
    "number",
    "person",
    "tense",
)


def _analysis_key(analysis: CanonicalAnalysis) -> tuple[bytes, int, tuple[tuple[str, str], ...]]:
    return (
        analysis.lemma.encode("utf-8"),
        int(analysis.part_of_speech),
        tuple(sorted(analysis.features.populated().items())),
    )


class DwdsmorMorphologyAnalyzer:
    """Convert an injected DWDSmor analyzer without exposing SFST types."""

    def __init__(self, analyzer: Any, limits: DwdsmorLimits = DwdsmorLimits()):
        if not callable(getattr(analyzer, "analyze", None)):
            raise ValueError("analyzer must provide analyze(surface)")
        self._analyzer = analyzer
        self._limits = limits

    @classmethod
    def from_open_edition(
        cls,
        limits: DwdsmorLimits = DwdsmorLimits(),
    ) -> "DwdsmorMorphologyAnalyzer":
        try:
            import dwdsmor
        except Exception as error:
            raise DwdsmorAdapterError(f"cannot import pinned DWDSmor: {error}") from error
        if dwdsmor.edition.strip() != "open":
            raise DwdsmorAdapterError(
                f"DWDSmor Open Edition required, found {dwdsmor.edition!r}"
            )
        try:
            analyzer = dwdsmor.analyzer(automaton_type="lemma")
        except Exception as error:
            raise DwdsmorAdapterError(f"cannot open DWDSmor lemma automaton: {error}") from error
        return cls(analyzer, limits)

    def analyze_surface(self, surface: str) -> Iterable[MorphologyCandidate]:
        if not isinstance(surface, str) or not surface:
            raise DwdsmorAdapterError("surface must be non-empty text")
        normalized = unicodedata.normalize("NFC", surface)
        surface_bytes = len(normalized.encode("utf-8"))
        if surface_bytes > self._limits.max_surface_bytes:
            raise DwdsmorAdapterError(
                f"surface has {surface_bytes} UTF-8 bytes; cap is {self._limits.max_surface_bytes}"
            )

        try:
            traversals = self._analyzer.analyze(normalized)
        except Exception as error:
            raise DwdsmorAdapterError(f"analysis failed: {error}") from error

        unique: set[CanonicalAnalysis] = set()
        traversal_count = 0
        try:
            for traversal in traversals:
                if traversal_count == self._limits.max_traversals:
                    raise DwdsmorAdapterError(
                        f"surface exceeds traversal cap {self._limits.max_traversals}"
                    )
                traversal_count += 1
                analysis = self._convert_traversal(traversal, traversal_count - 1)
                unique.add(analysis)
                if len(unique) > self._limits.max_unique_analyses:
                    raise DwdsmorAdapterError(
                        f"surface exceeds unique-analysis cap {self._limits.max_unique_analyses}"
                    )
        except DwdsmorAdapterError:
            raise
        except Exception as error:
            raise DwdsmorAdapterError(f"cannot iterate analyzer output: {error}") from error
        return tuple(
            MorphologyCandidate(analysis, AnalysisProvenance.PRIMARY_MORPHOLOGY)
            for analysis in sorted(unique, key=_analysis_key)
        )

    def _convert_traversal(self, traversal: Any, index: int) -> CanonicalAnalysis:
        lemma = getattr(traversal, "analysis", None)
        if not isinstance(lemma, str) or not lemma.strip():
            raise DwdsmorAdapterError(f"traversal {index} has invalid lemma")
        normalized_lemma = unicodedata.normalize("NFC", lemma.strip())
        lemma_bytes = len(normalized_lemma.encode("utf-8"))
        if lemma_bytes > self._limits.max_lemma_bytes:
            raise DwdsmorAdapterError(
                f"traversal {index} lemma has {lemma_bytes} UTF-8 bytes; "
                f"cap is {self._limits.max_lemma_bytes}"
            )

        pos = getattr(traversal, "pos", None)
        if not isinstance(pos, str) or not pos:
            raise DwdsmorAdapterError(f"traversal {index} has invalid POS")
        if len(pos.encode("utf-8")) > self._limits.max_tag_bytes:
            raise DwdsmorAdapterError(f"traversal {index} POS exceeds tag cap")

        features = {}
        for name in _MAPPED_FEATURES:
            value = getattr(traversal, name, None)
            if value is not None:
                if not isinstance(value, str):
                    raise DwdsmorAdapterError(
                        f"traversal {index} feature {name} is not text"
                    )
                if len(value.encode("utf-8")) > self._limits.max_tag_bytes:
                    raise DwdsmorAdapterError(
                        f"traversal {index} feature {name} exceeds tag cap"
                    )
            features[name] = value
        return map_dwdsmor_analysis(normalized_lemma, pos, **features)

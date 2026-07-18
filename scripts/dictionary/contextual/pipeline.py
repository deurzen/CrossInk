"""Language-neutral contracts and bounded orchestration for host analysis."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Protocol, runtime_checkable

from .analysis_policy import CanonicalAnalysis


class AnalysisPipelineError(RuntimeError):
    def __init__(self, stage: str, message: str):
        super().__init__(f"{stage}: {message}")
        self.stage = stage


@dataclass(frozen=True)
class AnalyzerLimits:
    max_sentence_bytes: int = 64 * 1024
    max_tokens: int = 4096
    max_morphology_analyses: int = 256
    max_retained_analyses: int = 8

    def __post_init__(self) -> None:
        for name, value in self.__dict__.items():
            if not isinstance(value, int) or value <= 0:
                raise ValueError(f"{name} must be a positive integer")


@dataclass(frozen=True)
class ContextToken:
    surface: str
    start: int
    end: int
    analysis: CanonicalAnalysis


@dataclass(frozen=True)
class RankedAnalysis:
    analysis: CanonicalAnalysis
    score: int


@dataclass(frozen=True)
class AnalyzedToken:
    surface: str
    start: int
    end: int
    context: CanonicalAnalysis
    analyses: tuple[RankedAnalysis, ...]


@runtime_checkable
class ContextAnalyzer(Protocol):
    def analyze_sentence(self, sentence: str) -> Iterable[ContextToken]: ...


@runtime_checkable
class MorphologyAnalyzer(Protocol):
    def analyze_surface(self, surface: str) -> Iterable[CanonicalAnalysis]: ...


@runtime_checkable
class AnalysisFuser(Protocol):
    def rank(
        self,
        token: ContextToken,
        candidates: tuple[CanonicalAnalysis, ...],
    ) -> Iterable[RankedAnalysis]: ...


@runtime_checkable
class LanguageAnalyzer(Protocol):
    @property
    def language(self) -> str: ...

    def analyze_sentence(self, sentence: str, source_offset: int = 0) -> tuple[AnalyzedToken, ...]: ...


def _bounded_tuple(values: Iterable[object], cap: int, stage: str) -> tuple[object, ...]:
    result = []
    for value in values:
        if len(result) == cap:
            raise AnalysisPipelineError(stage, f"result exceeds cap {cap}")
        result.append(value)
    return tuple(result)


class AnalyzerPipeline:
    """Compose contextual, morphology, and fusion providers with hard caps."""

    def __init__(
        self,
        language: str,
        context_analyzer: ContextAnalyzer,
        morphology_analyzer: MorphologyAnalyzer,
        fuser: AnalysisFuser,
        limits: AnalyzerLimits = AnalyzerLimits(),
    ):
        if not language or not language.isascii():
            raise ValueError("language must be non-empty ASCII")
        self._language = language
        self._context_analyzer = context_analyzer
        self._morphology_analyzer = morphology_analyzer
        self._fuser = fuser
        self._limits = limits

    @property
    def language(self) -> str:
        return self._language

    def analyze_sentence(self, sentence: str, source_offset: int = 0) -> tuple[AnalyzedToken, ...]:
        if not isinstance(sentence, str):
            raise AnalysisPipelineError("input", "sentence must be text")
        if source_offset < 0:
            raise AnalysisPipelineError("input", "source offset must be nonnegative")
        sentence_bytes = len(sentence.encode("utf-8"))
        if sentence_bytes > self._limits.max_sentence_bytes:
            raise AnalysisPipelineError(
                "input",
                f"sentence has {sentence_bytes} UTF-8 bytes; cap is {self._limits.max_sentence_bytes}",
            )

        try:
            raw_tokens = _bounded_tuple(
                self._context_analyzer.analyze_sentence(sentence),
                self._limits.max_tokens,
                "context",
            )
        except AnalysisPipelineError:
            raise
        except Exception as error:
            raise AnalysisPipelineError("context", str(error)) from error

        output = []
        previous_end = 0
        for token_index, value in enumerate(raw_tokens):
            if not isinstance(value, ContextToken):
                raise AnalysisPipelineError("context", f"token {token_index} has invalid type")
            token = value
            if token.start < previous_end or token.end <= token.start or token.end > len(sentence):
                raise AnalysisPipelineError("context", f"token {token_index} has invalid offsets")
            if sentence[token.start : token.end] != token.surface:
                raise AnalysisPipelineError("context", f"token {token_index} surface does not match offsets")
            previous_end = token.end

            try:
                candidate_values = _bounded_tuple(
                    self._morphology_analyzer.analyze_surface(token.surface),
                    self._limits.max_morphology_analyses,
                    "morphology",
                )
            except AnalysisPipelineError:
                raise
            except Exception as error:
                raise AnalysisPipelineError("morphology", str(error)) from error
            if any(not isinstance(candidate, CanonicalAnalysis) for candidate in candidate_values):
                raise AnalysisPipelineError(
                    "morphology",
                    f"token {token_index} has an invalid candidate type",
                )
            candidates = tuple(candidate_values)

            try:
                ranked_values = _bounded_tuple(
                    self._fuser.rank(token, candidates),
                    self._limits.max_retained_analyses,
                    "fusion",
                )
            except AnalysisPipelineError:
                raise
            except Exception as error:
                raise AnalysisPipelineError("fusion", str(error)) from error
            if any(not isinstance(item, RankedAnalysis) for item in ranked_values):
                raise AnalysisPipelineError("fusion", f"token {token_index} has an invalid ranked type")
            ranked = tuple(ranked_values)
            if any(item.analysis not in candidates for item in ranked):
                raise AnalysisPipelineError("fusion", f"token {token_index} introduced a new analysis")
            if len({item.analysis for item in ranked}) != len(ranked):
                raise AnalysisPipelineError("fusion", f"token {token_index} contains duplicate analyses")

            output.append(
                AnalyzedToken(
                    surface=token.surface,
                    start=source_offset + token.start,
                    end=source_offset + token.end,
                    context=token.analysis,
                    analyses=ranked,
                )
            )
        return tuple(output)

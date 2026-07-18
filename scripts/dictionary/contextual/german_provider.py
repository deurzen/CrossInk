"""Composition shell for the German contextual analyzer."""

from __future__ import annotations

from .pipeline import (
    AnalysisFuser,
    AnalyzedToken,
    AnalyzerLimits,
    AnalyzerPipeline,
    ContextAnalyzer,
    MorphologyAnalyzer,
)


class GermanLanguageAnalyzer:
    """Bind German providers without adding German logic to the generic pipeline."""

    LANGUAGE = "de"

    def __init__(
        self,
        context_analyzer: ContextAnalyzer,
        morphology_analyzer: MorphologyAnalyzer,
        fuser: AnalysisFuser,
        limits: AnalyzerLimits = AnalyzerLimits(),
    ):
        self._pipeline = AnalyzerPipeline(
            language=self.LANGUAGE,
            context_analyzer=context_analyzer,
            morphology_analyzer=morphology_analyzer,
            fuser=fuser,
            limits=limits,
        )

    @property
    def language(self) -> str:
        return self._pipeline.language

    def analyze_sentence(self, sentence: str, source_offset: int = 0) -> tuple[AnalyzedToken, ...]:
        return self._pipeline.analyze_sentence(sentence, source_offset)

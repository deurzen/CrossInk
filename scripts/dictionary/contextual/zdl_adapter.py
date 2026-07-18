"""Bounded adapter from the pinned ZDL spaCy model to contextual tokens."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
import importlib.metadata
import json
from pathlib import Path
import unicodedata
from typing import Any, Iterable

from .analysis_policy import map_zdl_analysis
from .pipeline import ContextToken


class ZdlAdapterError(RuntimeError):
    pass


@dataclass(frozen=True)
class ZdlLimits:
    max_sentence_bytes: int = 64 * 1024
    max_tokens: int = 4096
    max_token_bytes: int = 255
    max_lemma_bytes: int = 96
    max_tag_bytes: int = 32
    max_morph_features: int = 32
    max_morph_value_bytes: int = 64

    def __post_init__(self) -> None:
        for name, value in self.__dict__.items():
            if not isinstance(value, int) or value <= 0:
                raise ValueError(f"{name} must be a positive integer")


class ZdlContextAnalyzer:
    """Run one NFC sentence through an injected spaCy-compatible pipeline."""

    def __init__(self, nlp: Any, limits: ZdlLimits = ZdlLimits()):
        if not callable(nlp):
            raise ValueError("nlp must be callable")
        self._nlp = nlp
        self._limits = limits

    @classmethod
    def from_pinned_model(
        cls,
        limits: ZdlLimits = ZdlLimits(),
        manifest_path: Path | None = None,
    ) -> "ZdlContextAnalyzer":
        path = manifest_path or Path(__file__).with_name("zdl-model.json")
        try:
            manifest = json.loads(path.read_text(encoding="utf-8"))
            model = manifest["model"]
            expected_spacy = manifest["spacyVersion"]
            actual_spacy = importlib.metadata.version("spacy")
            actual_model = importlib.metadata.version(model["distribution"])
        except Exception as error:
            raise ZdlAdapterError(f"cannot read pinned ZDL environment: {error}") from error
        if actual_spacy != expected_spacy:
            raise ZdlAdapterError(
                f"spaCy {expected_spacy} required, found {actual_spacy}"
            )
        if actual_model != model["version"]:
            raise ZdlAdapterError(
                f"{model['distribution']} {model['version']} required, found {actual_model}"
            )

        try:
            import spacy

            nlp = spacy.load(model["module"], exclude=manifest["excludedComponents"])
        except Exception as error:
            raise ZdlAdapterError(f"cannot load pinned ZDL model: {error}") from error
        if nlp.pipe_names != manifest["enabledComponents"]:
            raise ZdlAdapterError(
                f"unexpected enabled pipeline: {nlp.pipe_names}; "
                f"expected {manifest['enabledComponents']}"
            )
        return cls(nlp, limits)

    def analyze_sentence(self, sentence: str) -> Iterable[ContextToken]:
        if not isinstance(sentence, str):
            raise ZdlAdapterError("sentence must be text")
        if unicodedata.normalize("NFC", sentence) != sentence:
            raise ZdlAdapterError("sentence must be NFC before contextual analysis")
        sentence_bytes = len(sentence.encode("utf-8"))
        if sentence_bytes > self._limits.max_sentence_bytes:
            raise ZdlAdapterError(
                f"sentence has {sentence_bytes} UTF-8 bytes; cap is {self._limits.max_sentence_bytes}"
            )
        try:
            document = self._nlp(sentence)
        except Exception as error:
            raise ZdlAdapterError(f"model inference failed: {error}") from error

        output = []
        previous_end = 0
        token_count = 0
        try:
            for token in document:
                if token_count == self._limits.max_tokens:
                    raise ZdlAdapterError(f"sentence exceeds token cap {self._limits.max_tokens}")
                token_index = token_count
                token_count += 1
                surface, start, end = self._token_span(token, token_index, sentence)
                if start < previous_end:
                    raise ZdlAdapterError(f"token {token_index} overlaps its predecessor")
                previous_end = end
                if bool(getattr(token, "is_space", False)):
                    continue
                analysis = self._token_analysis(token, token_index)
                output.append(ContextToken(surface, start, end, analysis))
        except ZdlAdapterError:
            raise
        except Exception as error:
            raise ZdlAdapterError(f"cannot iterate model output: {error}") from error
        return tuple(output)

    def _token_span(self, token: Any, index: int, sentence: str) -> tuple[str, int, int]:
        surface = getattr(token, "text", None)
        start = getattr(token, "idx", None)
        if not isinstance(surface, str) or not surface:
            raise ZdlAdapterError(f"token {index} has invalid text")
        if not isinstance(start, int) or isinstance(start, bool):
            raise ZdlAdapterError(f"token {index} has invalid offset")
        end = start + len(surface)
        if start < 0 or end > len(sentence) or sentence[start:end] != surface:
            raise ZdlAdapterError(f"token {index} does not round-trip to source offsets")
        surface_bytes = len(surface.encode("utf-8"))
        if surface_bytes > self._limits.max_token_bytes:
            raise ZdlAdapterError(
                f"token {index} has {surface_bytes} UTF-8 bytes; cap is {self._limits.max_token_bytes}"
            )
        return surface, start, end

    def _token_analysis(self, token: Any, index: int):
        lemma = getattr(token, "lemma_", None)
        upos = getattr(token, "pos_", None)
        tag = getattr(token, "tag_", None)
        if not isinstance(lemma, str) or not lemma:
            raise ZdlAdapterError(f"token {index} has invalid lemma")
        if len(lemma.encode("utf-8")) > self._limits.max_lemma_bytes:
            raise ZdlAdapterError(f"token {index} lemma exceeds byte cap")
        for name, value in (("universal POS", upos), ("tag", tag)):
            if not isinstance(value, str):
                raise ZdlAdapterError(f"token {index} has invalid {name}")
            if len(value.encode("utf-8")) > self._limits.max_tag_bytes:
                raise ZdlAdapterError(f"token {index} {name} exceeds byte cap")

        morph_object = getattr(token, "morph", None)
        to_dict = getattr(morph_object, "to_dict", None)
        if not callable(to_dict):
            raise ZdlAdapterError(f"token {index} has invalid morphology")
        morph = to_dict()
        if not isinstance(morph, Mapping):
            raise ZdlAdapterError(f"token {index} morphology is not a mapping")
        if len(morph) > self._limits.max_morph_features:
            raise ZdlAdapterError(
                f"token {index} exceeds morphology cap {self._limits.max_morph_features}"
            )
        bounded_morph = {}
        for key, value in morph.items():
            if not isinstance(key, str) or not isinstance(value, str):
                raise ZdlAdapterError(f"token {index} has non-text morphology")
            if len(key.encode("utf-8")) > self._limits.max_tag_bytes:
                raise ZdlAdapterError(f"token {index} morphology key exceeds byte cap")
            if len(value.encode("utf-8")) > self._limits.max_morph_value_bytes:
                raise ZdlAdapterError(f"token {index} morphology value exceeds byte cap")
            bounded_morph[key] = value
        return map_zdl_analysis(lemma, upos, tag, bounded_morph)

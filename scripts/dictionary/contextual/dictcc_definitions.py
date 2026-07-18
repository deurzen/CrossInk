"""Streaming importer for private dict.cc German-English TSV exports."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import html
from pathlib import Path
import re
import unicodedata

from .analysis_policy import CanonicalPos
from .canonical_lexicon import CanonicalLexiconIndex
from .definition_source import (
    CompiledDefinitionSource,
    DefinitionEntryInput,
    DefinitionFieldInput,
    compile_definition_source,
)

MAX_LINE_BYTES = 64 * 1024
MAX_FIELDS_PER_ENTRY = 128
_TRAILING_ANNOTATION = re.compile(r"\s*\[[^\[\]]*\]\s*$")
_BRACE_ANNOTATION = re.compile(r"\s*\{[^{}]*\}")
_ANGLE_ANNOTATION = re.compile(r"\s*<[^<>]*>")
_OPTIONAL_PREFIX = re.compile(r"^\([^()]*\)\s*")


class DictCcError(ValueError):
    pass


@dataclass
class DictCcStats:
    data_lines: int = 0
    malformed_lines: int = 0
    aligned_lines: int = 0
    unaligned_lines: int = 0
    emitted_entries: int = 0

    def as_dict(self) -> dict[str, int]:
        return {
            "dataLines": self.data_lines,
            "malformedLines": self.malformed_lines,
            "alignedLines": self.aligned_lines,
            "unalignedLines": self.unaligned_lines,
            "emittedEntries": self.emitted_entries,
        }


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _headword_variants(raw: str) -> tuple[str, ...]:
    value = unicodedata.normalize("NFC", html.unescape(raw).strip())
    value = _BRACE_ANNOTATION.sub("", value)
    value = _ANGLE_ANNOTATION.sub("", value)
    while True:
        stripped = _TRAILING_ANNOTATION.sub("", value)
        if stripped == value:
            break
        value = stripped
    value = _OPTIONAL_PREFIX.sub("", value).strip()
    variants = []
    for part in value.split(" / "):
        normalized = part.strip(" ,;")
        if normalized and normalized not in variants:
            variants.append(normalized)
    return tuple(variants)


def _pos_candidates(raw: str) -> tuple[CanonicalPos, ...]:
    normalized = raw.strip().lower().replace("adj.", "adj").replace("pred.", "pred")
    labels = set()
    for token in normalized.split():
        labels.add(token.rsplit(":", 1)[-1])
    result = set()
    mapping = {
        "noun": CanonicalPos.NOUN,
        "verb": CanonicalPos.VERB,
        "adj": CanonicalPos.ADJECTIVE,
        "adjj": CanonicalPos.ADJECTIVE,
        "adv": CanonicalPos.ADVERB,
        "prep": CanonicalPos.ADPOSITION,
        "pron": CanonicalPos.PRONOUN,
        "conj": CanonicalPos.CONJUNCTION,
        "prefix": CanonicalPos.OTHER,
        "suffix": CanonicalPos.OTHER,
    }
    for label in labels:
        if label in mapping:
            result.add(mapping[label])
        elif label in ("past-p", "pres-p"):
            result.update((CanonicalPos.ADJECTIVE, CanonicalPos.VERB))
    return tuple(sorted(result, key=int))


def _canonical_targets(
    canonical: CanonicalLexiconIndex,
    by_headword: dict[str, tuple[CanonicalPos, ...]],
    headwords: tuple[str, ...],
    raw_pos: str,
) -> tuple[tuple[str, CanonicalPos], ...]:
    requested = _pos_candidates(raw_pos)
    targets = set()
    for headword in headwords:
        positions = requested
        if not positions:
            available = by_headword.get(headword, ())
            if len(available) == 1:
                positions = available
        for part_of_speech in positions:
            if canonical.resolve(headword, part_of_speech) is not None:
                targets.add((headword, part_of_speech))
    return tuple(sorted(targets, key=lambda item: (item[0].encode("utf-8"), int(item[1]))))


def stream_dictcc_entries(
    path: Path,
    canonical: CanonicalLexiconIndex,
    stats: DictCcStats,
):
    by_headword_lists: dict[str, list[CanonicalPos]] = {}
    for headword, part_of_speech in canonical.by_key:
        by_headword_lists.setdefault(headword, []).append(part_of_speech)
    by_headword = {
        headword: tuple(sorted(values, key=int))
        for headword, values in by_headword_lists.items()
    }
    try:
        with path.open("r", encoding="utf-8", newline="") as source:
            for line_number, line in enumerate(source, 1):
                if line.startswith("#") or not line.strip():
                    continue
                stats.data_lines += 1
                if len(line.encode("utf-8")) > MAX_LINE_BYTES:
                    stats.malformed_lines += 1
                    continue
                columns = line.rstrip("\r\n").split("\t")
                if len(columns) != 4:
                    stats.malformed_lines += 1
                    continue
                german, english, raw_pos, subject = columns
                headwords = _headword_variants(german)
                translation = unicodedata.normalize("NFC", html.unescape(english).strip())
                if not headwords or not translation:
                    stats.malformed_lines += 1
                    continue
                targets = _canonical_targets(canonical, by_headword, headwords, raw_pos)
                if not targets:
                    stats.unaligned_lines += 1
                    continue
                stats.aligned_lines += 1
                fields = [DefinitionFieldInput(1, translation)]
                if raw_pos.strip():
                    fields.append(DefinitionFieldInput(2, raw_pos.strip()))
                if subject.strip():
                    fields.append(DefinitionFieldInput(4, subject.strip()))
                for headword, part_of_speech in targets:
                    stats.emitted_entries += 1
                    yield DefinitionEntryInput(headword, part_of_speech, tuple(fields))
    except (OSError, UnicodeDecodeError) as error:
        raise DictCcError(f"cannot stream dict.cc source: {error}") from error


def compile_dictcc_definition_source(
    path: Path,
    canonical: CanonicalLexiconIndex,
) -> tuple[CompiledDefinitionSource, DictCcStats]:
    try:
        source_sha256 = _file_sha256(path)
        header_lines = []
        with path.open("r", encoding="utf-8") as source:
            for line in source:
                if not line.startswith("#"):
                    break
                header_lines.append(line.rstrip())
    except (OSError, UnicodeDecodeError) as error:
        raise DictCcError(f"cannot read dict.cc header: {error}") from error
    header = "\n".join(header_lines)
    if "Private use is allowed" not in header or "dict.cc" not in header:
        raise DictCcError("dict.cc private-use license header is missing")

    stats = DictCcStats()
    stats_record = stats.as_dict()

    def entries():
        for entry in stream_dictcc_entries(path, canonical, stats):
            yield entry
        stats_record.update(stats.as_dict())

    license_text = header + "\n"
    compiled = compile_definition_source(
        canonical,
        entries(),
        "de",
        "en",
        "dict.cc",
        license_text,
        {
            "source": "dict.cc private DE-EN export",
            "sourceSha256": source_sha256,
            "license": "LicenseRef-dict.cc-private-use",
            "import": stats_record,
        },
        max_fields_per_entry=MAX_FIELDS_PER_ENTRY,
    )
    return compiled, stats

"""Line-by-line Kaikki English-Wiktionary German definition importer."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import unicodedata

from .analysis_policy import CanonicalPos
from .canonical_lexicon import CanonicalLexiconIndex
from .definition_source import (
    CompiledDefinitionSource,
    DefinitionEntryInput,
    DefinitionFieldInput,
    compile_definition_source,
)

MAX_LINE_BYTES = 256 * 1024
MAX_FIELDS_PER_ENTRY = 128


class KaikkiError(ValueError):
    pass


@dataclass
class KaikkiStats:
    records: int = 0
    malformed_records: int = 0
    malformed_senses: int = 0
    aligned_records: int = 0
    unaligned_records: int = 0
    pos_conflict_records: int = 0
    senses: int = 0
    glosses: int = 0
    examples: int = 0
    emitted_entries: int = 0

    def as_dict(self) -> dict[str, int]:
        return {
            "records": self.records,
            "malformedRecords": self.malformed_records,
            "malformedSenses": self.malformed_senses,
            "alignedRecords": self.aligned_records,
            "unalignedRecords": self.unaligned_records,
            "posConflictRecords": self.pos_conflict_records,
            "senses": self.senses,
            "glosses": self.glosses,
            "examples": self.examples,
            "emittedEntries": self.emitted_entries,
        }


_POS_MAP = {
    "noun": CanonicalPos.NOUN,
    "verb": CanonicalPos.VERB,
    "adj": CanonicalPos.ADJECTIVE,
    "adv": CanonicalPos.ADVERB,
    "pron": CanonicalPos.PRONOUN,
    "det": CanonicalPos.DETERMINER,
    "article": CanonicalPos.DETERMINER,
    "prep": CanonicalPos.ADPOSITION,
    "postp": CanonicalPos.ADPOSITION,
    "conj": CanonicalPos.CONJUNCTION,
    "num": CanonicalPos.NUMERAL,
    "particle": CanonicalPos.PARTICLE,
    "intj": CanonicalPos.INTERJECTION,
    "name": CanonicalPos.PROPER_NOUN,
    "phrase": CanonicalPos.PHRASE,
    "proverb": CanonicalPos.PHRASE,
    "prep_phrase": CanonicalPos.PHRASE,
    "contraction": CanonicalPos.ABBREVIATION,
    "prefix": CanonicalPos.OTHER,
    "suffix": CanonicalPos.OTHER,
    "interfix": CanonicalPos.OTHER,
    "character": CanonicalPos.OTHER,
    "symbol": CanonicalPos.OTHER,
    "punct": CanonicalPos.OTHER,
}


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _text(value) -> str | None:
    if not isinstance(value, str) or not value.strip():
        return None
    return unicodedata.normalize("NFC", value.strip())


def _string_list(value) -> tuple[str, ...]:
    if not isinstance(value, list):
        return ()
    result = []
    for item in value:
        normalized = _text(item)
        if normalized is not None and normalized not in result:
            result.append(normalized)
    return tuple(result)


def _entry_fields(record: dict, stats: KaikkiStats) -> tuple[DefinitionFieldInput, ...]:
    fields = set()
    etymology = _text(record.get("etymology_text"))
    if etymology is not None:
        fields.add((5, etymology))
    senses = record.get("senses")
    if not isinstance(senses, list):
        return tuple(DefinitionFieldInput(field_type, text) for field_type, text in sorted(fields))
    for sense in senses:
        if not isinstance(sense, dict):
            stats.malformed_senses += 1
            continue
        stats.senses += 1
        glosses = _string_list(sense.get("glosses"))
        if not glosses:
            glosses = _string_list(sense.get("raw_glosses"))
        for gloss in glosses:
            fields.add((1, gloss))
            stats.glosses += 1
        qualifiers = []
        qualifier = _text(sense.get("qualifier"))
        if qualifier is not None:
            qualifiers.append(qualifier)
        for key in ("tags", "raw_tags", "topics"):
            qualifiers.extend(_string_list(sense.get(key)))
        if qualifiers:
            fields.add((4, ", ".join(sorted(set(qualifiers), key=lambda value: value.encode("utf-8")))))
        examples = sense.get("examples")
        if isinstance(examples, list):
            for example in examples:
                if not isinstance(example, dict):
                    continue
                source_text = _text(example.get("text"))
                translation = _text(example.get("translation") or example.get("english"))
                if source_text is None:
                    continue
                rendered = source_text if translation is None else f"{source_text} — {translation}"
                fields.add((3, rendered))
                stats.examples += 1
    return tuple(
        DefinitionFieldInput(field_type, text)
        for field_type, text in sorted(fields, key=lambda item: (item[0], item[1].encode("utf-8")))
    )


def stream_kaikki_entries(
    path: Path,
    canonical: CanonicalLexiconIndex,
    stats: KaikkiStats,
):
    canonical_headwords = {headword for headword, _ in canonical.by_key}
    try:
        with path.open("rb") as source:
            for line in source:
                stats.records += 1
                if len(line) > MAX_LINE_BYTES:
                    stats.malformed_records += 1
                    continue
                try:
                    record = json.loads(line)
                except (UnicodeDecodeError, json.JSONDecodeError):
                    stats.malformed_records += 1
                    continue
                if not isinstance(record, dict) or record.get("lang_code") != "de":
                    stats.malformed_records += 1
                    continue
                headword = _text(record.get("word"))
                part_of_speech = _POS_MAP.get(record.get("pos"))
                if headword is None or part_of_speech is None:
                    stats.malformed_records += 1
                    continue
                fields = _entry_fields(record, stats)
                if not fields:
                    stats.malformed_records += 1
                    continue
                if canonical.resolve(headword, part_of_speech) is None:
                    stats.unaligned_records += 1
                    if headword in canonical_headwords:
                        stats.pos_conflict_records += 1
                    continue
                stats.aligned_records += 1
                stats.emitted_entries += 1
                yield DefinitionEntryInput(headword, part_of_speech, fields)
    except OSError as error:
        raise KaikkiError(f"cannot stream Kaikki source: {error}") from error


def compile_kaikki_definition_source(
    path: Path,
    canonical: CanonicalLexiconIndex,
) -> tuple[CompiledDefinitionSource, KaikkiStats]:
    try:
        source_sha256 = _file_sha256(path)
    except OSError as error:
        raise KaikkiError(f"cannot hash Kaikki source: {error}") from error
    stats = KaikkiStats()
    stats_record = stats.as_dict()

    def entries():
        for entry in stream_kaikki_entries(path, canonical, stats):
            yield entry
        stats_record.update(stats.as_dict())

    license_text = (
        "SPDX-License-Identifier: CC-BY-SA-4.0\n"
        "Source: https://kaikki.org/dictionary/German/\n\n"
        "Derived from English Wiktionary contributors via Kaikki.org.\n"
    )
    compiled = compile_definition_source(
        canonical,
        entries(),
        "de",
        "en",
        "Kaikki (en-Wiktionary)",
        license_text,
        {
            "source": "English Wiktionary German extraction via Kaikki.org",
            "sourceSha256": source_sha256,
            "sourceUrl": "https://kaikki.org/dictionary/German/",
            "license": "CC-BY-SA-4.0",
            "import": stats_record,
        },
        max_fields_per_entry=MAX_FIELDS_PER_ENTRY,
    )
    return compiled, stats

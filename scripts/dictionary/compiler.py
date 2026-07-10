"""Deterministic compiler for CrossInk native dictionary bundles.

The input is a small, explicit JSON interchange format. Expensive source-format
imports and morphology generation can feed this format without coupling their
licenses or dependencies to firmware tooling.
"""

from __future__ import annotations

import hashlib
import json
import os
import struct
import unicodedata
import uuid
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable
import zlib

PACKAGE_VERSION = 1
META_SIZE = 80
LEXEME_RECORD_SIZE = 24
FORMS_VERSION = 2
FORMS_HEADER_SIZE = 64
FORM_RECORD_SIZE = 20
FORM_ANALYSIS_SIZE = 8

MAX_LEXEMES = 500_000
MAX_HEADWORDS_SIZE = 64 * 1024 * 1024
MAX_ENTRIES_SIZE = 1024 * 1024 * 1024
MAX_HEADWORD_BYTES = 96
MAX_ENTRY_BYTES = 1024 * 1024
MAX_ENTRY_FIELDS = 1024
MAX_FIELD_BYTES = 65_535
MAX_FORMS = 2_000_000
MAX_FORM_ANALYSES = 4_000_000
MAX_FORM_BYTES = 255
MAX_FORM_STRING_POOL = 512 * 1024 * 1024

POS_VALUES = {
    "unknown": 0,
    "noun": 1,
    "verb": 2,
    "adjective": 3,
    "adverb": 4,
    "pronoun": 5,
    "determiner": 6,
    "preposition": 7,
    "conjunction": 8,
    "numeral": 9,
    "particle": 10,
    "interjection": 11,
    "proper-noun": 12,
    "phrase": 13,
    "abbreviation": 14,
    "other": 15,
}

FIELD_TYPES = {
    "definition": 1,
    "part-of-speech": 2,
    "example": 3,
    "usage": 4,
    "etymology": 5,
    "cross-reference": 6,
    "compound-component": 7,
}

LEXEME_FLAGS = {
    "compound": 0x01,
    "generated": 0x02,
}


class CompileError(ValueError):
    """Raised when source data cannot produce a valid bounded package."""


@dataclass
class LexemeInput:
    headword: str
    part_of_speech: int
    flags: int = 0
    fields: set[tuple[int, str]] = field(default_factory=set)
    forms: set[str] = field(default_factory=set)


FrequencyProvider = Callable[[str, str], float]


@dataclass(frozen=True)
class CompiledBundle:
    archive_bytes: bytes
    files: dict[str, bytes]


def _require_object(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise CompileError(f"{name} must be an object")
    return value


def _require_string(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise CompileError(f"{name} must be a non-empty string")
    return value


def _normalize_text(value: Any, name: str) -> str:
    return unicodedata.normalize("NFC", _require_string(value, name).strip())


def _language_bytes(value: Any, name: str) -> bytes:
    language = _require_string(value, name)
    if len(language) > 7 or not language[0].isalpha() or not all(c.isascii() and (c.isalnum() or c == "-") for c in language):
        raise CompileError(f"{name} must be a 1-7 byte ASCII language tag")
    return language.encode("ascii").ljust(8, b"\0")


def _fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def _crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _lexeme_key_hash(headword: bytes, part_of_speech: int) -> int:
    return _fnv1a64(headword + b"\x1f" + bytes((part_of_speech,)))


def _parse_flags(raw_flags: Any, name: str) -> int:
    if raw_flags is None:
        return 0
    if not isinstance(raw_flags, list):
        raise CompileError(f"{name} must be an array")
    flags = 0
    for raw_flag in raw_flags:
        flag = _require_string(raw_flag, f"{name} value")
        if flag not in LEXEME_FLAGS:
            raise CompileError(f"unsupported lexeme flag: {flag}")
        flags |= LEXEME_FLAGS[flag]
    return flags


def _parse_fields(raw_lexeme: dict[str, Any], name: str) -> set[tuple[int, str]]:
    fields: set[tuple[int, str]] = set()
    raw_fields = raw_lexeme.get("fields", [])
    if not isinstance(raw_fields, list):
        raise CompileError(f"{name}.fields must be an array")
    for index, raw_field in enumerate(raw_fields):
        field_object = _require_object(raw_field, f"{name}.fields[{index}]")
        field_name = _require_string(field_object.get("type"), f"{name}.fields[{index}].type")
        if field_name not in FIELD_TYPES:
            raise CompileError(f"unsupported entry field type: {field_name}")
        text = _normalize_text(field_object.get("text"), f"{name}.fields[{index}].text")
        if len(text.encode("utf-8")) > MAX_FIELD_BYTES:
            raise CompileError(f"{name}.fields[{index}] exceeds {MAX_FIELD_BYTES} UTF-8 bytes")
        fields.add((FIELD_TYPES[field_name], text))
    if not fields:
        raise CompileError(f"{name} must contain at least one entry field")
    if len(fields) > MAX_ENTRY_FIELDS:
        raise CompileError(f"{name} exceeds {MAX_ENTRY_FIELDS} entry fields")
    return fields


def _parse_lexemes(source: dict[str, Any]) -> list[LexemeInput]:
    raw_lexemes = source.get("lexemes")
    if not isinstance(raw_lexemes, list) or not raw_lexemes:
        raise CompileError("lexemes must be a non-empty array")

    merged: dict[tuple[bytes, int], LexemeInput] = {}
    for index, raw_value in enumerate(raw_lexemes):
        name = f"lexemes[{index}]"
        raw_lexeme = _require_object(raw_value, name)
        headword = _normalize_text(raw_lexeme.get("headword"), f"{name}.headword")
        headword_bytes = headword.encode("utf-8")
        if len(headword_bytes) > MAX_HEADWORD_BYTES:
            raise CompileError(f"{name}.headword exceeds {MAX_HEADWORD_BYTES} UTF-8 bytes")
        pos_name = _require_string(raw_lexeme.get("partOfSpeech"), f"{name}.partOfSpeech")
        if pos_name not in POS_VALUES:
            raise CompileError(f"unsupported part of speech: {pos_name}")
        part_of_speech = POS_VALUES[pos_name]
        key = (headword_bytes, part_of_speech)
        lexeme = merged.get(key)
        if lexeme is None:
            lexeme = LexemeInput(headword=headword, part_of_speech=part_of_speech)
            merged[key] = lexeme
        lexeme.flags |= _parse_flags(raw_lexeme.get("flags"), f"{name}.flags")
        lexeme.fields.update(_parse_fields(raw_lexeme, name))

        raw_forms = raw_lexeme.get("forms", [])
        if not isinstance(raw_forms, list):
            raise CompileError(f"{name}.forms must be an array")
        lexeme.forms.add(headword)
        for form_index, raw_form in enumerate(raw_forms):
            form = _normalize_text(raw_form, f"{name}.forms[{form_index}]")
            if len(form.encode("utf-8")) > MAX_FORM_BYTES:
                raise CompileError(f"{name}.forms[{form_index}] exceeds {MAX_FORM_BYTES} UTF-8 bytes")
            lexeme.forms.add(form)

    if len(merged) > MAX_LEXEMES:
        raise CompileError(f"dictionary exceeds {MAX_LEXEMES} lexemes")
    return [merged[key] for key in sorted(merged)]


def _encode_entry(lexeme: LexemeInput) -> bytes:
    sorted_fields = sorted(lexeme.fields, key=lambda item: (item[0], item[1].encode("utf-8")))
    output = bytearray(struct.pack("<BBH", 1, 0, len(sorted_fields)))
    for field_type, text in sorted_fields:
        encoded = text.encode("utf-8")
        output.extend(struct.pack("<BBH", field_type, 0, len(encoded)))
        output.extend(encoded)
    if len(output) > MAX_ENTRY_BYTES:
        raise CompileError(f"entry for {lexeme.headword!r} exceeds {MAX_ENTRY_BYTES} bytes")
    return bytes(output)


def _build_device_files(lexemes: list[LexemeInput], bundle_uuid: bytes, source_language: bytes,
                        target_language: bytes) -> dict[str, bytes]:
    headwords = bytearray()
    entries = bytearray()
    records = bytearray()

    for lexeme in lexemes:
        headword = lexeme.headword.encode("utf-8")
        entry = _encode_entry(lexeme)
        if len(headwords) + len(headword) > MAX_HEADWORDS_SIZE:
            raise CompileError(f"headword pool exceeds {MAX_HEADWORDS_SIZE} bytes")
        if len(entries) + len(entry) > MAX_ENTRIES_SIZE:
            raise CompileError(f"entry pool exceeds {MAX_ENTRIES_SIZE} bytes")
        headword_offset = len(headwords)
        entry_offset = len(entries)
        headwords.extend(headword)
        entries.extend(entry)
        records.extend(struct.pack(
            "<IIIQHBB",
            headword_offset,
            entry_offset,
            len(entry),
            _lexeme_key_hash(headword, lexeme.part_of_speech),
            len(headword),
            lexeme.part_of_speech,
            lexeme.flags,
        ))

    meta = bytearray(META_SIZE)
    struct.pack_into("<4sHHI16s8s8sIHHIIIIII", meta, 0,
                     b"CXDM", PACKAGE_VERSION, META_SIZE, 0, bundle_uuid,
                     source_language, target_language, len(lexemes), LEXEME_RECORD_SIZE, 0,
                     len(records), len(headwords), len(entries),
                     _crc32(records), _crc32(headwords), _crc32(entries))
    struct.pack_into("<I", meta, 76, _crc32(meta[:76]))
    return {
        "device/meta.bin": bytes(meta),
        "device/lexemes.bin": bytes(records),
        "device/headwords.bin": bytes(headwords),
        "device/entries.bin": bytes(entries),
    }


def _difficulty_score(surface: str, lexeme_ids: list[int], lemma_frequencies: list[float], source_language: str,
                      frequency_provider: FrequencyProvider | None) -> int:
    if frequency_provider is None:
        return 0
    surface_zipf = frequency_provider(surface, source_language)
    lemma_zipf = max((lemma_frequencies[index] - 0.5 for index in lexeme_ids), default=0.0)
    familiarity = max(0.0, min(8.0, max(surface_zipf, lemma_zipf)))
    return 1 + round((8.0 - familiarity) * 254.0 / 8.0)


def _build_forms(lexemes: list[LexemeInput], bundle_uuid: bytes, source_language: str,
                 frequency_provider: FrequencyProvider | None) -> bytes:
    lemma_frequencies = ([frequency_provider(lexeme.headword, source_language) for lexeme in lexemes]
                         if frequency_provider is not None else [])
    form_to_lexemes: dict[bytes, list[int]] = {}
    for lexeme_id, lexeme in enumerate(lexemes):
        for form in lexeme.forms:
            encoded = form.encode("utf-8")
            form_to_lexemes.setdefault(encoded, []).append(lexeme_id)

    if len(form_to_lexemes) > MAX_FORMS:
        raise CompileError(f"dictionary exceeds {MAX_FORMS} forms")

    ordered_forms = sorted(form_to_lexemes, key=lambda form: (_fnv1a64(form), form))
    analyses = bytearray()
    strings = bytearray()
    directory = bytearray()
    analysis_count = 0
    for form in ordered_forms:
        lexeme_ids = sorted(set(form_to_lexemes[form]))
        if len(lexeme_ids) > 255:
            lexeme_ids = lexeme_ids[:255]
        if len(strings) + len(form) > MAX_FORM_STRING_POOL:
            raise CompileError(f"form string pool exceeds {MAX_FORM_STRING_POOL} bytes")
        if analysis_count + len(lexeme_ids) > MAX_FORM_ANALYSES:
            raise CompileError(f"dictionary exceeds {MAX_FORM_ANALYSES} form analyses")
        string_offset = len(strings)
        strings.extend(form)
        first_analysis = analysis_count
        for lexeme_id in lexeme_ids:
            analyses.extend(struct.pack("<IHH", lexeme_id, 1000, 0))
        analysis_count += len(lexeme_ids)
        difficulty = _difficulty_score(
            form.decode("utf-8"), lexeme_ids, lemma_frequencies, source_language, frequency_provider
        )
        directory.extend(struct.pack("<QIIHBB", _fnv1a64(form), string_offset, first_analysis,
                                     len(form), len(lexeme_ids), difficulty))

    directory_offset = FORMS_HEADER_SIZE
    analysis_offset = directory_offset + len(directory)
    strings_offset = analysis_offset + len(analyses)
    file_size = strings_offset + len(strings)
    header = bytearray(FORMS_HEADER_SIZE)
    struct.pack_into("<4sHH16sIIIIIIII", header, 0,
                     b"CXDF", FORMS_VERSION, FORMS_HEADER_SIZE, bundle_uuid,
                     len(ordered_forms), analysis_count, directory_offset, analysis_offset,
                     strings_offset, len(strings), file_size, 0)
    payload = bytes(directory + analyses + strings)
    struct.pack_into("<I", header, 56, _crc32(payload))
    struct.pack_into("<I", header, 60, _crc32(header[:60]))
    return bytes(header) + payload


def _license_text(source: dict[str, Any]) -> tuple[str, dict[str, str]]:
    license_data = _require_object(source.get("license"), "license")
    spdx = _require_string(license_data.get("spdx"), "license.spdx")
    attribution = _require_string(license_data.get("attribution"), "license.attribution").strip()
    source_url = _require_string(license_data.get("sourceUrl"), "license.sourceUrl")
    text = f"SPDX-License-Identifier: {spdx}\nSource: {source_url}\n\n{attribution}\n"
    return text, {"spdx": spdx, "attribution": attribution, "sourceUrl": source_url}


def _archive(files: dict[str, bytes]) -> bytes:
    import io

    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", allowZip64=True) as archive:
        for path in sorted(files):
            info = zipfile.ZipInfo(path, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, files[path])
    return output.getvalue()


def compile_bundle(source: dict[str, Any], frequency_provider: FrequencyProvider | None = None) -> CompiledBundle:
    source = _require_object(source, "source")
    try:
        bundle_uuid = uuid.UUID(_require_string(source.get("bundleUuid"), "bundleUuid")).bytes
    except ValueError as exc:
        raise CompileError("bundleUuid must be a valid UUID") from exc
    if bundle_uuid == bytes(16):
        raise CompileError("bundleUuid must not be all zero")

    source_language = _language_bytes(source.get("sourceLanguage"), "sourceLanguage")
    target_language = _language_bytes(source.get("targetLanguage"), "targetLanguage")
    license_text, license_manifest = _license_text(source)
    lexemes = _parse_lexemes(source)

    files = _build_device_files(lexemes, bundle_uuid, source_language, target_language)
    files["device/licenses.txt"] = license_text.encode("utf-8")
    source_language_text = source_language.rstrip(b"\0").decode("ascii")
    files["compiler/forms.bin"] = _build_forms(
        lexemes, bundle_uuid, source_language_text, frequency_provider
    )
    frequency_provider_id = getattr(frequency_provider, "provider_id", "custom") if frequency_provider else "none"
    frequency_license = getattr(frequency_provider, "license_text", None) if frequency_provider else None
    if frequency_license:
        files["compiler/frequency-license.txt"] = frequency_license.encode("utf-8")

    manifest = {
        "formatVersion": 1,
        "bundleUuid": str(uuid.UUID(bytes=bundle_uuid)),
        "sourceLanguage": source_language_text,
        "targetLanguage": target_language.rstrip(b"\0").decode("ascii"),
        "lexemeCount": len(lexemes),
        "frequencyRanking": frequency_provider_id,
        "license": license_manifest,
        "files": {
            path: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            for path, data in sorted(files.items())
        },
    }
    files["manifest.json"] = (json.dumps(manifest, ensure_ascii=False, sort_keys=True,
                                          separators=(",", ":")) + "\n").encode("utf-8")
    return CompiledBundle(archive_bytes=_archive(files), files=files)


def compile_file(source_path: Path, output_path: Path, frequency_provider: FrequencyProvider | None = None) -> None:
    try:
        source = json.loads(source_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CompileError(f"cannot read dictionary source: {exc}") from exc

    bundle = compile_bundle(source, frequency_provider)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_name(output_path.name + ".tmp")
    try:
        with temporary_path.open("wb") as output:
            output.write(bundle.archive_bytes)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, output_path)
    finally:
        try:
            temporary_path.unlink()
        except FileNotFoundError:
            pass

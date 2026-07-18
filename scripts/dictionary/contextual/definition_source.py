"""Deterministic compiler and validator for canonical definition sources."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import io
import json
from pathlib import Path
import struct
import unicodedata
import uuid
import zipfile
import zlib
from typing import Iterable

from .analysis_policy import CanonicalPos
from .canonical_lexicon import CanonicalLexiconIndex

FORMAT_VERSION = 1
META_SIZE = 144
INDEX_RECORD_SIZE = 8
ENTRY_VERSION = 1
MAX_ENTRIES_SIZE = 1024 * 1024 * 1024
MAX_ENTRY_BYTES = 1024 * 1024
MAX_FIELDS = 1024
MAX_FIELD_BYTES = 65_535
MAX_UNMATCHED_EXAMPLES = 100
SOURCE_NAMESPACE = uuid.UUID("e2372afd-1852-542c-876d-11477b36920b")
VALID_FIELD_TYPES = frozenset(range(1, 8))


class DefinitionSourceError(ValueError):
    pass


@dataclass(frozen=True)
class DefinitionFieldInput:
    field_type: int
    text: str


@dataclass(frozen=True)
class DefinitionEntryInput:
    headword: str
    part_of_speech: CanonicalPos
    fields: tuple[DefinitionFieldInput, ...]


@dataclass(frozen=True)
class CompiledDefinitionSource:
    archive_bytes: bytes
    files: dict[str, bytes]
    source_uuid: uuid.UUID
    coverage_count: int
    unmatched_count: int


def _crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _canonical_json(value) -> bytes:
    return (
        json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
        + "\n"
    ).encode("utf-8")


def _language(value: str) -> bytes:
    if (
        not isinstance(value, str)
        or not 1 <= len(value) <= 7
        or not value[0].isalpha()
        or any(not character.isascii() or not (character.isalnum() or character == "-") for character in value)
    ):
        raise DefinitionSourceError("language must be a 1-7 byte ASCII tag")
    return value.encode("ascii").ljust(8, b"\0")


def _source_label(value: str) -> bytes:
    if not isinstance(value, str) or not value.strip():
        raise DefinitionSourceError("source label must be non-empty")
    normalized = unicodedata.normalize("NFC", value.strip())
    encoded = normalized.encode("utf-8")
    if len(encoded) > 31:
        raise DefinitionSourceError("source label exceeds 31 UTF-8 bytes")
    return encoded.ljust(32, b"\0")


def _prepare_field(field: DefinitionFieldInput, entry_index: int) -> tuple[int, bytes]:
    if not isinstance(field, DefinitionFieldInput) or field.field_type not in VALID_FIELD_TYPES:
        raise DefinitionSourceError(f"entry {entry_index} has invalid field type")
    if not isinstance(field.text, str) or not field.text.strip():
        raise DefinitionSourceError(f"entry {entry_index} has empty field text")
    text = unicodedata.normalize("NFC", field.text.strip())
    encoded = text.encode("utf-8")
    if len(encoded) > MAX_FIELD_BYTES:
        raise DefinitionSourceError(
            f"entry {entry_index} field exceeds {MAX_FIELD_BYTES} UTF-8 bytes"
        )
    return field.field_type, encoded


def _encode_fields(fields: set[tuple[int, bytes]], canonical_id: int) -> bytes:
    if not fields:
        raise DefinitionSourceError(f"canonical entry {canonical_id} has no fields")
    if len(fields) > MAX_FIELDS:
        raise DefinitionSourceError(
            f"canonical entry {canonical_id} exceeds {MAX_FIELDS} fields"
        )
    ordered = sorted(fields, key=lambda item: (item[0], item[1]))
    output = bytearray(struct.pack("<BBH", ENTRY_VERSION, 0, len(ordered)))
    for field_type, text in ordered:
        output.extend(struct.pack("<BBH", field_type, 0, len(text)))
        output.extend(text)
    if len(output) > MAX_ENTRY_BYTES:
        raise DefinitionSourceError(
            f"canonical entry {canonical_id} exceeds {MAX_ENTRY_BYTES} bytes"
        )
    return bytes(output)


def _archive(files: dict[str, bytes]) -> bytes:
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", allowZip64=True) as archive:
        for path in sorted(files):
            info = zipfile.ZipInfo(path, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, files[path])
    return output.getvalue()


def compile_definition_source(
    canonical: CanonicalLexiconIndex,
    entries: Iterable[DefinitionEntryInput],
    source_language: str,
    target_language: str,
    source_label: str,
    license_text: str,
    provenance: dict,
) -> CompiledDefinitionSource:
    source_language_bytes = _language(source_language)
    target_language_bytes = _language(target_language)
    label_bytes = _source_label(source_label)
    if not isinstance(license_text, str) or not license_text.strip():
        raise DefinitionSourceError("license text must be non-empty")
    if not isinstance(provenance, dict):
        raise DefinitionSourceError("provenance must be an object")

    fields_by_id: dict[int, set[tuple[int, bytes]]] = {}
    unmatched = []
    unmatched_count = 0
    for entry_index, entry in enumerate(entries):
        if not isinstance(entry, DefinitionEntryInput):
            raise DefinitionSourceError(f"entry {entry_index} has invalid type")
        if not isinstance(entry.headword, str) or not entry.headword.strip():
            raise DefinitionSourceError(f"entry {entry_index} has invalid headword")
        if not isinstance(entry.part_of_speech, CanonicalPos):
            raise DefinitionSourceError(f"entry {entry_index} has invalid part of speech")
        headword = unicodedata.normalize("NFC", entry.headword.strip())
        canonical_id = canonical.resolve(headword, entry.part_of_speech)
        prepared_fields = {
            _prepare_field(field, entry_index)
            for field in entry.fields
        }
        if not prepared_fields:
            raise DefinitionSourceError(f"entry {entry_index} has no fields")
        if canonical_id is None:
            unmatched_count += 1
            if len(unmatched) < MAX_UNMATCHED_EXAMPLES:
                unmatched.append(
                    {
                        "headword": headword,
                        "partOfSpeech": int(entry.part_of_speech),
                    }
                )
            continue
        fields_by_id.setdefault(canonical_id, set()).update(prepared_fields)
        if len(fields_by_id[canonical_id]) > MAX_FIELDS:
            raise DefinitionSourceError(
                f"canonical entry {canonical_id} exceeds {MAX_FIELDS} fields"
            )

    index = bytearray(canonical.lexeme_count * INDEX_RECORD_SIZE)
    payload = bytearray()
    for canonical_id in sorted(fields_by_id):
        entry = _encode_fields(fields_by_id[canonical_id], canonical_id)
        if len(payload) + len(entry) > MAX_ENTRIES_SIZE:
            raise DefinitionSourceError(
                f"definition payload exceeds {MAX_ENTRIES_SIZE} bytes"
            )
        struct.pack_into("<II", index, canonical_id * INDEX_RECORD_SIZE, len(payload), len(entry))
        payload.extend(entry)

    fingerprint = hashlib.sha256(index + payload).digest()
    source_uuid = uuid.uuid5(
        SOURCE_NAMESPACE,
        f"{canonical.canonical_uuid.hex}:{fingerprint.hex()}",
    )
    meta = bytearray(META_SIZE)
    struct.pack_into(
        "<4sHHI16s16s8s8s32sIHHIIIII20s",
        meta,
        0,
        b"CXDS",
        FORMAT_VERSION,
        META_SIZE,
        0,
        source_uuid.bytes,
        canonical.canonical_uuid.bytes,
        source_language_bytes,
        target_language_bytes,
        label_bytes,
        canonical.lexeme_count,
        INDEX_RECORD_SIZE,
        ENTRY_VERSION,
        len(index),
        len(payload),
        _crc32(index),
        _crc32(payload),
        len(fields_by_id),
        bytes(20),
    )
    struct.pack_into("<I", meta, 140, _crc32(meta[:140]))

    report = {
        "schemaVersion": 1,
        "canonicalUuid": str(canonical.canonical_uuid),
        "canonicalLexemeCount": canonical.lexeme_count,
        "coverageCount": len(fields_by_id),
        "coveragePercent": round(
            len(fields_by_id) * 100.0 / canonical.lexeme_count,
            6,
        ),
        "unmatchedCount": unmatched_count,
        "unmatchedExamples": unmatched,
        "provenance": provenance,
    }
    files = {
        "device/meta.bin": bytes(meta),
        "device/entry-index.bin": bytes(index),
        "device/entries.bin": bytes(payload),
        "device/licenses.txt": license_text.encode("utf-8"),
        "compiler/coverage.json": _canonical_json(report),
    }
    manifest = {
        "formatVersion": FORMAT_VERSION,
        "packageType": "definition-source",
        "sourceUuid": str(source_uuid),
        "canonicalUuid": str(canonical.canonical_uuid),
        "sourceLanguage": source_language,
        "targetLanguage": target_language,
        "sourceLabel": unicodedata.normalize("NFC", source_label.strip()),
        "canonicalLexemeCount": canonical.lexeme_count,
        "coverageCount": len(fields_by_id),
        "payloadSha256": fingerprint.hex(),
        "provenance": provenance,
        "files": {
            path: {
                "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
            for path, data in sorted(files.items())
        },
    }
    files["manifest.json"] = _canonical_json(manifest)
    return CompiledDefinitionSource(
        _archive(files),
        files,
        source_uuid,
        len(fields_by_id),
        unmatched_count,
    )


def validate_definition_files(
    meta: bytes,
    index: bytes,
    entries: bytes,
    expected_canonical_uuid: uuid.UUID,
    expected_lexeme_count: int,
) -> uuid.UUID:
    if len(meta) != META_SIZE or meta[:4] != b"CXDS":
        raise DefinitionSourceError("invalid definition metadata")
    if struct.unpack_from("<HHI", meta, 4) != (FORMAT_VERSION, META_SIZE, 0):
        raise DefinitionSourceError("unsupported definition metadata version")
    if _crc32(meta[:140]) != struct.unpack_from("<I", meta, 140)[0]:
        raise DefinitionSourceError("definition metadata CRC mismatch")
    source_uuid = uuid.UUID(bytes=meta[12:28])
    canonical_uuid = uuid.UUID(bytes=meta[28:44])
    if source_uuid.int == 0 or canonical_uuid != expected_canonical_uuid:
        raise DefinitionSourceError("definition UUID mismatch")
    if meta[120:140] != bytes(20):
        raise DefinitionSourceError("definition reserved bytes are nonzero")
    for offset in (44, 52):
        field = meta[offset : offset + 8]
        value, separator, padding = field.partition(b"\0")
        if not value or (separator and any(padding)):
            raise DefinitionSourceError("definition language field is invalid")
        try:
            decoded = value.decode("ascii")
        except UnicodeDecodeError as error:
            raise DefinitionSourceError("definition language field is invalid") from error
        _language(decoded)
    label = meta[60:92]
    value, separator, padding = label.partition(b"\0")
    if not value or not separator or any(padding):
        raise DefinitionSourceError("definition source label is invalid")
    try:
        decoded_label = value.decode("utf-8")
    except UnicodeDecodeError as error:
        raise DefinitionSourceError("definition source label is invalid") from error
    if unicodedata.normalize("NFC", decoded_label) != decoded_label:
        raise DefinitionSourceError("definition source label is not NFC")
    lexeme_count, record_size, entry_version = struct.unpack_from("<IHH", meta, 92)
    index_size, entries_size, index_crc, entries_crc, coverage = struct.unpack_from(
        "<IIIII", meta, 100
    )
    if lexeme_count != expected_lexeme_count or record_size != 8 or entry_version != 1:
        raise DefinitionSourceError("definition count or record contract mismatch")
    if len(index) > 4_000_000 or len(entries) > MAX_ENTRIES_SIZE:
        raise DefinitionSourceError("definition files exceed format caps")
    if index_size != len(index) or entries_size != len(entries):
        raise DefinitionSourceError("definition file size mismatch")
    if len(index) != lexeme_count * INDEX_RECORD_SIZE:
        raise DefinitionSourceError("definition index count mismatch")
    if _crc32(index) != index_crc or _crc32(entries) != entries_crc:
        raise DefinitionSourceError("definition payload CRC mismatch")
    fingerprint = hashlib.sha256(index + entries).hexdigest()
    expected_source_uuid = uuid.uuid5(
        SOURCE_NAMESPACE,
        f"{canonical_uuid.hex}:{fingerprint}",
    )
    if source_uuid != expected_source_uuid:
        raise DefinitionSourceError("definition source fingerprint UUID mismatch")

    present = 0
    previous_end = 0
    for canonical_id in range(lexeme_count):
        offset, length = struct.unpack_from("<II", index, canonical_id * INDEX_RECORD_SIZE)
        if length == 0:
            if offset != 0:
                raise DefinitionSourceError("invalid missing definition record")
            continue
        if length < 4 or offset < previous_end or offset + length > len(entries):
            raise DefinitionSourceError("definition entry range is invalid")
        entry = entries[offset : offset + length]
        version, flags, field_count = struct.unpack_from("<BBH", entry)
        if version != ENTRY_VERSION or flags != 0 or field_count == 0 or field_count > MAX_FIELDS:
            raise DefinitionSourceError("definition entry header is invalid")
        cursor = 4
        for _ in range(field_count):
            if cursor + 4 > len(entry):
                raise DefinitionSourceError("definition field header is truncated")
            field_type, field_flags, field_length = struct.unpack_from("<BBH", entry, cursor)
            cursor += 4
            if field_type not in VALID_FIELD_TYPES or field_flags != 0 or field_length == 0:
                raise DefinitionSourceError("definition field header is invalid")
            if cursor + field_length > len(entry):
                raise DefinitionSourceError("definition field is truncated")
            try:
                text = entry[cursor : cursor + field_length].decode("utf-8")
            except UnicodeDecodeError as error:
                raise DefinitionSourceError("definition field is not UTF-8") from error
            if unicodedata.normalize("NFC", text) != text:
                raise DefinitionSourceError("definition field is not NFC")
            cursor += field_length
        if cursor != len(entry):
            raise DefinitionSourceError("definition entry has trailing bytes")
        previous_end = offset + length
        present += 1
    if present != coverage:
        raise DefinitionSourceError("definition coverage count mismatch")
    return source_uuid

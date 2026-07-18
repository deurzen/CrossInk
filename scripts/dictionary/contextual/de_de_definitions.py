"""Import the retained de-DE dictionary as a canonical definition source."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct
import unicodedata
import zipfile
import zlib

from .canonical_lexicon import CanonicalLexiconIndex, load_de_de_seed
from .definition_source import (
    DefinitionEntryInput,
    DefinitionFieldInput,
    DefinitionSourceError,
    CompiledDefinitionSource,
    compile_definition_source,
)


class DeDeDefinitionError(DefinitionSourceError):
    pass


def _verified_definition_payload(path: Path) -> tuple[bytes, bytes]:
    try:
        with zipfile.ZipFile(path) as archive:
            manifest = json.loads(archive.read("manifest.json").decode("utf-8"))
            meta = archive.read("device/meta.bin")
            records = archive.read("device/lexemes.bin")
            entries = archive.read("device/entries.bin")
    except (OSError, KeyError, UnicodeDecodeError, zipfile.BadZipFile, ValueError) as error:
        raise DeDeDefinitionError(f"cannot read de-DE definitions: {error}") from error
    record = manifest.get("files", {}).get("device/entries.bin")
    if (
        not isinstance(record, dict)
        or record.get("bytes") != len(entries)
        or record.get("sha256") != hashlib.sha256(entries).hexdigest()
    ):
        raise DeDeDefinitionError("de-DE entries manifest hash mismatch")
    if len(meta) != 80 or struct.unpack_from("<I", meta, 60)[0] != len(entries):
        raise DeDeDefinitionError("de-DE entries size mismatch")
    if struct.unpack_from("<I", meta, 72)[0] != zlib.crc32(entries) & 0xFFFFFFFF:
        raise DeDeDefinitionError("de-DE entries CRC mismatch")
    return records, entries


def _decode_entry(data: bytes, lexeme_id: int) -> tuple[DefinitionFieldInput, ...]:
    if len(data) < 4:
        raise DeDeDefinitionError(f"de-DE entry {lexeme_id} is truncated")
    version, flags, field_count = struct.unpack_from("<BBH", data)
    if version != 1 or flags != 0 or field_count == 0 or field_count > 1024:
        raise DeDeDefinitionError(f"de-DE entry {lexeme_id} has invalid header")
    fields = []
    cursor = 4
    for _ in range(field_count):
        if cursor + 4 > len(data):
            raise DeDeDefinitionError(f"de-DE entry {lexeme_id} field header is truncated")
        field_type, field_flags, length = struct.unpack_from("<BBH", data, cursor)
        cursor += 4
        if field_type not in range(1, 8) or field_flags != 0 or length == 0:
            raise DeDeDefinitionError(f"de-DE entry {lexeme_id} field header is invalid")
        if cursor + length > len(data):
            raise DeDeDefinitionError(f"de-DE entry {lexeme_id} field is truncated")
        try:
            text = data[cursor : cursor + length].decode("utf-8")
        except UnicodeDecodeError as error:
            raise DeDeDefinitionError(f"de-DE entry {lexeme_id} field is not UTF-8") from error
        if unicodedata.normalize("NFC", text) != text:
            raise DeDeDefinitionError(f"de-DE entry {lexeme_id} field is not NFC")
        fields.append(DefinitionFieldInput(field_type, text))
        cursor += length
    if cursor != len(data):
        raise DeDeDefinitionError(f"de-DE entry {lexeme_id} has trailing bytes")
    return tuple(fields)


def compile_de_de_definition_source(
    dictionary_path: Path,
    canonical: CanonicalLexiconIndex,
) -> CompiledDefinitionSource:
    seed = load_de_de_seed(dictionary_path)
    records, entries = _verified_definition_payload(dictionary_path)
    if len(records) != len(seed.lexemes) * 24:
        raise DeDeDefinitionError("de-DE lexeme records do not match seed")

    def source_entries():
        previous_end = 0
        for lexeme_id, lexeme in enumerate(seed.lexemes):
            record_offset = lexeme_id * 24
            entry_offset, entry_length = struct.unpack_from("<II", records, record_offset + 4)
            if (
                entry_length < 4
                or entry_offset < previous_end
                or entry_offset + entry_length > len(entries)
            ):
                raise DeDeDefinitionError(f"de-DE entry {lexeme_id} has invalid range")
            previous_end = entry_offset + entry_length
            yield DefinitionEntryInput(
                lexeme.headword,
                lexeme.part_of_speech,
                _decode_entry(entries[entry_offset : entry_offset + entry_length], lexeme_id),
            )

    provenance = {
        "source": "German Wiktionary via Kaikki.org",
        "sourceArchiveSha256": seed.provenance["archiveSha256"],
        "sourceBundleUuid": seed.provenance["bundleUuid"],
        "sourceLexemeCount": seed.provenance["lexemeCount"],
        "license": seed.provenance["license"],
    }
    return compile_definition_source(
        canonical,
        source_entries(),
        "de",
        "de",
        "Deutsch (Wiktionary)",
        seed.license_text,
        provenance,
    )

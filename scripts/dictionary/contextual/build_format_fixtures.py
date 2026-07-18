#!/usr/bin/env python3
"""Generate deterministic fixtures for contextual dictionary format contracts."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_OUTPUT = ROOT / "test" / "data" / "contextual" / "formats"
CANONICAL_NAMESPACE = uuid.UUID("1b924302-36fe-5d3d-aafb-b3dc11267ef7")
SOURCE_NAMESPACE = uuid.UUID("e2372afd-1852-542c-876d-11477b36920b")


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def align4(data: bytearray) -> None:
    data.extend(b"\0" * ((-len(data)) & 3))


def language(value: str) -> bytes:
    return value.encode("ascii").ljust(8, b"\0")


def build_canonical() -> tuple[dict[str, bytes], uuid.UUID]:
    lexemes = (("Laden", 1), ("Liebe", 1), ("laden", 2))
    headwords = bytearray()
    records = bytearray()
    for headword, pos in lexemes:
        encoded = headword.encode("utf-8")
        offset = len(headwords)
        headwords.extend(encoded)
        key_hash = fnv1a64(encoded + b"\x1f" + bytes((pos,)))
        records.extend(struct.pack("<IQHBB", offset, key_hash, len(encoded), pos, 0))

    payload_fingerprint = hashlib.sha256(records + headwords).digest()
    canonical_uuid = uuid.uuid5(CANONICAL_NAMESPACE, payload_fingerprint.hex())
    meta = bytearray(112)
    struct.pack_into(
        "<4sHHI16s8sIHHIIII32s16s",
        meta,
        0,
        b"CXCL",
        1,
        112,
        0,
        canonical_uuid.bytes,
        language("de"),
        len(lexemes),
        16,
        1,
        len(records),
        len(headwords),
        crc32(records),
        crc32(headwords),
        payload_fingerprint,
        bytes(16),
    )
    struct.pack_into("<I", meta, 108, crc32(meta[:108]))
    return {
        "canonical-meta.bin": bytes(meta),
        "canonical-lexemes.bin": bytes(records),
        "canonical-headwords.bin": bytes(headwords),
    }, canonical_uuid


def encode_entry(*fields: tuple[int, str]) -> bytes:
    entry = bytearray(struct.pack("<BBH", 1, 0, len(fields)))
    for field_type, text in fields:
        payload = text.encode("utf-8")
        entry.extend(struct.pack("<BBH", field_type, 0, len(payload)))
        entry.extend(payload)
    return bytes(entry)


def build_definition(canonical_uuid: uuid.UUID) -> tuple[dict[str, bytes], uuid.UUID]:
    entries = bytearray()
    index = bytearray()
    fixture_entries = {
        0: encode_entry((1, "Geschäft"), (3, "Der Laden ist geöffnet.")),
        2: encode_entry((1, "to load"), (3, "Wir laden die Kisten.")),
    }
    for lexeme_id in range(3):
        entry = fixture_entries.get(lexeme_id)
        if entry is None:
            index.extend(struct.pack("<II", 0, 0))
            continue
        index.extend(struct.pack("<II", len(entries), len(entry)))
        entries.extend(entry)

    source_fingerprint = hashlib.sha256(index + entries).digest()
    source_uuid = uuid.uuid5(
        SOURCE_NAMESPACE,
        f"{canonical_uuid.hex}:{source_fingerprint.hex()}",
    )
    meta = bytearray(144)
    label = b"Fixture de-en".ljust(32, b"\0")
    struct.pack_into(
        "<4sHHI16s16s8s8s32sIHHIIIII20s",
        meta,
        0,
        b"CXDS",
        1,
        144,
        0,
        source_uuid.bytes,
        canonical_uuid.bytes,
        language("de"),
        language("en"),
        label,
        3,
        8,
        1,
        len(index),
        len(entries),
        crc32(index),
        crc32(entries),
        len(fixture_entries),
        bytes(20),
    )
    struct.pack_into("<I", meta, 140, crc32(meta[:140]))
    return {
        "definition-meta.bin": bytes(meta),
        "definition-index.bin": bytes(index),
        "definition-entries.bin": bytes(entries),
    }, source_uuid


def build_attachments(canonical_uuid: uuid.UUID, source_uuid: uuid.UUID) -> bytes:
    record = bytearray(88)
    struct.pack_into(
        "<4sHHI16sIB3s16s16s16s",
        record,
        0,
        b"CXAT",
        1,
        88,
        0,
        canonical_uuid.bytes,
        7,
        1,
        bytes(3),
        source_uuid.bytes,
        bytes(16),
        bytes(16),
    )
    struct.pack_into("<I", record, 84, crc32(record[:84]))
    return bytes(record)


def build_language(canonical_uuid: uuid.UUID) -> bytes:
    surface = b"laden"
    record = bytearray(struct.pack("<QHBBBBH", fnv1a64(surface), 0, len(surface), 2, 0x03, 200, 1000))
    record.extend(struct.pack("<HH", 1, 0))  # primary global 2, alternate global 0
    record.extend(surface)
    align4(record)
    struct.pack_into("<H", record, 8, len(record))

    spine_directory = struct.pack("<II", 0, 1)
    shard_directory = struct.pack("<IHHIII", 0, len(record), 1, 0, 1, 0)
    local_lemmas = struct.pack("<II", 0, 2)
    metadata_json = json.dumps(
        {
            "analysisPolicyVersion": 2,
            "canonicalUuid": str(canonical_uuid),
            "compilerVersion": 1,
            "dwdsmor": {"edition": "fixture-open", "sha256": "0" * 64, "version": "fixture"},
            "frequency": {"provider": "none", "version": "none"},
            "shardTokenCount": 64,
            "spacyVersion": "3.8.14",
            "tokenizerVersion": 1,
            "zdl": {
                "model": "de-zdl-lg",
                "sha256": "9d35263ac80e80e9730ee21830ffdbe96cf256b72c71e30326ae5865456ade9a",
                "version": "4.0.0",
            },
        },
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    metadata = struct.pack("<4sHHII", b"CXLM", 1, 16, len(metadata_json), 0) + metadata_json

    artifact = bytearray(108)
    offsets = []
    for section in (spine_directory, shard_directory, bytes(record), local_lemmas, metadata):
        align4(artifact)
        offsets.append(len(artifact))
        artifact.extend(section)
    struct.pack_into(
        "<4sHHIHH16s8s8sHHIIIIIIIIIIIII",
        artifact,
        0,
        b"CXLG",
        4,
        108,
        0,
        1,
        1,
        canonical_uuid.bytes,
        language("de"),
        language("und"),
        1,
        0,
        1,
        1,
        2,
        0,
        offsets[0],
        offsets[1],
        offsets[2],
        offsets[3],
        offsets[4],
        0,
        0,
        len(artifact),
        0,
    )
    struct.pack_into("<I", artifact, 100, crc32(artifact[108:]))
    struct.pack_into("<I", artifact, 104, crc32(artifact[:104]))
    return bytes(artifact)


def build_files() -> dict[str, bytes]:
    files, canonical_uuid = build_canonical()
    definition_files, source_uuid = build_definition(canonical_uuid)
    files.update(definition_files)
    files["attachments.bin"] = build_attachments(canonical_uuid, source_uuid)
    files["language-v4.bin"] = build_language(canonical_uuid)
    return files


def checksum_manifest(files: dict[str, bytes]) -> bytes:
    manifest = {
        name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        for name, data in sorted(files.items())
    }
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    files = build_files()
    files["checksums.json"] = checksum_manifest(files)
    if args.check:
        mismatches = [
            name
            for name, data in files.items()
            if not (args.output / name).is_file() or (args.output / name).read_bytes() != data
        ]
        if mismatches:
            raise SystemExit(f"contextual format fixtures differ: {', '.join(mismatches)}")
        print(f"verified {len(files)} contextual format fixtures")
        return 0

    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (args.output / name).write_bytes(data)
    print(f"wrote {len(files)} contextual format fixtures to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

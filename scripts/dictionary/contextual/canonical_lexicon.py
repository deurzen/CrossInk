"""Deterministic compiler for canonical German lexical identity bundles."""

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

from .analysis_policy import CANONICAL_POS_VERSION, POLICY_VERSION, CanonicalPos

FORMAT_VERSION = 1
COMPILER_VERSION = 1
META_SIZE = 112
LEXEME_RECORD_SIZE = 16
MAX_LEXEMES = 500_000
MAX_HEADWORDS_SIZE = 64 * 1024 * 1024
MAX_HEADWORD_BYTES = 96
LEXEME_FLAGS_MASK = 0x03
CANONICAL_NAMESPACE = uuid.UUID("1b924302-36fe-5d3d-aafb-b3dc11267ef7")

CONTEXTUAL_DIR = Path(__file__).resolve().parent
DEFAULT_DWDSMOR_MANIFEST = CONTEXTUAL_DIR / "dwdsmor-open.json"
DEFAULT_ZDL_MANIFEST = CONTEXTUAL_DIR / "zdl-model.json"


class CanonicalLexiconError(ValueError):
    pass


@dataclass(frozen=True)
class CanonicalLexemeInput:
    headword: str
    part_of_speech: CanonicalPos
    flags: int = 0


@dataclass(frozen=True)
class DeDeLexiconSeed:
    lexemes: tuple[CanonicalLexemeInput, ...]
    provenance: dict
    license_text: str


@dataclass(frozen=True)
class CompiledCanonicalBundle:
    archive_bytes: bytes
    files: dict[str, bytes]
    canonical_uuid: uuid.UUID
    payload_sha256: str
    lexeme_count: int


@dataclass(frozen=True)
class _PreparedLexeme:
    headword: str
    encoded: bytes
    part_of_speech: CanonicalPos
    flags: int


def _crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise CanonicalLexiconError(f"cannot hash de-DE seed: {error}") from error
    return digest.hexdigest()


def _canonical_json(value) -> bytes:
    return (
        json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
        + "\n"
    ).encode("utf-8")


def _verify_manifest_file(manifest: dict, name: str, data: bytes) -> None:
    files = manifest.get("files")
    record = files.get(name) if isinstance(files, dict) else None
    if not isinstance(record, dict):
        raise CanonicalLexiconError(f"de-DE manifest omits {name}")
    if record.get("bytes") != len(data):
        raise CanonicalLexiconError(f"de-DE size mismatch for {name}")
    if record.get("sha256") != hashlib.sha256(data).hexdigest():
        raise CanonicalLexiconError(f"de-DE hash mismatch for {name}")


def load_de_de_seed(path: Path) -> DeDeLexiconSeed:
    """Load only lexical identity files; forms and definitions stay unopened."""
    required = (
        "device/headwords.bin",
        "device/lexemes.bin",
        "device/licenses.txt",
        "device/meta.bin",
    )
    archive_sha256 = _file_sha256(path)
    try:
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            if len(names) != len(set(names)):
                raise CanonicalLexiconError("de-DE bundle contains duplicate paths")
            manifest = json.loads(archive.read("manifest.json").decode("utf-8"))
            files = {name: archive.read(name) for name in required}
    except CanonicalLexiconError:
        raise
    except (OSError, KeyError, UnicodeDecodeError, json.JSONDecodeError, zipfile.BadZipFile) as error:
        raise CanonicalLexiconError(f"cannot read de-DE seed: {error}") from error

    if not isinstance(manifest, dict) or manifest.get("formatVersion") != 1:
        raise CanonicalLexiconError("unsupported de-DE manifest")
    if manifest.get("sourceLanguage") != "de" or manifest.get("targetLanguage") != "de":
        raise CanonicalLexiconError("canonical seed must be a de-DE bundle")
    for name, data in files.items():
        _verify_manifest_file(manifest, name, data)

    meta = files["device/meta.bin"]
    records = files["device/lexemes.bin"]
    headwords = files["device/headwords.bin"]
    if len(meta) != 80 or meta[:4] != b"CXDM":
        raise CanonicalLexiconError("invalid de-DE metadata")
    if struct.unpack_from("<HHI", meta, 4) != (1, 80, 0):
        raise CanonicalLexiconError("unsupported de-DE metadata version")
    if _crc32(meta[:76]) != struct.unpack_from("<I", meta, 76)[0]:
        raise CanonicalLexiconError("de-DE metadata CRC mismatch")
    bundle_uuid = uuid.UUID(bytes=meta[12:28])
    if bundle_uuid.int == 0 or str(bundle_uuid) != manifest.get("bundleUuid"):
        raise CanonicalLexiconError("de-DE bundle UUID mismatch")
    source_language = meta[28:36].rstrip(b"\0")
    target_language = meta[36:44].rstrip(b"\0")
    if source_language != b"de" or target_language != b"de":
        raise CanonicalLexiconError("de-DE metadata has wrong language direction")
    lexeme_count, record_size = struct.unpack_from("<IH", meta, 44)
    records_size, headwords_size = struct.unpack_from("<II", meta, 52)
    records_crc, headwords_crc = struct.unpack_from("<II", meta, 64)
    if record_size != 24 or lexeme_count != manifest.get("lexemeCount"):
        raise CanonicalLexiconError("de-DE lexeme metadata mismatch")
    if records_size != len(records) or headwords_size != len(headwords):
        raise CanonicalLexiconError("de-DE lexical file size mismatch")
    if len(records) != lexeme_count * record_size:
        raise CanonicalLexiconError("de-DE lexeme count exceeds record file")
    if _crc32(records) != records_crc or _crc32(headwords) != headwords_crc:
        raise CanonicalLexiconError("de-DE lexical payload CRC mismatch")

    lexemes = []
    previous_key = None
    for lexeme_id in range(lexeme_count):
        offset = lexeme_id * record_size
        headword_offset = struct.unpack_from("<I", records, offset)[0]
        key_hash = struct.unpack_from("<Q", records, offset + 12)[0]
        headword_length, raw_pos, flags = struct.unpack_from("<HBB", records, offset + 20)
        if (
            headword_length == 0
            or headword_length > MAX_HEADWORD_BYTES
            or headword_offset + headword_length > len(headwords)
        ):
            raise CanonicalLexiconError(f"de-DE lexeme {lexeme_id} has invalid headword range")
        encoded = headwords[headword_offset : headword_offset + headword_length]
        try:
            headword = encoded.decode("utf-8")
            part_of_speech = CanonicalPos(raw_pos)
        except (UnicodeDecodeError, ValueError) as error:
            raise CanonicalLexiconError(f"de-DE lexeme {lexeme_id} has invalid identity") from error
        if unicodedata.normalize("NFC", headword) != headword:
            raise CanonicalLexiconError(f"de-DE lexeme {lexeme_id} headword is not NFC")
        if flags & ~LEXEME_FLAGS_MASK:
            raise CanonicalLexiconError(f"de-DE lexeme {lexeme_id} has unsupported flags")
        expected_hash = _fnv1a64(encoded + b"\x1f" + bytes((raw_pos,)))
        if key_hash != expected_hash:
            raise CanonicalLexiconError(f"de-DE lexeme {lexeme_id} key hash mismatch")
        key = (encoded, raw_pos)
        if previous_key is not None and key <= previous_key:
            raise CanonicalLexiconError("de-DE lexemes are not strictly sorted")
        previous_key = key
        lexemes.append(CanonicalLexemeInput(headword, part_of_speech, flags))

    try:
        license_text = files["device/licenses.txt"].decode("utf-8")
    except UnicodeDecodeError as error:
        raise CanonicalLexiconError("de-DE license text is not UTF-8") from error
    license_record = manifest.get("license")
    if not isinstance(license_record, dict) or not isinstance(license_record.get("spdx"), str):
        raise CanonicalLexiconError("de-DE manifest has no SPDX license")
    provenance = {
        "archiveSha256": archive_sha256,
        "bundleUuid": str(bundle_uuid),
        "lexemeCount": lexeme_count,
        "license": license_record,
    }
    return DeDeLexiconSeed(tuple(lexemes), provenance, license_text)


def _prepare_lexemes(values: Iterable[CanonicalLexemeInput]) -> list[_PreparedLexeme]:
    prepared = []
    for index, value in enumerate(values):
        if not isinstance(value, CanonicalLexemeInput):
            raise CanonicalLexiconError(f"lexeme {index} has invalid type")
        if not isinstance(value.headword, str) or not value.headword.strip():
            raise CanonicalLexiconError(f"lexeme {index} has invalid headword")
        headword = unicodedata.normalize("NFC", value.headword.strip())
        encoded = headword.encode("utf-8")
        if len(encoded) > MAX_HEADWORD_BYTES:
            raise CanonicalLexiconError(
                f"lexeme {index} headword exceeds {MAX_HEADWORD_BYTES} UTF-8 bytes"
            )
        if not isinstance(value.part_of_speech, CanonicalPos):
            raise CanonicalLexiconError(f"lexeme {index} has invalid part of speech")
        if not isinstance(value.flags, int) or value.flags & ~LEXEME_FLAGS_MASK:
            raise CanonicalLexiconError(f"lexeme {index} has invalid flags")
        prepared.append(_PreparedLexeme(headword, encoded, value.part_of_speech, value.flags))
        if len(prepared) > MAX_LEXEMES:
            raise CanonicalLexiconError(f"canonical lexicon exceeds {MAX_LEXEMES} inputs")
    if not prepared:
        raise CanonicalLexiconError("canonical lexicon has no lexemes")
    prepared.sort(key=lambda item: (item.encoded, int(item.part_of_speech)))

    merged = []
    for item in prepared:
        if (
            merged
            and merged[-1].encoded == item.encoded
            and merged[-1].part_of_speech == item.part_of_speech
        ):
            previous = merged[-1]
            merged[-1] = _PreparedLexeme(
                previous.headword,
                previous.encoded,
                previous.part_of_speech,
                previous.flags | item.flags,
            )
        else:
            merged.append(item)
    return merged


def _load_json_manifest(path: Path, label: str) -> tuple[dict, bytes]:
    try:
        data = path.read_bytes()
        manifest = json.loads(data.decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CanonicalLexiconError(f"cannot read {label} manifest: {error}") from error
    if not isinstance(manifest, dict) or manifest.get("schemaVersion") != 1:
        raise CanonicalLexiconError(f"unsupported {label} manifest")
    return manifest, _canonical_json(manifest)


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


def compile_canonical_bundle(
    lexemes: Iterable[CanonicalLexemeInput],
    seed_provenance: dict,
    runtime_license_text: str,
    dwdsmor_manifest_path: Path = DEFAULT_DWDSMOR_MANIFEST,
    zdl_manifest_path: Path = DEFAULT_ZDL_MANIFEST,
) -> CompiledCanonicalBundle:
    prepared = _prepare_lexemes(lexemes)
    dwdsmor, dwdsmor_json = _load_json_manifest(dwdsmor_manifest_path, "DWDSmor")
    zdl, zdl_json = _load_json_manifest(zdl_manifest_path, "ZDL")
    if dwdsmor.get("edition") != "open" or dwdsmor.get("license") != "GPL-2.0-only":
        raise CanonicalLexiconError("DWDSmor manifest is not the pinned Open Edition")
    zdl_model = zdl.get("model")
    zdl_license = zdl.get("license")
    if not isinstance(zdl_model, dict) or zdl_model.get("distribution") != "de-zdl-lg":
        raise CanonicalLexiconError("ZDL manifest is not the pinned static model")
    if not isinstance(zdl_license, dict) or zdl_license.get("modelSpdx") != "NOASSERTION":
        raise CanonicalLexiconError("ZDL manifest does not record model license status")
    if not isinstance(seed_provenance, dict):
        raise CanonicalLexiconError("seed provenance must be an object")
    if not isinstance(runtime_license_text, str) or not runtime_license_text.strip():
        raise CanonicalLexiconError("runtime license text must be non-empty")

    records = bytearray()
    headwords = bytearray()
    for item in prepared:
        if len(headwords) + len(item.encoded) > MAX_HEADWORDS_SIZE:
            raise CanonicalLexiconError(f"headword pool exceeds {MAX_HEADWORDS_SIZE} bytes")
        headword_offset = len(headwords)
        headwords.extend(item.encoded)
        key_hash = _fnv1a64(
            item.encoded + b"\x1f" + bytes((int(item.part_of_speech),))
        )
        records.extend(
            struct.pack(
                "<IQHBB",
                headword_offset,
                key_hash,
                len(item.encoded),
                int(item.part_of_speech),
                item.flags,
            )
        )

    fingerprint = hashlib.sha256(records + headwords).digest()
    canonical_uuid = uuid.uuid5(CANONICAL_NAMESPACE, fingerprint.hex())
    meta = bytearray(META_SIZE)
    struct.pack_into(
        "<4sHHI16s8sIHHIIII32s16s",
        meta,
        0,
        b"CXCL",
        FORMAT_VERSION,
        META_SIZE,
        0,
        canonical_uuid.bytes,
        b"de".ljust(8, b"\0"),
        len(prepared),
        LEXEME_RECORD_SIZE,
        CANONICAL_POS_VERSION,
        len(records),
        len(headwords),
        _crc32(records),
        _crc32(headwords),
        fingerprint,
        bytes(16),
    )
    struct.pack_into("<I", meta, 108, _crc32(meta[:108]))

    analyzer_manifest = {
        "schemaVersion": 1,
        "compilerVersion": COMPILER_VERSION,
        "canonicalPosVersion": CANONICAL_POS_VERSION,
        "analysisPolicyVersion": POLICY_VERSION,
        "sourceLanguage": "de",
        "seed": seed_provenance,
        "dwdsmorManifestSha256": hashlib.sha256(dwdsmor_json).hexdigest(),
        "zdlManifestSha256": hashlib.sha256(zdl_json).hexdigest(),
    }
    compiler_licenses = (
        "DWDSmor Open Edition: GPL-2.0-only\n"
        "ZDL spaCy model code: GPL-3.0-or-later; training datasets have separate terms.\n"
        "Model wheels are external compiler assets and are not included in this archive.\n"
    ).encode("utf-8")
    files = {
        "runtime/meta.bin": bytes(meta),
        "runtime/lexemes.bin": bytes(records),
        "runtime/headwords.bin": bytes(headwords),
        "runtime/licenses.txt": runtime_license_text.encode("utf-8"),
        "compiler/analyzer.json": _canonical_json(analyzer_manifest),
        "compiler/dwdsmor-open.json": dwdsmor_json,
        "compiler/zdl-model.json": zdl_json,
        "compiler/licenses.txt": compiler_licenses,
    }
    manifest = {
        "formatVersion": FORMAT_VERSION,
        "packageType": "canonical-lexicon",
        "canonicalUuid": str(canonical_uuid),
        "sourceLanguage": "de",
        "lexemeCount": len(prepared),
        "canonicalPosVersion": CANONICAL_POS_VERSION,
        "analysisPolicyVersion": POLICY_VERSION,
        "payloadSha256": fingerprint.hex(),
        "seed": seed_provenance,
        "files": {
            path: {
                "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
            for path, data in sorted(files.items())
        },
    }
    files["manifest.json"] = _canonical_json(manifest)
    return CompiledCanonicalBundle(
        archive_bytes=_archive(files),
        files=files,
        canonical_uuid=canonical_uuid,
        payload_sha256=fingerprint.hex(),
        lexeme_count=len(prepared),
    )


def compile_de_de_canonical_bundle(
    seed_path: Path,
    dwdsmor_manifest_path: Path = DEFAULT_DWDSMOR_MANIFEST,
    zdl_manifest_path: Path = DEFAULT_ZDL_MANIFEST,
) -> CompiledCanonicalBundle:
    seed = load_de_de_seed(seed_path)
    return compile_canonical_bundle(
        seed.lexemes,
        seed.provenance,
        seed.license_text,
        dwdsmor_manifest_path,
        zdl_manifest_path,
    )


@dataclass(frozen=True)
class CanonicalLexiconIndex:
    canonical_uuid: uuid.UUID
    lexeme_count: int
    by_key: dict[tuple[str, CanonicalPos], int]
    analyzer_metadata: dict
    dwdsmor_manifest: dict
    zdl_manifest: dict

    def resolve(self, headword: str, part_of_speech: CanonicalPos) -> int | None:
        return self.by_key.get((unicodedata.normalize("NFC", headword.strip()), part_of_speech))


def load_canonical_lexicon_index(path: Path) -> CanonicalLexiconIndex:
    required = (
        "compiler/analyzer.json",
        "compiler/dwdsmor-open.json",
        "compiler/zdl-model.json",
        "runtime/headwords.bin",
        "runtime/lexemes.bin",
        "runtime/meta.bin",
    )
    try:
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            if len(names) != len(set(names)):
                raise CanonicalLexiconError("canonical bundle contains duplicate paths")
            manifest = json.loads(archive.read("manifest.json").decode("utf-8"))
            files = {name: archive.read(name) for name in required}
    except CanonicalLexiconError:
        raise
    except (OSError, KeyError, UnicodeDecodeError, json.JSONDecodeError, zipfile.BadZipFile) as error:
        raise CanonicalLexiconError(f"cannot read canonical bundle: {error}") from error
    if not isinstance(manifest, dict) or manifest.get("packageType") != "canonical-lexicon":
        raise CanonicalLexiconError("unsupported canonical bundle manifest")
    for name, data in files.items():
        record = manifest.get("files", {}).get(name)
        if (
            not isinstance(record, dict)
            or record.get("bytes") != len(data)
            or record.get("sha256") != hashlib.sha256(data).hexdigest()
        ):
            raise CanonicalLexiconError(f"canonical bundle hash mismatch for {name}")

    meta = files["runtime/meta.bin"]
    records = files["runtime/lexemes.bin"]
    headwords = files["runtime/headwords.bin"]
    if len(meta) != META_SIZE or meta[:4] != b"CXCL":
        raise CanonicalLexiconError("invalid canonical metadata")
    if struct.unpack_from("<HHI", meta, 4) != (FORMAT_VERSION, META_SIZE, 0):
        raise CanonicalLexiconError("unsupported canonical metadata version")
    if _crc32(meta[:108]) != struct.unpack_from("<I", meta, 108)[0]:
        raise CanonicalLexiconError("canonical metadata CRC mismatch")
    canonical_uuid = uuid.UUID(bytes=meta[12:28])
    lexeme_count, record_size, pos_version = struct.unpack_from("<IHH", meta, 36)
    records_size, headwords_size, records_crc, headwords_crc = struct.unpack_from("<IIII", meta, 44)
    if canonical_uuid.int == 0 or str(canonical_uuid) != manifest.get("canonicalUuid"):
        raise CanonicalLexiconError("canonical UUID mismatch")
    if record_size != LEXEME_RECORD_SIZE or pos_version != CANONICAL_POS_VERSION:
        raise CanonicalLexiconError("canonical record contract mismatch")
    if lexeme_count != manifest.get("lexemeCount") or len(records) != lexeme_count * record_size:
        raise CanonicalLexiconError("canonical lexeme count mismatch")
    if records_size != len(records) or headwords_size != len(headwords):
        raise CanonicalLexiconError("canonical lexical file size mismatch")
    if _crc32(records) != records_crc or _crc32(headwords) != headwords_crc:
        raise CanonicalLexiconError("canonical lexical payload CRC mismatch")
    fingerprint = hashlib.sha256(records + headwords).digest()
    if meta[60:92] != fingerprint or manifest.get("payloadSha256") != fingerprint.hex():
        raise CanonicalLexiconError("canonical payload fingerprint mismatch")

    by_key = {}
    previous_key = None
    for lexeme_id in range(lexeme_count):
        offset = lexeme_id * record_size
        headword_offset, key_hash, length, raw_pos, flags = struct.unpack_from("<IQHBB", records, offset)
        if length == 0 or headword_offset + length > len(headwords) or flags & ~LEXEME_FLAGS_MASK:
            raise CanonicalLexiconError(f"canonical lexeme {lexeme_id} is malformed")
        encoded = headwords[headword_offset : headword_offset + length]
        try:
            headword = encoded.decode("utf-8")
            part_of_speech = CanonicalPos(raw_pos)
        except (UnicodeDecodeError, ValueError) as error:
            raise CanonicalLexiconError(f"canonical lexeme {lexeme_id} has invalid identity") from error
        key = (encoded, raw_pos)
        if previous_key is not None and key <= previous_key:
            raise CanonicalLexiconError("canonical lexemes are not strictly sorted")
        previous_key = key
        if unicodedata.normalize("NFC", headword) != headword:
            raise CanonicalLexiconError(f"canonical lexeme {lexeme_id} is not NFC")
        if key_hash != _fnv1a64(encoded + b"\x1f" + bytes((raw_pos,))):
            raise CanonicalLexiconError(f"canonical lexeme {lexeme_id} key hash mismatch")
        by_key[(headword, part_of_speech)] = lexeme_id

    def decode_compiler_json(name: str) -> dict:
        try:
            value = json.loads(files[name].decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise CanonicalLexiconError(f"canonical compiler metadata {name} is invalid") from error
        if not isinstance(value, dict):
            raise CanonicalLexiconError(f"canonical compiler metadata {name} is not an object")
        return value

    return CanonicalLexiconIndex(
        canonical_uuid,
        lexeme_count,
        by_key,
        decode_compiler_json("compiler/analyzer.json"),
        decode_compiler_json("compiler/dwdsmor-open.json"),
        decode_compiler_json("compiler/zdl-model.json"),
    )

"""Shared bounded parsing primitives for the contextual host compiler."""

from __future__ import annotations

from dataclasses import dataclass
import html
import json
import re
import struct
import unicodedata
import zlib

def _fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value

LANGUAGE_HEADER_SIZE = 108
SHARD_TOKEN_COUNT = 64
TOKENIZER_VERSION = 1
ANALYZER_VERSION = 1

MAX_SPINES = 4096
MAX_SHARDS = 65535
MAX_RECORDS = 1_000_000
MAX_LOCAL_LEMMAS = 32768
MAX_SHARD_BLOB_BYTES = 24 * 1024


GERMAN_STOPWORDS = frozenset(
    "aber als am an auch auf aus bei bin bis bist da dadurch daher darum das dass dein deine dem den der des die "
    "dies diese doch dort du durch ein eine einem einen einer eines er es für gegen hat hatte haben hier ich im in "
    "ist ja jede jedem jeden jeder jedes jener jenes jetzt kann kein keine mit muss nach nicht nichts noch nun nur ob "
    "oder ohne sehr sein seine selbst sich sie sind so über um und uns unser unter vom von vor war waren warst was "
    "weg weil weiter welche welchem welcher welches wenn werde werden wie wieder will wir wird wo zu zum zur".split()
)

BLOCK_TAGS = frozenset(
    "address article aside blockquote br dd div dl dt figcaption figure footer h1 h2 h3 h4 h5 h6 header hr li main "
    "nav ol p pre section table tbody td tfoot th thead tr ul".split()
)
HIDDEN_TAGS = frozenset(("script", "style"))
TAG_PATTERN = re.compile(r"<!--[\s\S]*?-->|<!\[CDATA\[[\s\S]*?\]\]>|<[^>]*>")
TAG_NAME_PATTERN = re.compile(r"<\s*(/?)\s*([A-Za-z0-9:_-]+)")
ENTITY_PATTERN = re.compile(r"&(?:#[xX][0-9A-Fa-f]+|#[0-9]+|[A-Za-z][A-Za-z0-9]+);")
LETTER = r"[^\W\d_]"
MARK = r"[\u0300-\u036f]"
TOKEN_PATTERN = re.compile(rf"{LETTER}(?:{LETTER}|{MARK})*(?:[-'’‑](?:{LETTER}|{MARK})+)*", re.UNICODE)


class BookCompileError(ValueError):
    pass


@dataclass(frozen=True)
class FormAnalysis:
    lexeme_ids: tuple[int, ...]
    confidence: int
    difficulty: int


@dataclass(frozen=True)
class CompilerDictionary:
    bundle_uuid: bytes
    source_language: str
    target_language: str
    lexeme_count: int
    forms: dict[str, FormAnalysis]
    folded_forms: dict[str, FormAnalysis]


@dataclass(frozen=True)
class Token:
    surface: str
    raw_offset: int
    text_offset: int


@dataclass(frozen=True)
class VisibleText:
    text: str
    raw_offsets: tuple[int, ...]


def _crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _align4(data: bytearray) -> None:
    data.extend(b"\0" * ((-len(data)) & 3))


def _language_field(language: str) -> bytes:
    encoded = language.encode("ascii")
    if not encoded or len(encoded) > 7:
        raise BookCompileError("language tags must contain 1-7 ASCII bytes")
    return encoded.ljust(8, b"\0")


def load_compiler_dictionary(meta: bytes, forms_data: bytes) -> CompilerDictionary:
    if len(meta) != 80 or meta[:4] != b"CXDM":
        raise BookCompileError("invalid dictionary metadata")
    if struct.unpack_from("<HH", meta, 4) != (1, 80) or _crc32(meta[:76]) != struct.unpack_from("<I", meta, 76)[0]:
        raise BookCompileError("unsupported or corrupt dictionary metadata")
    bundle_uuid = meta[12:28]
    source_language = meta[28:36].split(b"\0", 1)[0].decode("ascii")
    target_language = meta[36:44].split(b"\0", 1)[0].decode("ascii")
    lexeme_count = struct.unpack_from("<I", meta, 44)[0]
    if bundle_uuid == bytes(16) or lexeme_count == 0 or struct.unpack_from("<I", meta, 8)[0] != 0:
        raise BookCompileError("invalid dictionary identity or lexeme count")

    if len(forms_data) < 64 or forms_data[:4] != b"CXDF":
        raise BookCompileError("invalid forms table")
    (version, header_size) = struct.unpack_from("<HH", forms_data, 4)
    if version != 2 or header_size != 64 or forms_data[8:24] != bundle_uuid:
        raise BookCompileError("forms table is incompatible with dictionary metadata")
    (form_count, analysis_count, directory_offset, analysis_offset, strings_offset, strings_size,
     file_size, reserved, payload_crc, header_crc) = struct.unpack_from("<IIIIIIIIII", forms_data, 24)
    if reserved != 0 or file_size != len(forms_data) or _crc32(forms_data[:60]) != header_crc:
        raise BookCompileError("corrupt forms header")
    if _crc32(forms_data[64:]) != payload_crc:
        raise BookCompileError("corrupt forms payload")
    if directory_offset != 64 or analysis_offset != directory_offset + form_count * 20:
        raise BookCompileError("invalid forms table offsets")
    if strings_offset != analysis_offset + analysis_count * 8 or strings_offset + strings_size != file_size:
        raise BookCompileError("invalid forms string pool")

    forms: dict[str, FormAnalysis] = {}
    previous_key: tuple[int, bytes] | None = None
    for index in range(form_count):
        record_offset = directory_offset + index * 20
        surface_hash, string_offset, first_analysis, string_length, count, flags = struct.unpack_from(
            "<QIIHBB", forms_data, record_offset
        )
        difficulty = flags
        if count == 0 or string_offset + string_length > strings_size:
            raise BookCompileError("invalid form record")
        if first_analysis + count > analysis_count:
            raise BookCompileError("form analysis range is out of bounds")
        encoded = forms_data[strings_offset + string_offset:strings_offset + string_offset + string_length]
        if _fnv1a64(encoded) != surface_hash:
            raise BookCompileError("form hash mismatch")
        key = (surface_hash, encoded)
        if previous_key is not None and key <= previous_key:
            raise BookCompileError("forms are not strictly sorted")
        previous_key = key
        try:
            surface = encoded.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise BookCompileError("form is not valid UTF-8") from exc
        lexeme_ids = []
        confidence = 0
        for analysis_index in range(first_analysis, first_analysis + count):
            lexeme_id, item_confidence, analysis_flags = struct.unpack_from(
                "<IHH", forms_data, analysis_offset + analysis_index * 8
            )
            if analysis_flags != 0 or item_confidence > 1000 or lexeme_id >= lexeme_count:
                raise BookCompileError("invalid form analysis")
            lexeme_ids.append(lexeme_id)
            confidence = max(confidence, item_confidence)
        forms[surface] = FormAnalysis(tuple(lexeme_ids), confidence, difficulty)

    folded_ids: dict[str, set[int]] = {}
    folded_confidence: dict[str, int] = {}
    folded_difficulty: dict[str, int] = {}
    for surface, analysis in forms.items():
        folded = surface.casefold()
        folded_ids.setdefault(folded, set()).update(analysis.lexeme_ids)
        folded_confidence[folded] = max(folded_confidence.get(folded, 0), analysis.confidence)
        previous_difficulty = folded_difficulty.get(folded, 0)
        folded_difficulty[folded] = (analysis.difficulty if previous_difficulty == 0 else
                                     min(previous_difficulty, analysis.difficulty or previous_difficulty))
    folded_forms = {
        surface: FormAnalysis(tuple(sorted(lexeme_ids)), folded_confidence[surface], folded_difficulty[surface])
        for surface, lexeme_ids in folded_ids.items()
    }
    return CompilerDictionary(bundle_uuid, source_language, target_language, lexeme_count, forms, folded_forms)


def _append_visible_text(raw: str, raw_start: int, flattened: list[str], raw_offsets: list[int]) -> None:
    cursor = 0
    for entity_match in ENTITY_PATTERN.finditer(raw):
        prefix = raw[cursor:entity_match.start()]
        flattened.extend(prefix)
        raw_offsets.extend(range(raw_start + cursor, raw_start + entity_match.start()))
        decoded = html.unescape(entity_match.group(0))
        flattened.extend(decoded)
        raw_offsets.extend([raw_start + entity_match.start()] * len(decoded))
        cursor = entity_match.end()
    suffix = raw[cursor:]
    flattened.extend(suffix)
    raw_offsets.extend(range(raw_start + cursor, raw_start + len(raw)))


def extract_visible_text(xhtml: str) -> VisibleText:
    flattened: list[str] = []
    raw_offsets: list[int] = []
    hidden_depth = 0
    body_depth = 0
    has_body = re.search(r"<\s*(?:[A-Za-z0-9_-]+:)?body(?:\s|>)", xhtml, re.IGNORECASE) is not None
    cursor = 0
    for tag_match in TAG_PATTERN.finditer(xhtml):
        if hidden_depth == 0 and (not has_body or body_depth > 0):
            _append_visible_text(xhtml[cursor:tag_match.start()], cursor, flattened, raw_offsets)
        tag = tag_match.group(0)
        name_match = TAG_NAME_PATTERN.match(tag)
        if name_match:
            closing = bool(name_match.group(1))
            tag_name = name_match.group(2).split(":")[-1].lower()
            self_closing = tag.rstrip().endswith("/>")
            if tag_name == "body":
                if closing:
                    body_depth = max(0, body_depth - 1)
                elif not self_closing:
                    body_depth += 1
            if tag_name in HIDDEN_TAGS:
                if closing:
                    hidden_depth = max(0, hidden_depth - 1)
                elif not self_closing:
                    hidden_depth += 1
            if hidden_depth == 0 and (not has_body or body_depth > 0) and tag_name in BLOCK_TAGS:
                flattened.append("\n")
                raw_offsets.append(tag_match.end())
        cursor = tag_match.end()
    if hidden_depth == 0 and (not has_body or body_depth > 0):
        _append_visible_text(xhtml[cursor:], cursor, flattened, raw_offsets)

    return VisibleText("".join(flattened), tuple(raw_offsets))


def tokenize_xhtml(xhtml: str) -> list[Token]:
    visible = extract_visible_text(xhtml)
    text = visible.text
    tokens = []
    for match in TOKEN_PATTERN.finditer(text):
        surface = unicodedata.normalize("NFC", match.group(0))
        encoded = surface.encode("utf-8")
        if encoded and len(encoded) <= 255:
            tokens.append(Token(surface, visible.raw_offsets[match.start()], match.start()))
    return tokens



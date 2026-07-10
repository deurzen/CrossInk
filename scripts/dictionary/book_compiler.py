"""Pure host-side compiler for dictionary-aware XHTML spine documents."""

from __future__ import annotations

from dataclasses import dataclass
import html
import json
import re
import struct
import unicodedata
import zlib

from .compiler import _fnv1a64

LANGUAGE_HEADER_SIZE = 108
SHARD_TOKEN_COUNT = 64
TOKENIZER_VERSION = 1
ANALYZER_VERSION = 1

MAX_SPINES = 4096
MAX_SHARDS = 65535
MAX_RECORDS = 1_000_000
MAX_LOCAL_LEMMAS = 32768
MAX_SHARD_BLOB_BYTES = 24 * 1024
LANGUAGE_FORMAT_VERSION = 3

CANDIDATE_AMBIGUOUS = 0x01
CANDIDATE_NORMALIZED_FALLBACK = 0x04
CANDIDATE_ANALYSES_TRUNCATED = 0x08
MAX_INLINE_ANALYSES = 8

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


@dataclass(frozen=True)
class Candidate:
    surface: str
    global_lexeme_ids: tuple[int, ...]
    confidence: int
    flags: int
    difficulty: int


@dataclass(frozen=True)
class CompiledBook:
    xhtml_spines: tuple[str, ...]
    language_artifact: bytes


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


def tokenize_xhtml(xhtml: str) -> list[Token]:
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
                flattened.append(" ")
                raw_offsets.append(tag_match.end())
        cursor = tag_match.end()
    if hidden_depth == 0 and (not has_body or body_depth > 0):
        _append_visible_text(xhtml[cursor:], cursor, flattened, raw_offsets)

    text = "".join(flattened)
    tokens = []
    for match in TOKEN_PATTERN.finditer(text):
        surface = unicodedata.normalize("NFC", match.group(0))
        encoded = surface.encode("utf-8")
        if encoded and len(encoded) <= 255:
            tokens.append(Token(surface, raw_offsets[match.start()]))
    return tokens


def _insert_markers(xhtml: str, tokens: list[Token], first_shard: int) -> str:
    insertions = []
    for token_index in range(0, len(tokens), SHARD_TOKEN_COUNT):
        shard_id = first_shard + token_index // SHARD_TOKEN_COUNT
        marker = f'<span data-crossink-lang-shard="{shard_id}"></span>'
        insertions.append((tokens[token_index].raw_offset, marker))
    output = xhtml
    for offset, marker in reversed(insertions):
        output = output[:offset] + marker + output[offset:]
    return output


def _analyze(surface: str, dictionary: CompilerDictionary) -> Candidate | None:
    if surface.casefold() in GERMAN_STOPWORDS:
        return None

    exact = dictionary.forms.get(surface)
    folded = dictionary.folded_forms.get(surface.casefold())
    flags = 0
    if exact is not None:
        # Keep exact-case analyses first, but do not let capitalization hide a
        # credible folded analysis (especially a sentence-initial German verb).
        lexeme_ids = exact.lexeme_ids
        if folded is not None:
            folded_only = tuple(lexeme_id for lexeme_id in folded.lexeme_ids if lexeme_id not in lexeme_ids)
            if folded_only:
                lexeme_ids += folded_only
                flags |= CANDIDATE_NORMALIZED_FALLBACK
        confidence = exact.confidence
        difficulty = exact.difficulty
    elif folded is not None:
        lexeme_ids = folded.lexeme_ids
        confidence = min(folded.confidence, 900)
        difficulty = folded.difficulty
        flags |= CANDIDATE_NORMALIZED_FALLBACK
    else:
        return None

    if len(lexeme_ids) > 1:
        flags |= CANDIDATE_AMBIGUOUS
    if len(lexeme_ids) > MAX_INLINE_ANALYSES:
        flags |= CANDIDATE_ANALYSES_TRUNCATED
    return Candidate(surface, lexeme_ids[:MAX_INLINE_ANALYSES], confidence, flags, difficulty)


def compile_book(xhtml_spines: list[str], dictionary: CompilerDictionary) -> CompiledBook:
    if not xhtml_spines or len(xhtml_spines) > MAX_SPINES:
        raise BookCompileError(f"book must contain 1-{MAX_SPINES} XHTML spines")

    spine_tokens = [tokenize_xhtml(xhtml) for xhtml in xhtml_spines]
    transformed = []
    spine_ranges = []
    shard_candidates: list[list[Candidate]] = []
    source_token_base = 0
    for xhtml, tokens in zip(xhtml_spines, spine_tokens):
        first_shard = len(shard_candidates)
        shard_count = (len(tokens) + SHARD_TOKEN_COUNT - 1) // SHARD_TOKEN_COUNT
        transformed.append(_insert_markers(xhtml, tokens, first_shard))
        for shard_index in range(shard_count):
            start = shard_index * SHARD_TOKEN_COUNT
            end = min(start + SHARD_TOKEN_COUNT, len(tokens))
            by_surface: dict[str, Candidate] = {}
            for token in tokens[start:end]:
                candidate = _analyze(token.surface, dictionary)
                if candidate is not None:
                    by_surface.setdefault(candidate.surface, candidate)
            ordered_surfaces = sorted(
                by_surface,
                key=lambda value: (_fnv1a64(value.encode("utf-8")), value.encode("utf-8")),
            )
            shard_candidates.append([by_surface[key] for key in ordered_surfaces])
        spine_ranges.append((first_shard, shard_count, source_token_base, len(tokens)))
        source_token_base += len(tokens)

    if len(shard_candidates) > MAX_SHARDS:
        raise BookCompileError(f"book exceeds {MAX_SHARDS} source shards")
    record_count = sum(len(items) for items in shard_candidates)
    if record_count > MAX_RECORDS:
        raise BookCompileError(f"book exceeds {MAX_RECORDS} shard candidates")

    actionable_shards = shard_candidates
    record_count = sum(len(items) for items in actionable_shards)
    global_ids = {global_id for candidates in actionable_shards for candidate in candidates
                  for global_id in candidate.global_lexeme_ids}
    if len(global_ids) > MAX_LOCAL_LEMMAS:
        raise BookCompileError(f"book exceeds {MAX_LOCAL_LEMMAS} local lemmas")

    ordered_global_ids = sorted(global_ids)
    local_by_global = {global_id: local_id for local_id, global_id in enumerate(ordered_global_ids)}
    spine_directory = b"".join(struct.pack("<II", first_shard, shard_count)
                               for first_shard, shard_count, _, _ in spine_ranges)
    shard_directory = bytearray()
    shard_blobs = bytearray()
    for spine_index, (_, shard_count, source_start, token_count) in enumerate(spine_ranges):
        spine_first_shard = spine_ranges[spine_index][0]
        for local_shard in range(shard_count):
            candidates = actionable_shards[spine_first_shard + local_shard]
            blob = bytearray()
            for candidate in candidates:
                encoded = candidate.surface.encode("utf-8")
                local_ids = [local_by_global[item] for item in candidate.global_lexeme_ids]
                if not 1 <= len(local_ids) <= 8:
                    raise BookCompileError("surface analysis count exceeds version-2 limit")
                record_start = len(blob)
                blob.extend(struct.pack("<QHBBBBH", _fnv1a64(encoded), 0, len(encoded), len(local_ids),
                                        candidate.flags, candidate.difficulty, candidate.confidence))
                blob.extend(struct.pack(f"<{len(local_ids)}H", *local_ids))
                blob.extend(encoded)
                _align4(blob)
                struct.pack_into("<H", blob, record_start + 8, len(blob) - record_start)
            if len(blob) > MAX_SHARD_BLOB_BYTES:
                raise BookCompileError(f"shard blob exceeds {MAX_SHARD_BLOB_BYTES} bytes")
            token_start = source_start + local_shard * SHARD_TOKEN_COUNT
            token_end = min(source_start + token_count, token_start + SHARD_TOKEN_COUNT)
            shard_directory.extend(struct.pack("<IHHIII", len(shard_blobs), len(blob), len(candidates),
                                               token_start, token_end, 0))
            shard_blobs.extend(blob)

    local_lemmas = b"".join(struct.pack("<I", global_id) for global_id in ordered_global_ids)
    metadata_json = json.dumps(
        {"analyzer": "crossink-exact-forms-de", "analyzerVersion": ANALYZER_VERSION,
         "ranking": "wordfreq" if any(item.difficulty for shard in actionable_shards for item in shard) else "none",
         "languageFormatVersion": LANGUAGE_FORMAT_VERSION, "shardTokenCount": SHARD_TOKEN_COUNT,
         "tokenizerVersion": TOKENIZER_VERSION}, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    metadata = struct.pack("<4sHHII", b"CXLM", 1, 16, len(metadata_json), 0) + metadata_json

    artifact = bytearray(LANGUAGE_HEADER_SIZE)
    sections = []
    for section in (spine_directory, bytes(shard_directory), bytes(shard_blobs), local_lemmas, metadata):
        _align4(artifact)
        sections.append(len(artifact))
        artifact.extend(section)
    file_size = len(artifact)
    if file_size > 64 * 1024 * 1024:
        raise BookCompileError("language artifact exceeds 64 MiB")

    struct.pack_into("<4sHHIHH16s8s8sHHIIIIIIIIIIIII", artifact, 0,
                     b"CXLG", LANGUAGE_FORMAT_VERSION, LANGUAGE_HEADER_SIZE, 0, TOKENIZER_VERSION, ANALYZER_VERSION,
                     dictionary.bundle_uuid, _language_field(dictionary.source_language),
                     _language_field(dictionary.target_language), len(xhtml_spines), 0,
                     len(shard_candidates), record_count, len(ordered_global_ids), 0,
                     sections[0], sections[1], sections[2], sections[3], sections[4], 0, 0, file_size, 0)
    struct.pack_into("<I", artifact, 100, _crc32(artifact[LANGUAGE_HEADER_SIZE:]))
    struct.pack_into("<I", artifact, 104, _crc32(artifact[:104]))
    return CompiledBook(tuple(transformed), bytes(artifact))

"""Contextual XHTML and full-EPUB compiler for language.bin version 4."""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import struct
import tempfile
import unicodedata
from urllib.parse import unquote, urlsplit
import xml.etree.ElementTree as ET
import zipfile
import zlib
from typing import Callable

from dictionary.contextual.compiler_support import (
    GERMAN_STOPWORDS,
    LANGUAGE_HEADER_SIZE,
    MAX_LOCAL_LEMMAS,
    MAX_RECORDS,
    MAX_SHARDS,
    MAX_SHARD_BLOB_BYTES,
    SHARD_TOKEN_COUNT,
    TOKEN_PATTERN,
    Token,
    _fnv1a64,
    extract_visible_text,
    tokenize_xhtml,
)

from .analysis_policy import CanonicalPos
from .canonical_lexicon import CanonicalLexiconIndex
from .fusion import normalize_score
from .pipeline import AnalysisProvenance, AnalyzedToken, LanguageAnalyzer

FORMAT_VERSION = 4
TOKENIZER_VERSION = 1
ANALYZER_VERSION = 1
COMPILER_VERSION = 1
MAX_SPINES = 4096
MAX_SENTENCE_BYTES = 64 * 1024
MAX_LANGUAGE_BYTES = 64 * 1024 * 1024
LANGUAGE_PATH = "META-INF/crossink/language.bin"

FLAG_AMBIGUOUS = 0x01
FLAG_CONTEXTUAL = 0x02
FLAG_FOLDED = 0x04
FLAG_TRUNCATED = 0x08
FLAG_FALLBACK = 0x10
FLAG_PROPER_NOUN = 0x20

FrequencyProvider = Callable[[str, str], float]


class ContextualEpubError(ValueError):
    pass


@dataclass(frozen=True)
class CompiledContextualBook:
    xhtml_spines: tuple[str, ...]
    language_artifact: bytes
    missing_canonical_analyses: int


@dataclass
class _SurfaceEvidence:
    scores: dict[int, int]
    provenance: dict[int, AnalysisProvenance]
    difficulty: int = 0
    context_proper_noun: bool = False


@dataclass(frozen=True)
class _EncodedCandidate:
    surface: str
    global_ids: tuple[int, ...]
    confidence: int
    difficulty: int
    flags: int


def _crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _align4(data: bytearray) -> None:
    data.extend(b"\0" * ((-len(data)) & 3))


def _language_field(language: str) -> bytes:
    encoded = language.encode("ascii")
    if not encoded or len(encoded) > 7:
        raise ContextualEpubError("language tag must contain 1-7 ASCII bytes")
    return encoded.ljust(8, b"\0")


def _paragraph_ranges(text: str) -> tuple[tuple[int, int], ...]:
    ranges = []
    cursor = 0
    while cursor < len(text):
        line_end = text.find("\n", cursor)
        if line_end < 0:
            line_end = len(text)
        start = cursor
        while start < line_end and text[start].isspace():
            start += 1
        end = line_end
        while end > start and text[end - 1].isspace():
            end -= 1
        if start < end:
            byte_count = len(text[start:end].encode("utf-8"))
            if byte_count > MAX_SENTENCE_BYTES:
                raise ContextualEpubError(
                    f"visible block has {byte_count} UTF-8 bytes; cap is {MAX_SENTENCE_BYTES}"
                )
            ranges.append((start, end))
        cursor = line_end + 1
    return tuple(ranges)


def _difficulty(
    surface: str,
    primary: AnalyzedToken,
    frequency_provider: FrequencyProvider | None,
) -> int:
    if frequency_provider is None or not primary.analyses:
        return 0
    surface_zipf = frequency_provider(surface, "de")
    lemma_zipf = frequency_provider(primary.analyses[0].analysis.lemma, "de") - 0.5
    familiarity = max(0.0, min(8.0, max(surface_zipf, lemma_zipf)))
    return 1 + round((8.0 - familiarity) * 254.0 / 8.0)


def _analyze_spine(
    xhtml: str,
    analyzer: LanguageAnalyzer,
    canonical: CanonicalLexiconIndex,
    frequency_provider: FrequencyProvider | None,
) -> tuple[list[Token], dict[int, _EncodedCandidate], int]:
    if "data-crossink-lang-shard" in xhtml:
        raise ContextualEpubError("XHTML spine already contains CrossInk language markers")
    visible = extract_visible_text(xhtml)
    visible_is_nfc = unicodedata.normalize("NFC", visible.text) == visible.text
    analyzed_by_start: dict[int, AnalyzedToken] = {}
    analyzed_words = []
    for start, end in _paragraph_ranges(visible.text):
        paragraph = visible.text[start:end]
        normalized = unicodedata.normalize("NFC", paragraph)
        try:
            analyzed = analyzer.analyze_sentence(
                normalized,
                source_offset=start if visible_is_nfc else 0,
            )
        except Exception as error:
            raise ContextualEpubError(f"contextual analysis failed: {error}") from error
        if visible_is_nfc:
            for token in analyzed:
                if token.start in analyzed_by_start:
                    raise ContextualEpubError("contextual analyzer returned duplicate token offsets")
                analyzed_by_start[token.start] = token
        else:
            analyzed_words.extend(
                token for token in analyzed if TOKEN_PATTERN.fullmatch(token.surface)
            )

    words = tokenize_xhtml(xhtml)
    if not visible_is_nfc:
        if len(analyzed_words) != len(words) or any(
            analyzed.surface != word.surface
            for analyzed, word in zip(analyzed_words, words)
        ):
            raise ContextualEpubError("NFC token alignment does not match visible XHTML")
        analyzed_by_start = {
            word.text_offset: analyzed
            for analyzed, word in zip(analyzed_words, words)
        }
    candidates = {}
    missing = 0
    for word_index, word in enumerate(words):
        if word.surface.casefold() in GERMAN_STOPWORDS:
            continue
        analyzed = analyzed_by_start.get(word.text_offset)
        if analyzed is None or analyzed.surface != word.surface:
            continue
        scores: dict[int, int] = {}
        provenance: dict[int, AnalysisProvenance] = {}
        for ranked in analyzed.analyses:
            canonical_id = canonical.resolve(
                ranked.analysis.lemma,
                ranked.analysis.part_of_speech,
            )
            if canonical_id is None:
                missing += 1
                continue
            previous = scores.get(canonical_id)
            if previous is None or ranked.score > previous:
                scores[canonical_id] = ranked.score
            provenance[canonical_id] = (
                provenance.get(canonical_id, AnalysisProvenance(0))
                | ranked.provenance
            )
        if not scores:
            continue
        ordered = sorted(scores, key=lambda canonical_id: (-scores[canonical_id], canonical_id))
        truncated = len(ordered) > 8
        ordered = ordered[:8]
        primary_id = ordered[0]
        flags = FLAG_CONTEXTUAL
        if len(ordered) > 1:
            flags |= FLAG_AMBIGUOUS
        if truncated:
            flags |= FLAG_TRUNCATED
        if any(provenance[item] & AnalysisProvenance.FOLDED_FORM_INVENTORY for item in ordered):
            flags |= FLAG_FOLDED
        if not provenance[primary_id] & AnalysisProvenance.PRIMARY_MORPHOLOGY:
            flags |= FLAG_FALLBACK
        if analyzed.context.part_of_speech == CanonicalPos.PROPER_NOUN:
            flags |= FLAG_PROPER_NOUN
        candidates[word_index] = _EncodedCandidate(
            word.surface,
            tuple(ordered),
            normalize_score(scores[primary_id]),
            _difficulty(word.surface, analyzed, frequency_provider),
            flags,
        )
    return words, candidates, missing


def _merge_surface(
    existing: _SurfaceEvidence | None,
    candidate: _EncodedCandidate,
) -> _SurfaceEvidence:
    evidence = existing or _SurfaceEvidence({}, {})
    evidence.difficulty = max(evidence.difficulty, candidate.difficulty)
    evidence.context_proper_noun |= bool(candidate.flags & FLAG_PROPER_NOUN)
    for canonical_id in candidate.global_ids:
        # Candidate records carry one primary confidence. Preserve ordering for
        # repeated surfaces by assigning descending synthetic tie-break scores.
        score = candidate.confidence * 16 - candidate.global_ids.index(canonical_id)
        evidence.scores[canonical_id] = max(evidence.scores.get(canonical_id, -1), score)
    if candidate.flags & FLAG_FOLDED:
        for canonical_id in candidate.global_ids:
            evidence.provenance[canonical_id] = (
                evidence.provenance.get(canonical_id, AnalysisProvenance(0))
                | AnalysisProvenance.FOLDED_FORM_INVENTORY
            )
    if candidate.flags & FLAG_FALLBACK:
        evidence.provenance[candidate.global_ids[0]] = (
            evidence.provenance.get(candidate.global_ids[0], AnalysisProvenance(0))
            | AnalysisProvenance.EXACT_FORM_INVENTORY
        )
    else:
        evidence.provenance[candidate.global_ids[0]] = (
            evidence.provenance.get(candidate.global_ids[0], AnalysisProvenance(0))
            | AnalysisProvenance.PRIMARY_MORPHOLOGY
        )
    return evidence


def _finalize_surface(surface: str, evidence: _SurfaceEvidence) -> _EncodedCandidate:
    ordered = sorted(evidence.scores, key=lambda item: (-evidence.scores[item], item))
    truncated = len(ordered) > 8
    ordered = ordered[:8]
    flags = FLAG_CONTEXTUAL
    if len(ordered) > 1:
        flags |= FLAG_AMBIGUOUS
    if truncated:
        flags |= FLAG_TRUNCATED
    if any(
        evidence.provenance.get(item, AnalysisProvenance(0))
        & AnalysisProvenance.FOLDED_FORM_INVENTORY
        for item in ordered
    ):
        flags |= FLAG_FOLDED
    if not evidence.provenance.get(ordered[0], AnalysisProvenance(0)) & AnalysisProvenance.PRIMARY_MORPHOLOGY:
        flags |= FLAG_FALLBACK
    if evidence.context_proper_noun:
        flags |= FLAG_PROPER_NOUN
    confidence = min(1000, max(0, (evidence.scores[ordered[0]] + 8) // 16))
    return _EncodedCandidate(surface, tuple(ordered), confidence, evidence.difficulty, flags)


def compile_contextual_book(
    xhtml_spines: list[str],
    analyzer: LanguageAnalyzer,
    canonical: CanonicalLexiconIndex,
    frequency_provider: FrequencyProvider | None = None,
) -> CompiledContextualBook:
    if not xhtml_spines or len(xhtml_spines) > MAX_SPINES:
        raise ContextualEpubError(f"book must contain 1-{MAX_SPINES} XHTML spines")
    if analyzer.language != "de":
        raise ContextualEpubError("contextual analyzer language must be de")

    transformed = []
    spine_ranges = []
    shard_candidates: list[list[_EncodedCandidate]] = []
    source_token_base = 0
    missing = 0
    for xhtml in xhtml_spines:
        words, candidates_by_index, spine_missing = _analyze_spine(
            xhtml,
            analyzer,
            canonical,
            frequency_provider,
        )
        missing += spine_missing
        first_shard = len(shard_candidates)
        shard_count = (len(words) + SHARD_TOKEN_COUNT - 1) // SHARD_TOKEN_COUNT
        output = xhtml
        insertions = []
        for token_index in range(0, len(words), SHARD_TOKEN_COUNT):
            shard_id = first_shard + token_index // SHARD_TOKEN_COUNT
            insertions.append(
                (
                    words[token_index].raw_offset,
                    f'<span data-crossink-lang-shard="{shard_id}"></span>',
                )
            )
        for offset, marker in reversed(insertions):
            output = output[:offset] + marker + output[offset:]
        transformed.append(output)

        for shard_index in range(shard_count):
            start = shard_index * SHARD_TOKEN_COUNT
            end = min(start + SHARD_TOKEN_COUNT, len(words))
            by_surface: dict[str, _SurfaceEvidence] = {}
            for word_index in range(start, end):
                candidate = candidates_by_index.get(word_index)
                if candidate is not None:
                    by_surface[candidate.surface] = _merge_surface(
                        by_surface.get(candidate.surface),
                        candidate,
                    )
            ordered_surfaces = sorted(
                by_surface,
                key=lambda value: (
                    _fnv1a64(value.encode("utf-8")),
                    value.encode("utf-8"),
                ),
            )
            shard_candidates.append(
                [_finalize_surface(surface, by_surface[surface]) for surface in ordered_surfaces]
            )
        spine_ranges.append((first_shard, shard_count, source_token_base, len(words)))
        source_token_base += len(words)

    if len(shard_candidates) > MAX_SHARDS:
        raise ContextualEpubError(f"book exceeds {MAX_SHARDS} source shards")
    record_count = sum(len(items) for items in shard_candidates)
    if record_count > MAX_RECORDS:
        raise ContextualEpubError(f"book exceeds {MAX_RECORDS} shard candidates")
    global_ids = {
        canonical_id
        for shard in shard_candidates
        for candidate in shard
        for canonical_id in candidate.global_ids
    }
    if len(global_ids) > MAX_LOCAL_LEMMAS:
        raise ContextualEpubError(f"book exceeds {MAX_LOCAL_LEMMAS} local lemmas")
    ordered_global_ids = sorted(global_ids)
    local_by_global = {
        global_id: local_id
        for local_id, global_id in enumerate(ordered_global_ids)
    }

    spine_directory = b"".join(
        struct.pack("<II", first_shard, shard_count)
        for first_shard, shard_count, _, _ in spine_ranges
    )
    shard_directory = bytearray()
    shard_blobs = bytearray()
    for first_shard, shard_count, source_start, token_count in spine_ranges:
        for local_shard in range(shard_count):
            candidates = shard_candidates[first_shard + local_shard]
            blob = bytearray()
            for candidate in candidates:
                encoded = candidate.surface.encode("utf-8")
                local_ids = [local_by_global[item] for item in candidate.global_ids]
                record_start = len(blob)
                blob.extend(
                    struct.pack(
                        "<QHBBBBH",
                        _fnv1a64(encoded),
                        0,
                        len(encoded),
                        len(local_ids),
                        candidate.flags,
                        candidate.difficulty,
                        candidate.confidence,
                    )
                )
                blob.extend(struct.pack(f"<{len(local_ids)}H", *local_ids))
                blob.extend(encoded)
                _align4(blob)
                struct.pack_into("<H", blob, record_start + 8, len(blob) - record_start)
            if len(blob) > MAX_SHARD_BLOB_BYTES:
                raise ContextualEpubError(
                    f"shard blob exceeds {MAX_SHARD_BLOB_BYTES} bytes"
                )
            token_start = source_start + local_shard * SHARD_TOKEN_COUNT
            token_end = min(source_start + token_count, token_start + SHARD_TOKEN_COUNT)
            shard_directory.extend(
                struct.pack(
                    "<IHHIII",
                    len(shard_blobs),
                    len(blob),
                    len(candidates),
                    token_start,
                    token_end,
                    0,
                )
            )
            shard_blobs.extend(blob)

    local_lemmas = b"".join(struct.pack("<I", value) for value in ordered_global_ids)
    dwdsmor_package = canonical.dwdsmor_manifest["package"]
    zdl_model = canonical.zdl_manifest["model"]
    frequency_id = getattr(frequency_provider, "provider_id", "none") if frequency_provider else "none"
    metadata_json = json.dumps(
        {
            "analysisPolicyVersion": canonical.analyzer_metadata["analysisPolicyVersion"],
            "canonicalPosVersion": canonical.analyzer_metadata["canonicalPosVersion"],
            "canonicalUuid": str(canonical.canonical_uuid),
            "compilerVersion": COMPILER_VERSION,
            "dwdsmor": {
                "edition": canonical.dwdsmor_manifest["edition"],
                "sha256": dwdsmor_package["sha256"],
                "version": dwdsmor_package["version"],
            },
            "frequency": {"provider": frequency_id, "version": frequency_id},
            "shardTokenCount": SHARD_TOKEN_COUNT,
            "spacyVersion": canonical.zdl_manifest["spacyVersion"],
            "tokenizerVersion": TOKENIZER_VERSION,
            "zdl": {
                "model": zdl_model["distribution"],
                "sha256": zdl_model["sha256"],
                "version": zdl_model["version"],
            },
        },
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    if len(metadata_json) > 16 * 1024:
        raise ContextualEpubError("language metadata exceeds 16384 bytes")
    metadata = struct.pack("<4sHHII", b"CXLM", 1, 16, len(metadata_json), 0) + metadata_json

    artifact = bytearray(LANGUAGE_HEADER_SIZE)
    sections = []
    for section in (
        spine_directory,
        bytes(shard_directory),
        bytes(shard_blobs),
        local_lemmas,
        metadata,
    ):
        _align4(artifact)
        sections.append(len(artifact))
        artifact.extend(section)
    if len(artifact) > MAX_LANGUAGE_BYTES:
        raise ContextualEpubError("language artifact exceeds 64 MiB")
    struct.pack_into(
        "<4sHHIHH16s8s8sHHIIIIIIIIIIIII",
        artifact,
        0,
        b"CXLG",
        FORMAT_VERSION,
        LANGUAGE_HEADER_SIZE,
        0,
        TOKENIZER_VERSION,
        ANALYZER_VERSION,
        canonical.canonical_uuid.bytes,
        _language_field("de"),
        _language_field("und"),
        len(xhtml_spines),
        0,
        len(shard_candidates),
        record_count,
        len(ordered_global_ids),
        0,
        sections[0],
        sections[1],
        sections[2],
        sections[3],
        sections[4],
        0,
        0,
        len(artifact),
        0,
    )
    struct.pack_into("<I", artifact, 100, _crc32(artifact[LANGUAGE_HEADER_SIZE:]))
    struct.pack_into("<I", artifact, 104, _crc32(artifact[:104]))
    return CompiledContextualBook(tuple(transformed), bytes(artifact), missing)


def _local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _spine_paths(archive: zipfile.ZipFile) -> tuple[str, bytes, tuple[str, ...]]:
    try:
        container_data = archive.read("META-INF/container.xml")
        container = ET.fromstring(container_data)
        rootfiles = [
            element.attrib.get("full-path")
            for element in container.iter()
            if _local_name(element.tag) == "rootfile"
        ]
        rootfiles = [value for value in rootfiles if value]
        if len(rootfiles) != 1:
            raise ContextualEpubError("EPUB must declare exactly one rootfile")
        opf_path = rootfiles[0]
        opf_data = archive.read(opf_path)
        opf = ET.fromstring(opf_data)
    except ContextualEpubError:
        raise
    except (KeyError, ET.ParseError) as error:
        raise ContextualEpubError(f"cannot parse EPUB package: {error}") from error

    manifest_items = {}
    spine_ids = []
    for element in opf.iter():
        name = _local_name(element.tag)
        if name == "item":
            item_id = element.attrib.get("id")
            href = element.attrib.get("href")
            media_type = element.attrib.get("media-type")
            if item_id and href and media_type in ("application/xhtml+xml", "text/html"):
                manifest_items[item_id] = href
        elif name == "itemref":
            item_id = element.attrib.get("idref")
            if item_id:
                spine_ids.append(item_id)
    if not spine_ids or len(spine_ids) > MAX_SPINES:
        raise ContextualEpubError("EPUB spine count is invalid")

    opf_directory = posixpath.dirname(opf_path)
    paths = []
    for item_id in spine_ids:
        href = manifest_items.get(item_id)
        if href is None:
            raise ContextualEpubError(f"spine item {item_id!r} is not XHTML")
        parsed = urlsplit(href)
        if parsed.scheme or parsed.netloc:
            raise ContextualEpubError("external XHTML spine href is unsupported")
        decoded = unquote(parsed.path)
        normalized = posixpath.normpath(posixpath.join(opf_directory, decoded))
        if normalized == ".." or normalized.startswith("../") or PurePosixPath(normalized).is_absolute():
            raise ContextualEpubError("XHTML spine href escapes the EPUB")
        paths.append(normalized)
    if len(paths) != len(set(paths)):
        raise ContextualEpubError("EPUB spine contains duplicate XHTML paths")
    return opf_path, opf_data, tuple(paths)


def compile_contextual_epub(
    input_path: Path,
    output_path: Path,
    analyzer: LanguageAnalyzer,
    canonical: CanonicalLexiconIndex,
    frequency_provider: FrequencyProvider | None = None,
) -> CompiledContextualBook:
    try:
        if input_path.resolve() == output_path.resolve():
            raise ContextualEpubError("input and output EPUB paths must differ")
        with zipfile.ZipFile(input_path) as source:
            names = source.namelist()
            if len(names) != len(set(names)):
                raise ContextualEpubError("EPUB contains duplicate ZIP paths")
            opf_path, opf_data, spine_paths = _spine_paths(source)
            xhtml_spines = [source.read(path).decode("utf-8") for path in spine_paths]
            compiled = compile_contextual_book(
                xhtml_spines,
                analyzer,
                canonical,
                frequency_provider,
            )

            output_path.parent.mkdir(parents=True, exist_ok=True)
            file_descriptor, temporary_name = tempfile.mkstemp(
                prefix=output_path.name + ".tmp-",
                dir=output_path.parent,
            )
            os.close(file_descriptor)
            temporary = Path(temporary_name)
            try:
                replacements = {
                    path: xhtml.encode("utf-8")
                    for path, xhtml in zip(spine_paths, compiled.xhtml_spines)
                }
                with zipfile.ZipFile(temporary, "w", allowZip64=True) as target:
                    target.comment = source.comment
                    for info in source.infolist():
                        if info.filename == LANGUAGE_PATH:
                            continue
                        data = replacements.get(info.filename)
                        if data is None:
                            data = source.read(info.filename)
                        if info.filename == opf_path and data != opf_data:
                            raise ContextualEpubError("canonical OPF bytes changed")
                        target.writestr(info, data)
                    language_info = zipfile.ZipInfo(
                        LANGUAGE_PATH,
                        date_time=(1980, 1, 1, 0, 0, 0),
                    )
                    language_info.compress_type = zipfile.ZIP_STORED
                    language_info.create_system = 3
                    language_info.external_attr = 0o100644 << 16
                    target.writestr(language_info, compiled.language_artifact)
                with temporary.open("rb+") as output:
                    os.fsync(output.fileno())
                os.replace(temporary, output_path)
            finally:
                try:
                    temporary.unlink()
                except FileNotFoundError:
                    pass
    except ContextualEpubError:
        raise
    except (OSError, UnicodeDecodeError, zipfile.BadZipFile) as error:
        raise ContextualEpubError(f"cannot compile EPUB: {error}") from error
    return compiled

"""Bounded morphology fallback backed by a compiled de-DE form inventory."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import unicodedata
import zipfile
import zlib

from dictionary.contextual.compiler_support import CompilerDictionary, load_compiler_dictionary

from .analysis_policy import CanonicalAnalysis, CanonicalPos
from .pipeline import AnalysisProvenance, MorphologyAnalyzer, MorphologyCandidate


class FormInventoryError(RuntimeError):
    pass


@dataclass(frozen=True)
class FormInventoryLimits:
    max_surface_bytes: int = 255
    max_unique_analyses: int = 256

    def __post_init__(self) -> None:
        for name, value in self.__dict__.items():
            if not isinstance(value, int) or value <= 0:
                raise ValueError(f"{name} must be a positive integer")


@dataclass(frozen=True)
class FormInventoryIdentity:
    bundle_uuid: bytes
    lexeme_count: int
    source_sha256: str
    license_spdx: str


_REQUIRED_FILES = (
    "compiler/forms.bin",
    "device/headwords.bin",
    "device/lexemes.bin",
    "device/meta.bin",
)


def _analysis_key(candidate: MorphologyCandidate):
    analysis = candidate.analysis
    return (
        analysis.lemma.encode("utf-8"),
        int(analysis.part_of_speech),
        tuple(sorted(analysis.features.populated().items())),
    )


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _read_verified_bundle(path: Path) -> tuple[dict, dict[str, bytes]]:
    try:
        archive_sha256 = _file_sha256(path)
        with zipfile.ZipFile(path) as archive:
            if len(archive.namelist()) != len(set(archive.namelist())):
                raise FormInventoryError("bundle contains duplicate paths")
            manifest_data = archive.read("manifest.json")
            manifest = json.loads(manifest_data.decode("utf-8"))
            files = {name: archive.read(name) for name in _REQUIRED_FILES}
    except FormInventoryError:
        raise
    except (OSError, KeyError, UnicodeDecodeError, json.JSONDecodeError, zipfile.BadZipFile) as error:
        raise FormInventoryError(f"cannot read form inventory: {error}") from error

    if not isinstance(manifest, dict) or manifest.get("formatVersion") != 1:
        raise FormInventoryError("unsupported dictionary bundle manifest")
    if manifest.get("sourceLanguage") != "de" or manifest.get("targetLanguage") != "de":
        raise FormInventoryError("form inventory must be a de-DE dictionary bundle")
    file_manifest = manifest.get("files")
    if not isinstance(file_manifest, dict):
        raise FormInventoryError("bundle manifest has no file inventory")
    for name, data in files.items():
        record = file_manifest.get(name)
        if not isinstance(record, dict):
            raise FormInventoryError(f"bundle manifest omits {name}")
        if record.get("bytes") != len(data) or record.get("sha256") != hashlib.sha256(data).hexdigest():
            raise FormInventoryError(f"bundle hash mismatch for {name}")
    manifest["_archiveSha256"] = archive_sha256
    return manifest, files


def _load_lexeme_analyses(meta: bytes, records: bytes, headwords: bytes) -> tuple[CanonicalAnalysis, ...]:
    if len(meta) != 80 or meta[:4] != b"CXDM":
        raise FormInventoryError("invalid dictionary metadata")
    lexeme_count, record_size = struct.unpack_from("<IH", meta, 44)
    expected_records, expected_headwords = struct.unpack_from("<II", meta, 52)
    records_crc, headwords_crc = struct.unpack_from("<II", meta, 64)
    if record_size != 24 or expected_records != len(records) or expected_headwords != len(headwords):
        raise FormInventoryError("dictionary lexeme files have invalid sizes")
    if len(records) != lexeme_count * record_size:
        raise FormInventoryError("dictionary lexeme count does not match records")
    if zlib.crc32(records) & 0xFFFFFFFF != records_crc:
        raise FormInventoryError("dictionary lexeme records are corrupt")
    if zlib.crc32(headwords) & 0xFFFFFFFF != headwords_crc:
        raise FormInventoryError("dictionary headwords are corrupt")

    analyses = []
    for lexeme_id in range(lexeme_count):
        offset = lexeme_id * record_size
        headword_offset = struct.unpack_from("<I", records, offset)[0]
        headword_length, raw_pos = struct.unpack_from("<HB", records, offset + 20)
        if headword_length == 0 or headword_offset + headword_length > len(headwords):
            raise FormInventoryError(f"lexeme {lexeme_id} has an invalid headword range")
        try:
            headword = headwords[headword_offset : headword_offset + headword_length].decode("utf-8")
            part_of_speech = CanonicalPos(raw_pos)
        except (UnicodeDecodeError, ValueError) as error:
            raise FormInventoryError(f"lexeme {lexeme_id} has invalid identity") from error
        if unicodedata.normalize("NFC", headword) != headword:
            raise FormInventoryError(f"lexeme {lexeme_id} headword is not NFC")
        analyses.append(CanonicalAnalysis(headword, part_of_speech))
    return tuple(analyses)


class DeDeFormInventoryAnalyzer:
    """Resolve exact and case-folded forms from compiler-only de-DE data."""

    def __init__(
        self,
        dictionary: CompilerDictionary,
        lexemes: tuple[CanonicalAnalysis, ...],
        identity: FormInventoryIdentity,
        limits: FormInventoryLimits = FormInventoryLimits(),
    ):
        if dictionary.source_language != "de" or dictionary.target_language != "de":
            raise ValueError("form inventory must be de-DE")
        if dictionary.lexeme_count != len(lexemes) or dictionary.bundle_uuid != identity.bundle_uuid:
            raise ValueError("form inventory identity does not match lexeme records")
        self._dictionary = dictionary
        self._lexemes = lexemes
        self._identity = identity
        self._limits = limits

    @property
    def identity(self) -> FormInventoryIdentity:
        return self._identity

    @classmethod
    def from_cpdict(
        cls,
        path: Path,
        limits: FormInventoryLimits = FormInventoryLimits(),
    ) -> "DeDeFormInventoryAnalyzer":
        manifest, files = _read_verified_bundle(path)
        try:
            dictionary = load_compiler_dictionary(
                files["device/meta.bin"],
                files["compiler/forms.bin"],
            )
        except ValueError as error:
            raise FormInventoryError(f"invalid compiler form inventory: {error}") from error
        lexemes = _load_lexeme_analyses(
            files["device/meta.bin"],
            files["device/lexemes.bin"],
            files["device/headwords.bin"],
        )
        license_data = manifest.get("license")
        if not isinstance(license_data, dict) or not isinstance(license_data.get("spdx"), str):
            raise FormInventoryError("bundle manifest has no SPDX license")
        identity = FormInventoryIdentity(
            dictionary.bundle_uuid,
            dictionary.lexeme_count,
            manifest["_archiveSha256"],
            license_data["spdx"],
        )
        return cls(dictionary, lexemes, identity, limits)

    def analyze_surface(self, surface: str) -> tuple[MorphologyCandidate, ...]:
        if not isinstance(surface, str) or not surface:
            raise FormInventoryError("surface must be non-empty text")
        normalized = unicodedata.normalize("NFC", surface)
        surface_bytes = len(normalized.encode("utf-8"))
        if surface_bytes > self._limits.max_surface_bytes:
            raise FormInventoryError(
                f"surface has {surface_bytes} UTF-8 bytes; cap is {self._limits.max_surface_bytes}"
            )

        by_analysis: dict[CanonicalAnalysis, AnalysisProvenance] = {}
        exact = self._dictionary.forms.get(normalized)
        if exact is not None:
            for lexeme_id in exact.lexeme_ids:
                by_analysis[self._lexemes[lexeme_id]] = AnalysisProvenance.EXACT_FORM_INVENTORY
        folded = self._dictionary.folded_forms.get(normalized.casefold())
        if folded is not None:
            for lexeme_id in folded.lexeme_ids:
                analysis = self._lexemes[lexeme_id]
                by_analysis.setdefault(analysis, AnalysisProvenance.FOLDED_FORM_INVENTORY)
        if len(by_analysis) > self._limits.max_unique_analyses:
            raise FormInventoryError(
                f"surface exceeds inventory-analysis cap {self._limits.max_unique_analyses}"
            )
        return tuple(
            sorted(
                (
                    MorphologyCandidate(analysis, provenance)
                    for analysis, provenance in by_analysis.items()
                ),
                key=_analysis_key,
            )
        )


class AugmentedMorphologyAnalyzer:
    """Union primary morphology with a lower-precedence form inventory."""

    def __init__(
        self,
        primary: MorphologyAnalyzer,
        inventory: MorphologyAnalyzer,
        limits: FormInventoryLimits = FormInventoryLimits(),
    ):
        self._primary = primary
        self._inventory = inventory
        self._limits = limits

    def analyze_surface(self, surface: str) -> tuple[MorphologyCandidate, ...]:
        by_analysis: dict[CanonicalAnalysis, AnalysisProvenance] = {}
        for analyzer in (self._primary, self._inventory):
            for candidate in analyzer.analyze_surface(surface):
                if not isinstance(candidate, MorphologyCandidate):
                    raise FormInventoryError("morphology provider returned an invalid candidate")
                by_analysis[candidate.analysis] = (
                    by_analysis.get(candidate.analysis, AnalysisProvenance(0))
                    | candidate.provenance
                )
                if len(by_analysis) > self._limits.max_unique_analyses:
                    raise FormInventoryError(
                        f"combined morphology exceeds cap {self._limits.max_unique_analyses}"
                    )
        return tuple(
            sorted(
                (
                    MorphologyCandidate(analysis, provenance)
                    for analysis, provenance in by_analysis.items()
                ),
                key=lambda item: (
                    0 if item.provenance & AnalysisProvenance.PRIMARY_MORPHOLOGY else 1,
                    0 if item.provenance & AnalysisProvenance.EXACT_FORM_INVENTORY else 1,
                    _analysis_key(item),
                ),
            )
        )

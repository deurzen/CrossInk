"""Validated coverage and conflict reporting for canonical definition sources."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct
import zipfile

from .analysis_policy import CanonicalPos
from .canonical_lexicon import CanonicalLexiconIndex
from .definition_source import DefinitionSourceError, validate_definition_files


class CoverageReportError(ValueError):
    pass


def _percent(count: int, total: int) -> float:
    return round(count * 100.0 / total, 6) if total else 0.0


def _percentile(sorted_values: list[int], percentile: int) -> int:
    if not sorted_values:
        return 0
    index = (len(sorted_values) - 1) * percentile // 100
    return sorted_values[index]


def _read_source(path: Path, canonical: CanonicalLexiconIndex) -> dict:
    required = (
        "compiler/coverage.json",
        "device/entries.bin",
        "device/entry-index.bin",
        "device/meta.bin",
    )
    try:
        with zipfile.ZipFile(path) as archive:
            manifest = json.loads(archive.read("manifest.json").decode("utf-8"))
            files = {name: archive.read(name) for name in required}
    except (OSError, KeyError, UnicodeDecodeError, json.JSONDecodeError, zipfile.BadZipFile) as error:
        raise CoverageReportError(f"cannot read definition source {path}: {error}") from error
    if not isinstance(manifest, dict) or manifest.get("packageType") != "definition-source":
        raise CoverageReportError(f"{path} is not a definition-source bundle")
    for name, data in files.items():
        expected = manifest.get("files", {}).get(name)
        if (
            not isinstance(expected, dict)
            or expected.get("bytes") != len(data)
            or expected.get("sha256") != hashlib.sha256(data).hexdigest()
        ):
            raise CoverageReportError(f"definition source hash mismatch for {name}")
    try:
        source_uuid = validate_definition_files(
            files["device/meta.bin"],
            files["device/entry-index.bin"],
            files["device/entries.bin"],
            canonical.canonical_uuid,
            canonical.lexeme_count,
        )
        compiler_report = json.loads(files["compiler/coverage.json"].decode("utf-8"))
    except (DefinitionSourceError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CoverageReportError(f"invalid definition source {path}: {error}") from error
    if not isinstance(compiler_report, dict):
        raise CoverageReportError(f"invalid compiler coverage report in {path}")

    index = files["device/entry-index.bin"]
    present = set()
    lengths = []
    for canonical_id in range(canonical.lexeme_count):
        _, length = struct.unpack_from("<II", index, canonical_id * 8)
        if length:
            present.add(canonical_id)
            lengths.append(length)
    lengths.sort()
    return {
        "path": str(path),
        "manifest": manifest,
        "compiler": compiler_report,
        "uuid": source_uuid,
        "present": present,
        "lengths": lengths,
        "index": index,
    }


def build_coverage_report(
    canonical: CanonicalLexiconIndex,
    sources: dict[str, Path],
) -> dict:
    if not sources or len(sources) > 3:
        raise CoverageReportError("coverage report requires one to three sources")
    loaded = {name: _read_source(path, canonical) for name, path in sorted(sources.items())}
    id_to_key = [None] * canonical.lexeme_count
    for key, canonical_id in canonical.by_key.items():
        id_to_key[canonical_id] = key

    source_reports = {}
    for name, source in loaded.items():
        present = source["present"]
        by_pos = {}
        for part_of_speech in CanonicalPos:
            total = sum(1 for key in id_to_key if key[1] == part_of_speech)
            covered = sum(1 for canonical_id in present if id_to_key[canonical_id][1] == part_of_speech)
            if total:
                by_pos[part_of_speech.name.lower().replace("_", "-")] = {
                    "covered": covered,
                    "total": total,
                    "percent": _percent(covered, total),
                }
        # Entry payload text is intentionally absent from this shareable report.
        index = source["index"]
        largest_records = []
        for canonical_id in present:
            _, length = struct.unpack_from("<II", index, canonical_id * 8)
            largest_records.append((length, canonical_id))
        largest_records.sort(key=lambda item: (-item[0], item[1]))
        pathological = [
            {
                "canonicalId": canonical_id,
                "headword": id_to_key[canonical_id][0],
                "partOfSpeech": id_to_key[canonical_id][1].name.lower().replace("_", "-"),
                "bytes": length,
            }
            for length, canonical_id in largest_records[:20]
        ]
        provenance = source["compiler"].get("provenance", {})
        import_stats = provenance.get("import", {}) if isinstance(provenance, dict) else {}
        source_reports[name] = {
            "sourceUuid": str(source["uuid"]),
            "sourceLabel": source["manifest"]["sourceLabel"],
            "direction": f"{source['manifest']['sourceLanguage']}→{source['manifest']['targetLanguage']}",
            "coverage": len(present),
            "coveragePercent": _percent(len(present), canonical.lexeme_count),
            "entryBytes": source["manifest"]["files"]["device/entries.bin"]["bytes"],
            "entrySizeBytes": {
                "p50": _percentile(source["lengths"], 50),
                "p95": _percentile(source["lengths"], 95),
                "p99": _percentile(source["lengths"], 99),
                "max": source["lengths"][-1] if source["lengths"] else 0,
            },
            "byPartOfSpeech": by_pos,
            "unmatchedCount": source["compiler"].get("unmatchedCount", 0),
            "unalignedInputCount": import_stats.get(
                "unalignedLines",
                import_stats.get("unalignedRecords", 0),
            ),
            "posConflictInputCount": import_stats.get(
                "posConflictLines",
                import_stats.get("posConflictRecords", 0),
            ),
            "truncatedFieldCount": source["compiler"].get("truncatedFieldCount", 0),
            "largestEntries": pathological,
        }

    names = sorted(loaded)
    sets = {name: loaded[name]["present"] for name in names}
    union = set().union(*sets.values())
    intersections = {}
    for left_index, left in enumerate(names):
        for right in names[left_index + 1 :]:
            intersections[f"{left}&{right}"] = len(sets[left] & sets[right])
    all_sources = set.intersection(*(sets[name] for name in names))
    source_only = {
        name: len(sets[name] - set().union(*(sets[other] for other in names if other != name)))
        for name in names
    }
    return {
        "schemaVersion": 1,
        "canonicalUuid": str(canonical.canonical_uuid),
        "canonicalLexemeCount": canonical.lexeme_count,
        "sources": source_reports,
        "union": {
            "covered": len(union),
            "percent": _percent(len(union), canonical.lexeme_count),
            "allSources": len(all_sources),
            "intersections": intersections,
            "sourceOnly": source_only,
        },
    }

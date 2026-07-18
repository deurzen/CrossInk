#!/usr/bin/env python3
"""Validate C35/C36 contextual dictionary metrics from an ESP32 serial log."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import statistics
import sys

LOOKUP_RE = re.compile(r"Lookup start: free=(\d+) maxAlloc=(\d+) stackHwm=(\d+)")
DEFINITION_RE = re.compile(
    r"Definition prepared: (\d+) ms opens=(\d+) switches=(\d+) seeks=(\d+) "
    r"reads=(\d+) bytes=(\d+) free=(\d+) maxAlloc=(\d+) stackHwm=(\d+)"
)
RELEASE_RE = re.compile(
    r"Activity resources released: total=(\d+) ms free=(\d+) maxAlloc=(\d+) stackHwm=(\d+)"
)
STATUS_RE = re.compile(
    r"Status saved: value=(\d+) generation=(\d+) free=(\d+) maxAlloc=(\d+) stackHwm=(\d+)"
)
ATTACHMENT_RE = re.compile(r"Attachment order saved: generation=(\d+) sources=(\d+)")
SD_FAILURE_RE = re.compile(r"\[DIN\] (?:Failed to open file for reading|File does not exist):")
DICTIONARY_ERROR_RE = re.compile(r"\[ERR\] \[DICT\]")


def _matches(pattern: re.Pattern[str], text: str) -> list[tuple[int, ...]]:
    return [tuple(int(value) for value in match.groups()) for match in pattern.finditer(text)]


def parse_log(text: str) -> dict[str, object]:
    return {
        "lookups": _matches(LOOKUP_RE, text),
        "definitions": _matches(DEFINITION_RE, text),
        "releases": _matches(RELEASE_RE, text),
        "statuses": _matches(STATUS_RE, text),
        "attachments": _matches(ATTACHMENT_RE, text),
        "dictionaryErrors": [line for line in text.splitlines() if DICTIONARY_ERROR_RE.search(line)],
        "sdFailures": [line for line in text.splitlines() if SD_FAILURE_RE.search(line)],
    }


def _window_median(values: list[int], warmup: int, window: int) -> tuple[int, int] | None:
    stable = values[min(warmup, len(values)) :]
    if len(stable) < window * 2:
        return None
    first = round(statistics.median(stable[:window]))
    last = round(statistics.median(stable[-window:]))
    return first, last


def _range_summary(values: list[int]) -> dict[str, int] | None:
    if not values:
        return None
    return {
        "min": min(values),
        "median": round(statistics.median(values)),
        "max": max(values),
    }


def qualify(
    parsed: dict[str, object],
    *,
    min_lookups: int,
    min_definitions: int,
    min_status_updates: int,
    min_attachment_updates: int,
    expected_sources: int,
    min_stack_hwm: int,
    heap_tolerance: int,
    warmup: int,
    window: int,
) -> tuple[dict[str, object], list[str]]:
    lookups = parsed["lookups"]
    definitions = parsed["definitions"]
    releases = parsed["releases"]
    statuses = parsed["statuses"]
    attachments = parsed["attachments"]
    assert isinstance(lookups, list)
    assert isinstance(definitions, list)
    assert isinstance(releases, list)
    assert isinstance(statuses, list)
    assert isinstance(attachments, list)

    failures: list[str] = []
    if len(lookups) < min_lookups:
        failures.append(f"lookup count {len(lookups)} is below {min_lookups}")
    if len(definitions) < min_definitions:
        failures.append(f"definition count {len(definitions)} is below {min_definitions}")
    if len(releases) < min_definitions:
        failures.append(f"activity release count {len(releases)} is below {min_definitions}")
    if len(statuses) < min_status_updates:
        failures.append(f"status update count {len(statuses)} is below {min_status_updates}")
    if len(attachments) < min_attachment_updates:
        failures.append(f"attachment update count {len(attachments)} is below {min_attachment_updates}")

    dictionary_errors = parsed["dictionaryErrors"]
    sd_failures = parsed["sdFailures"]
    assert isinstance(dictionary_errors, list)
    assert isinstance(sd_failures, list)
    if dictionary_errors:
        failures.append(f"serial log contains {len(dictionary_errors)} dictionary error(s)")
    if sd_failures:
        failures.append(f"serial log contains {len(sd_failures)} DIN open failure(s)")

    minimum_definition_reads = expected_sources + 3
    for index, definition in enumerate(definitions):
        _, opens, switches, seeks, reads, byte_count, *_ = definition
        if opens == 0 or seeks == 0 or byte_count == 0:
            failures.append(f"definition {index + 1} has incomplete SD metrics")
        if reads < minimum_definition_reads:
            failures.append(
                f"definition {index + 1} has {reads} reads; expected at least {minimum_definition_reads}"
            )
        if switches < expected_sources:
            failures.append(
                f"definition {index + 1} has {switches} source switches; expected at least {expected_sources}"
            )

    if attachments and attachments[-1][1] != expected_sources:
        failures.append(
            f"final attachment update has {attachments[-1][1]} sources; expected {expected_sources}"
        )
    generations = [item[1] for item in statuses]
    if any(after <= before for before, after in zip(generations, generations[1:])):
        failures.append("status generations are not strictly increasing")
    attachment_generations = [item[0] for item in attachments]
    if any(after <= before for before, after in zip(attachment_generations, attachment_generations[1:])):
        failures.append("attachment generations are not strictly increasing")

    stack_values = [item[2] for item in lookups]
    stack_values.extend(item[8] for item in definitions)
    stack_values.extend(item[3] for item in releases)
    stack_values.extend(item[4] for item in statuses)
    if stack_values and min(stack_values) < min_stack_hwm:
        failures.append(
            f"minimum stack high-water mark is {min(stack_values)} bytes; required {min_stack_hwm}"
        )

    lookup_free = [item[0] for item in lookups]
    lookup_max_alloc = [item[1] for item in lookups]
    free_window = _window_median(lookup_free, warmup, window)
    max_alloc_window = _window_median(lookup_max_alloc, warmup, window)
    if free_window is None or max_alloc_window is None:
        failures.append(
            f"need at least {warmup + window * 2} lookup samples for warm heap comparison"
        )
    else:
        if free_window[1] + heap_tolerance < free_window[0]:
            failures.append(
                f"free-heap median declined {free_window[0] - free_window[1]} bytes "
                f"(tolerance {heap_tolerance})"
            )
        if max_alloc_window[1] + heap_tolerance < max_alloc_window[0]:
            failures.append(
                f"largest-block median declined {max_alloc_window[0] - max_alloc_window[1]} bytes "
                f"(tolerance {heap_tolerance})"
            )

    summary = {
        "lookups": len(lookups),
        "definitions": len(definitions),
        "activityReleases": len(releases),
        "statusUpdates": len(statuses),
        "attachmentUpdates": len(attachments),
        "lookupFreeHeap": _range_summary(lookup_free),
        "lookupMaxAlloc": _range_summary(lookup_max_alloc),
        "lookupStackHwm": _range_summary([item[2] for item in lookups]),
        "coldDefinitionMilliseconds": definitions[0][0] if definitions else None,
        "warmDefinitionMilliseconds": _range_summary([item[0] for item in definitions[1:]]),
        "definitionMilliseconds": _range_summary([item[0] for item in definitions]),
        "definitionOpens": _range_summary([item[1] for item in definitions]),
        "definitionSourceSwitches": _range_summary([item[2] for item in definitions]),
        "definitionSeeks": _range_summary([item[3] for item in definitions]),
        "definitionReads": _range_summary([item[4] for item in definitions]),
        "definitionBytes": _range_summary([item[5] for item in definitions]),
        "warmFreeHeapMedian": list(free_window) if free_window else None,
        "warmMaxAllocMedian": list(max_alloc_window) if max_alloc_window else None,
        "dictionaryErrors": len(dictionary_errors),
        "sdFailures": len(sd_failures),
        "passed": not failures,
    }
    return summary, failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--min-lookups", type=int, default=100)
    parser.add_argument("--min-definitions", type=int, default=100)
    parser.add_argument("--min-status-updates", type=int, default=10)
    parser.add_argument("--min-attachment-updates", type=int, default=2)
    parser.add_argument("--expected-sources", type=int, default=3)
    parser.add_argument("--min-stack-hwm", type=int, default=1024)
    parser.add_argument("--heap-tolerance", type=int, default=256)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--window", type=int, default=10)
    parser.add_argument("--json", type=Path, help="also write the summary as JSON")
    args = parser.parse_args()
    if min(
        args.min_lookups,
        args.min_definitions,
        args.min_status_updates,
        args.min_attachment_updates,
        args.expected_sources,
        args.min_stack_hwm,
        args.heap_tolerance,
        args.warmup,
        args.window,
    ) < 0 or args.window == 0:
        parser.error("counts and thresholds must be non-negative; window must be positive")

    parsed = parse_log(args.log.read_text(encoding="utf-8", errors="replace"))
    summary, failures = qualify(
        parsed,
        min_lookups=args.min_lookups,
        min_definitions=args.min_definitions,
        min_status_updates=args.min_status_updates,
        min_attachment_updates=args.min_attachment_updates,
        expected_sources=args.expected_sources,
        min_stack_hwm=args.min_stack_hwm,
        heap_tolerance=args.heap_tolerance,
        warmup=args.warmup,
        window=args.window,
    )
    output = json.dumps(summary, indent=2, sort_keys=True)
    print(output)
    if args.json:
        args.json.write_text(output + "\n", encoding="utf-8")
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())

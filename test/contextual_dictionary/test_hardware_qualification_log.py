from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from qualify_contextual_dictionary_log import parse_log, qualify  # noqa: E402


def qualification_lines(count=6, heap_step=0, stack=1800):
    lines = []
    for index in range(count):
        free = 210000 - index * heap_step
        max_alloc = 170000 - index * heap_step
        lines.extend(
            (
                f"[{index}] [INF] [DICT] Lookup start: free={free} maxAlloc={max_alloc} stackHwm={stack}",
                f"[{index}] [INF] [DICT] Definition prepared: 42 ms opens=9 switches=8 seeks=9 "
                f"reads=9 bytes=840 free={free - 8000} maxAlloc={max_alloc - 4000} stackHwm={stack - 100}",
                f"[{index}] [INF] [DICT] Activity resources released: total=90 ms free={free - 200} "
                f"maxAlloc={max_alloc - 100} stackHwm={stack - 100}",
            )
        )
    lines.extend(
        (
            "[20] [INF] [DICT] Status saved: value=0 generation=10 free=200000 maxAlloc=160000 stackHwm=1700",
            "[21] [INF] [DICT] Status saved: value=1 generation=11 free=200000 maxAlloc=160000 stackHwm=1700",
            "[22] [INF] [DICT] Attachment order saved: generation=4 sources=2",
            "[23] [INF] [DICT] Attachment order saved: generation=5 sources=3",
        )
    )
    return "\n".join(lines)


def qualify_fixture(text, **overrides):
    arguments = {
        "min_lookups": 6,
        "min_definitions": 6,
        "min_status_updates": 2,
        "min_attachment_updates": 2,
        "expected_sources": 3,
        "min_stack_hwm": 1024,
        "heap_tolerance": 128,
        "warmup": 0,
        "window": 2,
    }
    arguments.update(overrides)
    return qualify(parse_log(text), **arguments)


class HardwareQualificationLogTest(unittest.TestCase):
    def test_accepts_stable_complete_endurance_log(self):
        summary, failures = qualify_fixture(qualification_lines())
        self.assertEqual(failures, [])
        self.assertTrue(summary["passed"])
        self.assertEqual(summary["lookups"], 6)
        self.assertEqual(summary["definitions"], 6)
        self.assertEqual(summary["warmFreeHeapMedian"], [210000, 210000])
        self.assertEqual(summary["lookupStackHwm"]["min"], 1800)

    def test_rejects_heap_decline_low_stack_and_dictionary_io_failures(self):
        text = qualification_lines(heap_step=100, stack=900)
        text += "\n[99] [ERR] [DICT] Contextual index lookup failed: canonical-invalid"
        text += "\n[99] [DIN] Failed to open file for reading: /.crosspoint/lexicons/x/meta.bin"
        summary, failures = qualify_fixture(text, heap_tolerance=0)
        self.assertFalse(summary["passed"])
        self.assertTrue(any("free-heap median declined" in failure for failure in failures))
        self.assertTrue(any("largest-block median declined" in failure for failure in failures))
        self.assertTrue(any("stack high-water" in failure for failure in failures))
        self.assertTrue(any("dictionary error" in failure for failure in failures))
        self.assertTrue(any("DIN open failure" in failure for failure in failures))

    def test_rejects_missing_cycles_bad_source_metrics_and_nonmonotonic_generations(self):
        text = qualification_lines(count=2)
        text = text.replace("reads=9", "reads=4", 1).replace("switches=8", "switches=1", 1)
        text += "\n[24] [INF] [DICT] Status saved: value=2 generation=9 free=1 maxAlloc=1 stackHwm=1700"
        text += "\n[25] [INF] [DICT] Attachment order saved: generation=4 sources=3"
        _, failures = qualify_fixture(text)
        self.assertTrue(any("lookup count" in failure for failure in failures))
        self.assertTrue(any("has 4 reads" in failure for failure in failures))
        self.assertTrue(any("has 1 source switches" in failure for failure in failures))
        self.assertTrue(any("status generations" in failure for failure in failures))
        self.assertTrue(any("attachment generations" in failure for failure in failures))
        self.assertTrue(any("warm heap comparison" in failure for failure in failures))


if __name__ == "__main__":
    unittest.main()

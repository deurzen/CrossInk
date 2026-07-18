# Contextual Dictionary Hardware Qualification

This runbook closes C35 and C36 on physical X3/X4 hardware. Host and simulator
results cannot prove ESP32-C3 heap behavior, FreeRTOS stack margin, e-ink layout
or the SD driver's one-reader constraint.

## Qualified inputs

Use a firmware containing the qualification metrics from this branch and these
runtime assets:

| Asset | Expected identity |
| --- | --- |
| German canonical lexicon | `c6246d63-38eb-5df8-9d83-ab0d812503f9` |
| German Wiktionary source | `8bccd788-22d0-5306-a00d-f8d96b8d8652` |
| dict.cc de-en source | `122f8887-ab1b-574b-9026-ced568335fc0` |
| Kaikki de-en source | `84546eba-7af7-53e3-8f0c-a9e79414cc9c` |

Attach the sources in Wiktionary, dict.cc, Kaikki order. Upload the validated v5
production EPUB after deleting or overwriting its previous device copy. The
Christian Homma novel contains `knipste`; its primary canonical lemma `knipsen`
is covered by all three installed sources.

Capture the complete 115200-baud serial stream. On Linux, for example:

```sh
mkdir -p tmp.local/qualification
pio device monitor -b 115200 | tee tmp.local/qualification/x3-contextual.log
```

The added `DICT` lines report free heap, largest allocatable block and the
ESP-IDF task stack high-water mark in bytes. Definition lines also report
opens, source switches, seeks, reads and bytes for that explicit definition.
The metrics add no retained buffer; counters already live in the heap-owned
lookup session.

## C35 production lookup matrix

Run this matrix once on X3 and once on X4. Start each device with a reboot and
retain the unedited log plus firmware commit, SD-card model and filesystem.

| Step | Action | Expected result |
| --- | --- | --- |
| 1 | Open the converted Homma novel and navigate to `knipste` | Normal page rendering; no dictionary I/O during page turns |
| 2 | Open Dictionary Lookup for the first time after reboot | A shortlist appears and the serial log records the cold lookup stages |
| 3 | Open `knipste` | Header is `knipste · knipsen`; all three labeled sources are readable in attachment order |
| 4 | Page forward and backward across source boundaries | Text and divider placement replay identically without clipping |
| 5 | Close the dictionary and repeat the same lookup twice | Warm timings are recorded and resources are released after each close |
| 6 | Mark a different test word Learning, close, and reboot | The primary canonical item remains suppressed and generation advances once |
| 7 | Repeat in portrait and both landscape orientations | Header, source labels, final line and button hints remain inside safe bounds |

For a short C35 capture containing at least three complete lookups, run:

```sh
python3 scripts/qualify_contextual_dictionary_log.py \
  tmp.local/qualification/x3-contextual.log \
  --min-lookups 3 --min-definitions 3 \
  --min-status-updates 1 --min-attachment-updates 0 \
  --warmup 0 --window 1 \
  --json tmp.local/qualification/x3-c35.json
```

Repeat with X4 paths. Record the first definition time as cold and the remaining
range as warm. Preserve the reported I/O, minimum heap/largest block and minimum
stack high-water mark. A pass requires at least 1,024 stack bytes free at the
worst observed point, no `DICT` errors, no `[DIN] Failed to open` line and no
warm heap or largest-block decline beyond the parser's 256-byte tolerance.

## C36 100-cycle endurance sequence

Use the same converted novel, package order and serial capture. Complete five
unmeasured warm-up cycles first, then perform 100 complete cycles of: open
Dictionary Lookup, open a definition covered by all three sources, page once,
return, and close the dictionary activity.

At cycles 10 through 100, mark one distinct item Learning, Known or Ignore so
there are at least ten successful WAL updates. At cycle 35, enter network mode,
change source order to Kaikki, Wiktionary, dict.cc, leave network mode and
continue. At cycle 70, restore Wiktionary, dict.cc, Kaikki. Reboot at cycle 50
and verify that previously saved status remains effective before continuing the
same serial capture.

Run the default endurance gate:

```sh
python3 scripts/qualify_contextual_dictionary_log.py \
  tmp.local/qualification/x3-contextual-100.log \
  --json tmp.local/qualification/x3-c36.json
```

The default gate requires 100 lookups, 100 definitions and activity releases,
ten status writes, two attachment updates, three final sources, monotonically
increasing state/attachment generations, at least 1,024 stack bytes, bounded
per-definition SD activity, no dictionary or DIN errors, and stable warm free
heap/largest block medians. It discards five warm-up samples and compares the
first and last ten-sample medians with a 256-byte tolerance.

After the run, reboot again and verify all three source labels and the restored
order, then inspect the Dictionaries page for a valid attachment generation.
Do not mark C35 or C36 Done from simulator values or from a partial serial log.
If the parser fails, preserve the raw log and investigate the first failing
cycle rather than relaxing thresholds.

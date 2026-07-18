# Contextual Dictionary Input Contract

This document freezes U03. It separates definition-page navigation from direct
word navigation using the logical button distinctions that already exist in
`MappedInputManager`. It adds no setting and never reads raw hardware button
indices in `DictionaryActivity`.

## Logical button map

| Activity mode | Back | Confirm | Left | Right | Up | Down |
| --- | --- | --- | --- | --- | --- | --- |
| Shortlist | close dictionary | open selected word | previous word | next word | previous shortlist page | next shortlist page |
| Definition | return to shortlist | open status selection | previous definition page | next definition page | previous word | next word |
| Status | cancel status selection | save selected status | previous status | next status | previous status | next status |

`Left` and `Right` are the remappable front-navigation actions. `Up` and `Down`
are the orientation-aware side-button actions. Physical placement, reader front
button remapping, inverted mode and landscape swapping continue to be handled by
`MappedInputManager`; the dictionary consumes only
`MappedInputManager::Button::*`.

The four bottom/front hints describe Back, Confirm and compact Up/Down page
movement. Hardware qualification replaced the longer previous/next-page labels
because they overflowed the X3/X4 hint boxes. Definition mode separately shows
a `current/total` word indicator plus bounded `Up: previous-word` and
`Down: next-word` previews so side-button word movement is visible before input.

## Event priority

At most one action is performed per `loop()` iteration. If a simulator injection
or unusual hardware mapping reports more than one release in the same frame,
use this priority:

1. Back;
2. Confirm;
3. Left;
4. Right;
5. Up;
6. Down.

Every handled branch returns immediately. This preserves Back as an escape path,
prevents a status save and word change in one frame, and makes tests independent
of condition ordering.

## Shortlist behavior

Existing behavior remains unchanged:

- Left decrements the sorted item index and wraps from first to last.
- Right increments the sorted item index and wraps from last to first.
- Up moves back by one visible shortlist page and clamps at the first item.
- Down moves forward by one visible shortlist page and clamps at the last item.
- Confirm opens page zero for the selected word.
- Back closes the activity and releases session/shortlist resources.

The sorted order remains difficulty descending, confidence descending, then
visible page order. Direct definition-mode navigation operates on this exact
array; it never creates a second order.

## Definition-page behavior

Left and Right navigate the existing source × analysis page cursor for the
current word:

- Left on page zero is a no-op.
- Left on a later page uses the existing bounded replay from the entry start.
- Right advances only when `hasNext` is true.
- Right on the final page is a no-op.
- A page action never changes `selected_`.

The cursor continues sequentially through meanings, attached sources and
canonical analyses. Source order remains the attachment order. This contract
does not add source jumping.

If definition loading failed, Left/Right are no-ops. Back, Up and Down remain
available so a missing/corrupt definition cannot trap the user.

## Direct previous/next word behavior

Up means the preceding item in the sorted shortlist; Down means the following
item. Navigation wraps at both ends:

```text
Up:   index 0       -> count - 1
Down: index count-1 -> 0
```

If the shortlist contains one unsuppressed item, Up and Down are no-ops and do
not reread SD data. Otherwise a word change:

1. updates `selected_` exactly once;
2. clears primary title, grammar line and all lazy analysis-label cache slots;
3. resets definition failure/warning and status feedback;
4. clears the three-source index cache and sets its analysis sentinel invalid;
5. resets definition start/next cursors and page index to zero;
6. reuses the existing `Pager` and `Page` allocations;
7. loads page zero for the new item through the same switching SD reader;
8. requests one display update.

No `Pager`, `Page`, `Session`, `Shortlist`, string or vector allocation occurs
when changing words. The action is explicit dictionary I/O; ordinary reader
page turns remain I/O-free.

Direct word navigation is available when the current definition has no entry,
attachments are partial, one source entry is corrupt, or a status write failed.
The new word gets a fresh failure state and can recover normally.

## Status-save filtering

A successful status write stays on the current definition and sets the existing
saved feedback. Back continues to reapply canonical filtering before showing
the shortlist.

When the first Up/Down word action follows a successful status save, filtering
runs before selecting the destination. The current item is removed because its
primary canonical status is now suppressing. The direction is interpreted in
the **old** sorted list:

| Old position | Direction | Selection after removal |
| --- | --- | --- |
| middle `i` | Down | new item at `i` (the old successor) |
| middle `i` | Up | new item at `i-1` (the old predecessor) |
| first | Down | new first item |
| first | Up | new last item |
| last | Down | new first item |
| last | Up | new last item |

If filtering removes the only remaining item, finish the dictionary activity.
If filtering fails, keep the current selection, show/log a state failure, and do
not load another word. The user may still Back out. Filtering is not rerun on
subsequent word changes until another status is saved.

A cancelled status selection performs no write, does not filter, and returns to
the unchanged definition page.

## Status-mode behavior

Status mode deliberately keeps all four navigation directions within the three
status choices. Side Up/Down do not switch words while the status selector is
visible. The user must save or cancel first, which prevents an ambiguous pending
status from being applied to a different word.

Previous status wraps Learning <- Ignore; next status wraps Ignore -> Learning.
Confirm writes exactly one primary canonical state update. Back cancels.

## Position and page feedback

Definition mode exposes two independent positions:

- `current/total` in the header is the one-based sorted word position;
- the existing bottom-right page indicator is the one-based definition page and
  retains `+` while another page exists;
- the footer previews show the literal words reached by side Up and Down,
  including wrap destinations, without grammar or SD reads.

Changing definition pages updates only the page indicator. Changing words
updates the word indicator and both previews, then resets the page indicator to
page one. Neither indicator is persisted.

## Lifecycle and one-reader behavior

A word change does not close/reopen the activity or retain another file. The
existing `SwitchingFileReader` closes the current entries/index/headword source
before selecting the next path. Lazy canonical labels are loaded before render,
never during render.

A saved-status filter follows the existing order: close the shared source reader
before packed state reads/WAL operations. Definition loading resumes only after
state filtering completes. Attachment records and definition-source discovery
are session-wide and are not reread for every word.

## Required state-machine tests

Use a three-item sorted fixture `[A, B, C]` unless noted otherwise.

| Start/action | Expected selection/result |
| --- | --- |
| definition B + Up | A, page zero |
| definition B + Down | C, page zero |
| definition A + Up | C, page zero |
| definition C + Down | A, page zero |
| definition page 2 + Up | previous word, page zero |
| definition B missing + Down | C loads normally |
| partial source warning + Up | previous word starts with warning recomputed |
| one unsaved item + Up/Down | no-op; no source I/O |
| saved B + Down | filtered `[A,C]`, select C |
| saved B + Up | filtered `[A,C]`, select A |
| saved A + Up | filtered `[B,C]`, select C |
| saved C + Down | filtered `[A,B]`, select A |
| saved only item + Up/Down | activity finishes |
| saved item + filter failure | selection unchanged; explicit failure |
| status mode + Up/Down | status changes; word unchanged |
| simultaneous Back+Down | Back only |
| simultaneous Confirm+Down | Confirm only |
| page zero + Left | no-op |
| final page + Right | no-op |

U15 also injects every logical action through the simulator in portrait,
inverted and both landscape orientations. It verifies front remapping and side
orientation settings without asserting raw hardware indices.

## Hardware acceptance

On X3/X4 with three attached sources and the production Homma EPUB:

1. open a middle shortlist word;
2. use front Left/Right across at least one source boundary;
3. use side Up/Down through three sorted words and across both wrap boundaries;
4. save one status, then move in each direction and verify the destination;
5. repeat from a no-definition word and after a partial-source warning;
6. repeat in portrait and both landscape orientations.

Expected logs show one retained activity, explicit per-word definition I/O,
resource release only when leaving the activity, no `[DIN] Failed to open`, no
second handle, stable heap/largest block, and stack high-water above the C35
threshold.

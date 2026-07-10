# Timed Call-Trace + Timeline Viewer — Design

**Date:** 2026-07-10
**Status:** Approved (pending spec review)
**Builds on:** `2026-07-10-calltrace-visualizer-design.md` (the edge recorder,
`.xct` format, and HTML viewer this extends).

## Goal

Add a deeper "Timed" recording mode that captures the game's function calls as
an ordered event log, and a timeline scrubber + synapse-style animation in the
viewer that replays the program splintering outward from Entry over time.
Include record-time throttling of spammy functions and a curate-then-rerun
ignore-list loop so recordings stay small and readable.

## Decisions (from brainstorming)

| Question | Decision |
|---|---|
| Temporal model | Full per-call event log (not bucketed) |
| Compression | Deflate-compressed binary event stream (zlib; browser inflates natively) |
| Time axis | Call sequence number — the event's position in the log IS its timestamp; no per-event clock value is stored |
| Playback | Auto-reveal: the graph grows as edges first fire; pulses trace each call |
| Spam throttle | Adaptive per-edge auto-throttle, always on in Timed mode |
| Noise handling | File carries exact counts → viewer ranks noisiest functions → user curates an ignore-list → next run drops them |

## Component 1: Recorder (xemu, C)

Extends `xemu-calltrace.c` / `.h`.

### Recording modes ("depth")

A tri-state mode replaces the current boolean arm:
`CT_OFF`, `CT_EDGES` (today's dedup edges + counts), `CT_TIMED`
(edges + ordered event log). The translation-time guard stays
`xemu_calltrace_armed` (true for EDGES or TIMED). The record hot path branches
on the mode.

### Edge indices

The edge map assigns each unique `(call_site, callee)` a stable **index**
(0,1,2… in first-seen order) via a new `uint32_t index` field on `CTEdge`,
set to `num_entries` at insertion. Events reference edges by this index, and
the `.xct` writer emits edges in index order so `event value i → edges[i]`.

### Event log (TIMED mode)

An append-only growable buffer of `uint32_t` edge indices, one per recorded
call, in execution order. Growth is chunked (linked list of fixed 1 M-entry
`uint32_t` chunks) to avoid giant reallocs. A configurable cap
(`CT_EVENT_CAP`, default 20 M ≈ 80 MB live) stops appends and sets an
event-truncation flag.

### Adaptive auto-throttle (TIMED mode)

After incrementing an edge's count, append an event iff
`count <= CT_THROTTLE_FULL` (default 256) **or**
`count % CT_THROTTLE_EVERY == 0` (default 64). Hot edges are downsampled
~`CT_THROTTLE_EVERY`× past the threshold; the edge's total `count` stays
exact, so the noise report and timeline "still active" signal remain truthful.

### Ignore-list (both modes)

An optional set of **callee entry addresses** loaded before recording (open
hash set). In the record hot path, a call is dropped entirely — no count, no
edge, no event — when `callee ∈ ignore_set`. This removes inbound-heavy
pollers (their node and all calls into them disappear); a listed function's
own outbound calls are still governed by auto-throttle.

Loaded via a new `Debug → Call Trace → Load ignore list…` action (a text file
of hex addresses, one per line, `#`/`;` comments allowed). The parsed address
set persists in memory and is applied to every subsequent recording this
session until **Clear ignore list** is chosen or a new list is loaded (no new
config key; the set lives in the engine).

### UI (menubar.cc)

`Debug → Call Trace` when idle offers, in place of the single Start item:
- **Start — Edges**
- **Start — Timed (call timeline)**
- **Load ignore list…** / **Clear ignore list** (shows loaded count)

While recording it shows mode, unique-edge count, and (Timed) event count and
whether the event cap was hit. **Stop & Save** writes v1 for Edges, v2 for
Timed.

## Component 2: File format `.xct` v2

v2 is v1 plus a trailing events block; v1 files and Edges-mode saves are
unchanged. `version = 2` signals the block is present.

Appended after the existing edges array:

| Field | Type | Meaning |
|---|---|---|
| event_count | u64 | number of recorded events |
| event_flags | u32 | bit0 = event log truncated (cap hit) |
| throttle_full | u32 | `CT_THROTTLE_FULL` used (provenance) |
| throttle_every | u32 | `CT_THROTTLE_EVERY` used (provenance) |
| raw_bytes | u64 | uncompressed size = event_count × 4 |
| comp_bytes | u64 | compressed size of the blob that follows |
| blob | u8[comp_bytes] | zlib-deflated stream of `u32` edge indices |

Edges are written in index order in v2 (so `events[k]` indexes `edges[]`
directly). Compression uses zlib `compress2` (bundled with QEMU); the viewer
inflates with `DecompressionStream('deflate')`.

## Component 3: Viewer (`tools/calltrace/viewer.html`)

### Loading

`parseXCT` becomes async (loading already is): parse header/tables
synchronously, then if `version >= 2`, inflate the events blob into a
`Uint32Array` of edge indices. Decompression failure shows an error banner and
falls back to the static graph.

### Derived timeline data

- `edgeOf[i]` = the raw edge `{site, callee}` for index `i` (edges array).
- `edgeFn[i]` = `{callerId, calleeId}` via the same nearest-preceding-entry
  attribution `deriveModel` uses, so a pulse knows which function nodes it runs
  between.
- `firstFire[i]` = index of the first event that references edge `i`
  (single pass; `-1` if never — only when truncated).

### Timeline model

- `playhead` ∈ `[0, event_count]`.
- **Discovered set at P:** edges with `0 <= firstFire[i] <= P`. The visible
  graph is laid out from roots over the functions these edges connect. Scrub to
  any P → recompute in O(edges); no replay. Playing forward grows the graph.
- When `version < 2` (Edges-only), the timeline UI is hidden and the viewer
  behaves exactly as today (manual expand/collapse).

### Playback + animation

- A `requestAnimationFrame` loop advances `playhead` by `speed` events/frame
  while playing.
- Events crossed in a frame spawn **pulses**: a pulse rides its edge from
  caller to callee over ~0.4 s and fades. Concurrent pulses are capped
  (`MAX_PULSES`, ~200); beyond the cap, an event instead bumps its edge's
  transient glow, so hot regions light up without unbounded pulse objects.
- Muted/ignored functions never pulse and are hidden from the graph.
- Newly discovered edges reflow the layout; to keep it watchable, layout
  recompute is throttled to at most once per animation frame.

### Timeline controls (shown only for v2)

A bar under the toolbar: **Play/Pause**, a **scrub slider** over
`[0, event_count]`, a **speed** selector (1× / 10× / 100× / 1000× / 10000×
events per frame), and a readout `event P / N · last: <fnName>`.

### Noisiest Functions panel + ignore-list export

A toolbar **Noise** button renders this report into the right-hand details
pane (the same slot the Legend and node details use). It lists functions
ranked by **total inbound call count** (sum of `count` over edges whose callee
is that function), which is the true spam ranking even after throttling. Each
row: name, count, and a checkbox. Checked functions are
**muted** live (hidden from graph + playback) and included when **Export
ignore list** writes their addresses to `<name>.ignore.txt` — the file
`Load ignore list…` consumes in xemu for the next run.

## Error handling

- Event cap hit → event-truncation flag → viewer warns the timeline is partial.
- `firstFire[i] == -1` (edge counted but no surviving event, only under
  truncation) → edge shown as discovered at P = 0 so structure isn't lost, but
  it never pulses.
- Decompression / bad-version → error banner, static-graph fallback.
- Empty ignore file or malformed lines → skipped with a count, like symbol maps.

## Testing

- **Engine:** record Timed on a game, save; verify `event_count > 0`, the
  inflated stream decodes to in-range edge indices, and per-edge event
  occurrences ≤ `count` with equality below the throttle threshold. Verify an
  ignore-list run omits the listed callees entirely. Confirm Edges mode still
  writes byte-compatible v1.
- **Viewer:** a synthetic v2 fixture (extend `make_test_xct.py`) with a known
  event stream drives new selftests in the existing harness: inflate
  round-trip (build with `CompressionStream`), `firstFire` values,
  discovered-set membership at several P, throttle-count reconciliation, and
  the noise ranking order. Existing selftests must stay green.

## Non-goals (v1 of timed mode)

- Real wall-clock / guest-time axis (call-sequence only).
- Call-stack nesting / durations (no RET capture; that would be a further
  depth level).
- Per-event data beyond the edge (no arguments, registers, or thread id).
- Streaming huge logs from disk — the event stream is inflated fully into
  memory (bounded by the record-time cap).
- Editing the ignore-list inside xemu (curated in the viewer, loaded as a file).

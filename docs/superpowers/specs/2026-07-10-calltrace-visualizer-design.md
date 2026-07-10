# Call-Trace Recorder + External Graph Viewer — Design

**Date:** 2026-07-10
**Status:** Approved (pending spec review)

## Goal

Record the dynamic function call graph of a running Xbox game inside xemu, then
explore it as an interactive expand-from-Entry flow chart in an external
viewer. The purpose is understanding a game's structure, not gameplay
performance — though the design keeps recording overhead modest and idle
overhead at exactly zero.

## Decisions (from brainstorming)

| Question | Decision |
|---|---|
| Usage mode | Record, then explore (no live graph) |
| Trace scope | XBE code only; kernel calls appear as named leaf nodes; kernel→XBE calls recorded (reveals thread entry points / callbacks as roots) |
| Function naming | `sub_XXXXXXXX` by default + optional user symbol map file; kernel exports auto-named |
| Exploration model | Expand-from-Entry collapsible graph with pan/zoom/search |
| Persistence | Native binary recording file; DOT export from the viewer |
| Architecture | Approach B: tracing engine in xemu, viewing in an external bundled HTML file |
| Viewer tech | Single self-contained HTML file, vanilla JS + Canvas 2D, no dependencies, no build step. No GPU compute — the data is ~5–20k functions / ~100k edges, far below where acceleration matters |

## Component 1: Trace engine (xemu, C)

### Instrumentation

- A TCG helper (declared in `target/i386/helper.h`, implemented in a new
  xemu-specific source file) is called from the CALL translation sites in
  `target/i386/tcg/emit.c.inc`: `gen_CALL` (~line 1575), `gen_CALL_m`,
  `gen_CALLF`, `gen_CALLF_m`.
- **RET is not instrumented.** Caller attribution uses call-site addresses
  resolved to containing functions later (see Function derivation), so no
  shadow stack is needed. This halves hot-path cost and is immune to guest
  thread switches, setjmp/longjmp, and exceptions.
- Helper arguments: call-site EIP (address of the CALL instruction) and callee
  EIP (branch target). For direct calls the target is the decoded immediate;
  for indirect calls it is the runtime value already in the translator's
  target temporary.

### Recorded data

- Open-addressing hash map, power-of-two sized, keyed by the packed 64-bit
  pair `(call_site << 32) | callee`, value = 64-bit hit counter.
- Scope filter in the helper: record iff call_site or callee is below
  0x80000000 (XBE space). Kernel-internal calls (both ≥ 0x80000000) are
  skipped.
- Capacity cap ~1M edges: past the cap, existing edges keep counting, new
  edges are dropped, and a truncation flag is set (surfaced in the saved file
  and a toast).

### Arming / disarming

- Global `calltrace_enabled` flag checked at **translation time**: the helper
  call is only emitted into TCG output while recording is armed.
- Toggling Record/Stop flushes the translation cache (`tb_flush`) so all guest
  code retranslates with/without instrumentation. Idle builds execute code
  byte-identical to today's.
- Xbox has a single vCPU thread, so the hot path needs no locks. The UI thread
  reads an approximate edge count for status display only; save happens after
  recording stops.

### UI controls (the only in-emulator UI)

- `Debug → Call Trace` submenu in `ui/xui/menubar.cc`: **Start Recording**,
  **Stop & Save**, and a status line (edge/function count while armed).
- Start is disabled when no XBE is running.
- Stop & Save writes `<TitleName>-<timestamp>.xct` to an output directory
  stored in xemu's config system (`g_config`, new key; defaults to the
  existing xemu data/screenshot directory) and fires a toast notification with
  the path (xemu's existing notification system).

## Component 2: Recording file format (`.xct`)

Compact binary, little-endian, written once at save time:

| Block | Contents |
|---|---|
| Header | magic `XCTR`, format version, truncation flag, XBE title ID, title name, base address, entry point, edge/section/import counts |
| Sections | XBE section table: name, virtual start, virtual size |
| Kernel imports | kernel address → export name (resolved via the XBE kernel thunk table and a static ordinal→name table of the ~379 documented Xbox kernel exports, compiled into xemu), string-table encoded |
| Edges | flat array of `{u32 call_site, u32 callee, u64 count}` — 16 bytes/edge |

XBE metadata comes from the existing `xemu-xbe.c` guest-memory reader
(`xemu_get_xbe_info`, `virt_dma_memory_read`).

## Component 3: Viewer (`tools/calltrace/viewer.html`)

One self-contained HTML file. Open from disk in any browser; drag-and-drop (or
file-pick) a `.xct`.

### Function derivation

- Function entry set = all distinct callee addresses in the file + the XBE
  entry point.
- A call site belongs to the function with the greatest entry ≤ call site
  (nearest-preceding-entry rule). Edges are then merged from call-site
  granularity to function→function granularity, preserving per-call-site
  detail for the details panel.

### Graph view

- Initial view: the Entry node, plus kernel-called roots (thread entries,
  callbacks) collapsed under a "Roots" group.
- Click a node to expand/collapse its callees. Layout is a left-to-right
  layered tree, reflowed on expand. Calls to already-visible functions render
  as distinct curved cross-links (makes recursion and shared helpers visible).
- Pan by drag, zoom by wheel. Edge thickness/label reflects call count.
- Node labels: symbol name if mapped, else `sub_XXXXXXXX`; tinted by XBE
  section. Kernel leaves show the export name in a distinct style.
- Details panel for the selected node: address, section, callers, callees,
  per-edge call sites and counts. Call sites observed reaching multiple
  distinct callees are flagged as indirect/polymorphic (likely vtable
  dispatch or callback invocation).
- Search by name or address; auto-expands the path from a root to the match.

### Symbol maps

- Load a plain-text file of `address name` lines (hex address, whitespace,
  name). Malformed lines are skipped and counted.
- The viewer documents one-liner export scripts for IDA and Ghidra inline.

### Export

- Button to export Graphviz `.dot` — full graph or currently-expanded subset.

## Error handling

- No XBE running → Start Recording disabled.
- Edge cap reached → keep counting existing edges, drop new ones, flag
  truncation in file + toast.
- Save I/O failure → error toast; in-memory data retained for retry.
- Viewer: bad magic/version/truncated file → error banner; bad symbol-map
  lines skipped with a visible count.

## Testing

- **Engine:** boot a game, record ~30 s, save. Verify the file parses and edge
  counts are sane. Spot-check 3–5 recorded edges against the same XBE
  disassembled in IDA (ground truth). Confirm zero helper emission when
  disarmed.
- **Viewer:** load the real recording — Entry expands, kernel leaves named,
  search works, DOT export opens in Graphviz. A tiny hand-built synthetic
  `.xct` fixture validates layout and the nearest-preceding-entry attribution.

## Non-goals (v1)

- Live/streaming graph while the game runs.
- Tracing inside kernel code.
- RET/temporal data (flame graphs, call trees over time).
- Whole-graph force-directed layout; GPU acceleration of any kind.
- Loading recordings back into xemu.

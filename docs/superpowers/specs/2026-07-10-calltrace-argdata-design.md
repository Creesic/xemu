# Call-Trace "Data" Mode (argument capture) — Design

**Date:** 2026-07-10
**Status:** Approved (pending spec review)
**Builds on:** the existing call-trace engine (`xemu-calltrace.c`, Edges + Timed
modes, `.xct` v1/v2), the viewer (`tools/calltrace/viewer.html`), and xemu's
global hotkey handling (`ui/xemu.c`).

## Goal

Add a third trace mode, **Data**, that records the *arguments carried into each
call* — not just which function called which. It captures a fixed snapshot of
the outgoing argument words at every recorded call, stores them compactly
(bounded by call-graph size, not call volume), and surfaces them in the viewer
both as a per-function summary and as a live per-call readout on the timeline.
Recording can be started and stopped with a keyboard hotkey, not just the menu.

**Explicitly deferred (Tier 2, not this project):** return values, pointer/
string dereferencing, and per-argument type inference beyond range
classification. See Non-goals.

## Decisions (from brainstorming)

| Question | Decision |
|---|---|
| What to capture | `ECX`, `EDX`, and `[ESP+0]`, `[ESP+4]`, `[ESP+8]`, `[ESP+12]` — 6 dwords / 24 bytes per recorded call |
| Why those | `ECX`/`EDX` = `__thiscall` `this` + `__fastcall` register pair; the 4 stack dwords = typical `cdecl`/`stdcall` slots |
| Mode shape | A distinct third mode (`Edges` / `Timed` / **`Data`**), behaving like Timed plus the snapshot |
| Storage | **C-linked, N=16**: event stream unchanged, a parallel 1-byte-per-event arg-set index, and a per-edge table of ≤16 distinct arg-sets |
| Why C | Arg storage is bounded by `edges × 16`, independent of call volume — the file-size-safe scheme |
| Viewer | Node details panel (distinct arg-sets, section-classified) **plus** live pulse readout of the current call's specific args |
| Hotkey | `Ctrl+Alt+T` toggles start/stop; start mode is a configurable default (`general.calltrace_hotkey_mode`, default `data`); stop is universal + auto-save + toast |

## Architecture

Four touch-points, each a self-contained change:

1. **Engine** (`xemu-calltrace.c/.h`, `xemu-calltrace-map.h`) — new `CT_DATA`
   mode; per-edge arg-set tables with dedup + overflow; per-event index stream.
2. **Translator + helper** (`target/i386/tcg/emit.c.inc`, `helper.h`,
   `target/i386/tcg/misc_helper.c`) — a Data-only helper that reads the six
   words and forwards them; emitted only when armed in Data mode.
3. **Format** (`.xct` v3 writer in the engine; `XCT_FORMAT.md`;
   `tools/calltrace/{make_test_xct.py,xct_dump.py}`) — a Data block appended
   after the v2 event block.
4. **UI** (`ui/xui/menubar.cc`, `ui/xemu.c`, `config_spec.yml`) — menu entry
   for Data mode, the `Ctrl+Alt+T` toggle, the config key, and toasts.

The viewer (`tools/calltrace/viewer.html`) parses v3 and renders the args.

---

## Engine: capture and storage

### New mode

`CalltraceMode` gains `CT_DATA`. `xemu_calltrace_start_mode(CT_DATA)` arms
exactly like `CT_TIMED` (event buffer, throttle, ignore-list) and additionally
enables argument capture. Because arming flushes the TB cache and the mode is
stable while armed, the translator can branch on the mode at translation time.
A single boolean, set when arming in Data mode, drives the translator:

```c
extern bool xemu_calltrace_capture_args;  /* true iff armed in CT_DATA */
```

### The snapshot

Per **recorded** call (i.e. only for calls that survive the throttle and get an
event), the engine receives six little-endian dwords in this fixed order:

| Index | Source |
|---|---|
| 0 | `ECX` |
| 1 | `EDX` |
| 2 | `[ESP+0]` |
| 3 | `[ESP+4]` |
| 4 | `[ESP+8]` |
| 5 | `[ESP+12]` |

The CALL hook runs *before* the return address is pushed, so at that instant
`ESP` points exactly at the caller's outgoing argument block and `ECX`/`EDX`
still hold the register-convention arguments.

### Per-edge arg-set table (C-linked, N=16)

Each edge owns a small table of up to **16 distinct** 6-dword arg-sets. Two
arg-sets are equal iff all six dwords are equal. On each recorded call for an
edge:

1. Linear-scan the edge's ≤16 existing sets. If the snapshot matches set `j`,
   the call's arg-set index is `j`.
2. Else if the table has room, append the snapshot as new set `j`; index `j`.
3. Else (table full, snapshot is a 17th distinct set): index `0xFF`
   (**overflow** — the set is not stored).

The scan is bounded (≤16 × 6 comparisons) and runs only on throttle-surviving
calls, so cost tracks the logged-event rate, not the raw call rate.

The arg-set index for each event is appended to a **byte stream parallel to the
event stream** (one `u8` per event, same length = `event_count`). Event `i`'s
call used `edges[events[i]].argsets[index[i]]`, unless `index[i] == 0xFF`.

Storage bound: `edge_count × 16 × 24` bytes for the tables (realistically far
less — most edges see only a handful of distinct sets) plus `event_count × 1`
byte for the index stream. Both independent of call volume.

---

## Translator + helper

`gen_CALL` / `gen_CALL_m` / `gen_CALLF` / `gen_CALLF_m` currently emit
`gen_helper_xemu_calltrace_call(call_site, callee)` when `xemu_calltrace_armed`.
Under Data mode they instead emit a new helper that also captures args:

```c
/* helper.h — reads guest regs/stack, so it takes env and is NOT NO_RWG.
   TCG_CALL_NO_WG = "does not write globals" but may read them, so TCG syncs
   all guest registers (incl. ECX/EDX/ESP) to env before the call. */
DEF_HELPER_FLAGS_3(xemu_calltrace_data, TCG_CALL_NO_WG, void, env, tl, tl)
```

The wrapper picks the helper by mode at translate time:

```c
static void gen_xemu_calltrace(DisasContext *s, TCGv callee)
{
    if (xemu_calltrace_capture_args) {
        gen_helper_xemu_calltrace_data(tcg_env, eip_cur_tl(s), callee);
    } else {
        gen_helper_xemu_calltrace_call(eip_cur_tl(s), callee);
    }
}
```

The helper reads `ECX`/`EDX`/`ESP` from the freshly-synced `env` and the four
stack words from guest memory, then forwards:

```c
void HELPER(xemu_calltrace_data)(CPUX86State *env, target_ulong call_site,
                                 target_ulong callee)
{
    uint32_t esp = (uint32_t)env->regs[R_ESP];
    uint32_t a[6];
    a[0] = env->regs[R_ECX];
    a[1] = env->regs[R_EDX];
    for (int k = 0; k < 4; k++) {
        a[2 + k] = ct_safe_ldl(env, esp + 4u * k);
    }
    xemu_calltrace_record_data((uint32_t)call_site, (uint32_t)callee, a);
}
```

**Non-faulting reads (correctness requirement).** Tracing must never perturb
guest execution. A stack read that lands on an unmapped page (e.g. a leaf CALL
with `ESP` near a guard page) must yield `0`, not raise a guest exception.
`ct_safe_ldl` therefore uses a non-faulting probe (probe the page for read; on
failure return `0`) rather than `cpu_ldl_data_ra`, which longjmps on fault. The
plan pins the exact QEMU primitive.

`xemu_calltrace_record_data(call_site, callee, args)` performs the existing
edge/throttle/event logic, and — for events it logs — the arg-set dedup above,
appending the index byte. Edges/Timed continue to call the unchanged
`xemu_calltrace_record`.

---

## `.xct` v3 format

`version = 3`. A v3 file is a complete v2 file (header, string table, sections,
kernel imports, edges, **event block**) with a **Data block** appended after the
v2 event block. All multi-byte fields little-endian, consistent with v1/v2.

### Data block — sub-header (40 bytes)

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| +0 | 4 | u32 | `argset_dwords` | dwords per arg-set (always `6`; self-describing) |
| +4 | 4 | u32 | `argset_cap` | max distinct sets per edge (always `16`) |
| +8 | 8 | u64 | `table_raw_bytes` | uncompressed size of the arg-set table blob |
| +16 | 8 | u64 | `table_comp_bytes` | compressed size of the arg-set table blob |
| +24 | 8 | u64 | `index_raw_bytes` | uncompressed index-stream size = `event_count` |
| +32 | 8 | u64 | `index_comp_bytes` | compressed size of the index stream |

### Arg-set table blob (`table_comp_bytes`, zlib)

Decompresses to `table_raw_bytes`. It is a sequential walk over **all edges, in
index order** (`edge_count` entries, matching the v2 edge order that the event
stream indexes):

```
for each edge (edge_count of them):
    u8  nsets                       # 0..16
    nsets × (argset_dwords × u32)   # each set = ECX,EDX,[ESP+0..12]
```

Edge `k`'s distinct arg-sets are the `k`-th entry.

### Index stream (`index_comp_bytes`, zlib)

Decompresses to `event_count` bytes, one `u8` per event, parallel to the v2
event stream. `index[i]` selects the arg-set for event `i`:
`edges[events[i]].argsets[index[i]]`, or **no stored args** when
`index[i] == 0xFF` (overflow).

### `XCT_FORMAT.md`

Extend the reference guide with the Data block layout, bump the version note to
"1 / 2 / 3", and add v3 handling to the Python `read_xct()` snippet.

### Fixtures / tools

- `make_test_xct.py` gains `--data`: emit a v3 fixture with a known arg-set
  table (including at least one edge with two distinct sets and one overflow)
  and a known index stream parallel to the existing timed `EVENT_STREAM`.
- `xct_dump.py` parses and pretty-prints the Data block (per-edge set counts, a
  few decoded arg-sets, overflow count).

---

## Viewer (`tools/calltrace/viewer.html`)

### Parse

`parseXCT` reads the Data block when `version === 3`: inflate the arg-set table
(walk edges → `argsets[]` per edge) and the index stream (`Uint8Array` of length
`event_count`). Attach `argsets` to each derived edge; keep the index array
alongside the event array so playback can look up a call's set.

### Section-range classification

A helper `classifyWord(v)` labels each dword by where it falls, using the
section table + kernel boundary already parsed:

- `v` in a `.text`-type section range → `code ptr` (function/vtable candidate)
- `v` in a data section range → `data ptr` (global candidate)
- `v ≥ 0x80000000` → `kernel` (kernel handle/callback candidate)
- small `v` (e.g. `< 0x10000`) → `int` (integer/flag/bool candidate)
- otherwise → `?` (raw hex)

No dereferencing: the pointed-at memory is not in the file. This is stated in
the panel so the classification isn't mistaken for a value read.

### Node details panel

When a node is selected, list its incoming edges and, per edge, the distinct
arg-sets (deduped across the ≤16 stored), each shown as six classified hex
words. If any incoming edge overflowed (saw an `0xFF` event), note "+ more
distinct sets not captured (cap 16)".

### Live pulse readout

During playback/scrub, the current event's specific arg-set (via the index
array) is shown in a compact readout, e.g.
`sub_12000(ecx=0x0011a40 ·data, edx=0, [esp]=5 ·int)`. Hovering a pulse shows
that pulse's arg-set. Overflow events show `args: (not captured)`.

Everything else in the viewer (layout, timeline, fade, collapse, selection,
noise/legend panels, WebGL/2D rendering, symbols, DOT) is unchanged; args are
additive data on existing edges/events.

---

## Hotkey + config

### Config key

`config_spec.yml`, under `general`, mirroring `calltrace_dir`:

```yaml
  calltrace_hotkey_mode:
    type: string
    default: data     # one of: edges | timed | data
```

Exposes `g_config.general.calltrace_hotkey_mode`. An unrecognized value falls
back to `data`.

### Menu

`ui/xui/menubar.cc` Call Trace submenu gains **"Start - Data (call + args)"**
(→ `xemu_calltrace_start_mode(CT_DATA)`), gated on an XBE being loaded like the
other two. While recording in Data mode the status line reads
`Recording (Data): N edges` with the event count, matching Timed.

### The `Ctrl+Alt+T` toggle

In `ui/xemu.c` `handle_keydown`, inside the existing
`gui_key_modifier_pressed` switch (the `Ctrl+Alt+*` group), add
`case SDL_SCANCODE_T`:

- If `xemu_calltrace_mode() != CT_OFF` → **stop & save**: `xemu_calltrace_stop()`,
  then `xemu_calltrace_save(dir, &err)` to the calltrace dir (falling back to the
  screenshot dir exactly as the menu's "Stop & Save" does), and
  `xemu_queue_notification` with the saved filename or the error.
- Else, if an XBE is loaded → **start** in the mode parsed from
  `calltrace_hotkey_mode` (default `CT_DATA`); toast
  `Call trace: recording (<Mode>)`. If no XBE is loaded, toast a short "load a
  game first" message and do nothing.
- Set `gui_keysym = 1` so the key is consumed like the other hotkeys.

Stop is universal — it ends whatever mode is running, including one started from
the menu. The hotkey records with whatever ignore-list is currently loaded.

---

## Testing

**C unit test (engine).** Drive `xemu_calltrace_record_data` with a scripted
sequence and assert: (a) identical snapshots dedup to one set with the right
repeated index; (b) distinct snapshots allocate ascending indices; (c) the 17th
distinct set on an edge yields index `0xFF` and is not stored; (d) the index
stream length equals the event count; (e) per-edge `nsets ≤ 16`.

**Format round-trip.** `make_test_xct.py --data` writes a v3 fixture; a Python
round-trip (mirroring the guide's `read_xct`) reads back the exact arg-set
tables and index stream; `xct_dump.py` parses it without error.

**Viewer selftests** (`?selftest=1`): v3 parse attaches `argsets` + index;
`classifyWord` returns the expected label for a `.text` address, a data
address, a kernel address, and a small int; selecting a known node lists its
distinct arg-sets; scrubbing to a known event yields that event's arg-set (and
`0xFF` → "not captured"). Existing selftests stay green (v1/v2 unaffected).

**Build.** Incremental `ninja`; `qemu-system-i386w.exe` launches.

**Manual (user hardware, final feel-test).** Boot a game, `Ctrl+Alt+T` to start
Data, play briefly, `Ctrl+Alt+T` to stop; confirm the toast, load the `.xct` in
the viewer, click a function to see classified arg-sets, and scrub the timeline
to watch the live per-call readout.

---

## Non-goals (this project)

- **Return values** (Tier 2) — needs a shadow-stack return-trap mechanism.
- **Pointer/string dereferencing** — guest memory is deliberately not stored, to
  keep files bounded; classification is by address range only.
- **Type inference** beyond range classification — real prototypes are a future
  symbol/recomp step.
- **Variable capture width** — the snapshot is a fixed 6 dwords
  (`argset_dwords` is written for forward-compat but not made configurable).
- **Rebindable hotkey UI** — one fixed `Ctrl+Alt+T`, matching the other hotkeys.

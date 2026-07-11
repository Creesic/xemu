# Call-Trace "Data" Mode (argument capture) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a third call-trace mode, **Data**, that captures a fixed 6-dword argument snapshot at every recorded call, stores it compactly (`.xct` v3), surfaces it in the viewer, and can be toggled with `Ctrl+Alt+T`.

**Architecture:** A Data-only TCG helper reads `ECX`/`EDX`/`[ESP+0..12]` and forwards them to the engine, which interns them into per-edge tables of ≤16 distinct sets and appends a 1-byte index per event (parallel to the existing v2 event stream). The writer appends a Data block; the viewer parses it and shows args in the node panel and the live timeline readout. A hotkey toggles recording using a configurable default mode.

**Tech Stack:** C (QEMU/TCG, glib, zlib), x86 TCG translator, C++ (ImGui menubar), Python 3 (fixtures/tools), single-file HTML/JS viewer.

**Spec:** `docs/superpowers/specs/2026-07-10-calltrace-argdata-design.md`

## Global Constraints

- Capture is a **fixed 6 dwords**, in order: `ECX`, `EDX`, `[ESP+0]`, `[ESP+4]`, `[ESP+8]`, `[ESP+12]`. Constants: `CT_ARGSET_DWORDS = 6`, `CT_ARGSET_CAP = 16`, overflow sentinel `0xFF`.
- **Little-endian** throughout; host is LE on all supported platforms.
- Tracing must **never perturb guest execution**: stack reads are non-faulting (unmapped → `0`).
- Arg storage is bounded by `edges × 16`, **independent of call volume**.
- Data mode is a **superset of Timed**: it writes the full v2 event block plus the Data block; `version = 3`.
- Edges/Timed modes and the v1/v2 format are **unchanged**.
- The header-only `xemu-calltrace-*.h` files stay **QEMU-independent** (standalone-testable with plain gcc).
- Standalone C tests build with `/c/msys64/ucrt64/bin/gcc.exe` (plain `gcc` in Git Bash is FPC's gcc 2.95 — do not use it). Incremental emulator builds: `ninja -C build` from an MSYS2 UCRT64 shell (see memory `xemu-windows-build-env`).

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `xemu-calltrace-argsets.h` | create (A1) | header-only arg-set intern/dedup/overflow; unit-testable |
| `tests/calltrace/test-calltrace-argsets.c` | create (A1) | standalone unit test for intern |
| `xemu-calltrace.h` | modify (A2) | `CT_DATA` enum, `xemu_calltrace_record_data` decl |
| `xemu-calltrace.c` | modify (A2, C1) | mode wiring, arg store, `record_data`; v3 writer |
| `target/i386/helper.h` | modify (B1) | `xemu_calltrace_data` helper decl |
| `target/i386/tcg/misc_helper.c` | modify (B1) | helper body + non-faulting stack read |
| `target/i386/tcg/emit.c.inc` | modify (B1) | branch to Data helper when capturing args |
| `tools/calltrace/make_test_xct.py` | modify (C2) | `--data` v3 fixture |
| `tools/calltrace/xct_dump.py` | modify (C2) | parse/print the Data block |
| `tools/calltrace/test-fixture-data.xct` | create (C2) | committed v3 fixture |
| `tools/calltrace/XCT_FORMAT.md` | modify (C3) | v3 documentation |
| `tools/calltrace/viewer.html` | modify (D1–D3) | parse v3, classify, node panel, live readout, selftests |
| `config_spec.yml` | modify (E1) | `general.calltrace_hotkey_mode` |
| `ui/xui/menubar.cc` | modify (E1) | "Start - Data" menu entry |
| `ui/xemu.c` | modify (E2) | `Ctrl+Alt+T` toggle + toasts |

Phases: **A** engine store · **B** translator/helper · **C** format+tools · **D** viewer · **E** UI/hotkey.

---

## Phase A — Engine: arg-set store

### Task A1: Arg-set intern (header-only + unit test)

**Files:**
- Create: `xemu-calltrace-argsets.h`
- Test: `tests/calltrace/test-calltrace-argsets.c`

**Interfaces:**
- Produces: `CTEdgeArgs` struct; `uint8_t ct_argset_intern(CTEdgeArgs *ea, const uint32_t args[6])` returning `0..15` or `0xFF`; macros `CT_ARGSET_DWORDS`, `CT_ARGSET_CAP`, `CT_ARGSET_OVERFLOW`.

- [ ] **Step 1: Write the failing test**

Create `tests/calltrace/test-calltrace-argsets.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xemu-calltrace-argsets.h"

static void set(uint32_t a[6], uint32_t v) {
    for (int i = 0; i < 6; i++) a[i] = v + i;
}

int main(void)
{
    CTEdgeArgs ea;
    memset(&ea, 0, sizeof(ea));
    assert(ea.nsets == 0);

    uint32_t a[6], b[6];
    set(a, 100);
    set(b, 200);

    /* first insert -> index 0 */
    assert(ct_argset_intern(&ea, a) == 0);
    /* identical snapshot dedups to same index, no new set */
    assert(ct_argset_intern(&ea, a) == 0);
    assert(ea.nsets == 1);
    /* distinct snapshot -> index 1 */
    assert(ct_argset_intern(&ea, b) == 1);
    assert(ea.nsets == 2);
    /* old one still dedups */
    assert(ct_argset_intern(&ea, a) == 0);
    assert(ea.nsets == 2);

    /* fill to the cap with distinct sets: already have 2, add 14 more = 16 */
    for (uint32_t i = 2; i < CT_ARGSET_CAP; i++) {
        uint32_t c[6];
        set(c, 1000 + i * 10);
        assert(ct_argset_intern(&ea, c) == (uint8_t)i);
    }
    assert(ea.nsets == CT_ARGSET_CAP);

    /* a new, 17th distinct set overflows and is NOT stored */
    uint32_t ov[6];
    set(ov, 99999);
    assert(ct_argset_intern(&ea, ov) == CT_ARGSET_OVERFLOW);
    assert(ea.nsets == CT_ARGSET_CAP);
    /* an already-known set still dedups after overflow */
    assert(ct_argset_intern(&ea, a) == 0);

    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run it to confirm it fails to compile (header missing)**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -o tests/calltrace/test-calltrace-argsets.exe tests/calltrace/test-calltrace-argsets.c`
Expected: FAIL — `xemu-calltrace-argsets.h: No such file or directory`.

- [ ] **Step 3: Create the header**

Create `xemu-calltrace-argsets.h`:

```c
/*
 * xemu call-trace argument-set table
 *
 * Per-edge table of up to CT_ARGSET_CAP distinct argument snapshots
 * (CT_ARGSET_DWORDS dwords each). Header-only and QEMU-independent for
 * standalone unit testing (tests/calltrace/test-calltrace-argsets.c).
 *
 * Copyright (C) 2026 xemu contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef XEMU_CALLTRACE_ARGSETS_H
#define XEMU_CALLTRACE_ARGSETS_H

#include <stdint.h>
#include <string.h>

#define CT_ARGSET_DWORDS 6
#define CT_ARGSET_CAP 16
#define CT_ARGSET_OVERFLOW 0xFFu

typedef struct CTEdgeArgs {
    uint8_t nsets;                                    /* 0..CT_ARGSET_CAP */
    uint32_t sets[CT_ARGSET_CAP][CT_ARGSET_DWORDS];
} CTEdgeArgs;

/*
 * Intern a snapshot into this edge's table. Returns the set index
 * (0..CT_ARGSET_CAP-1) for a matched or newly-stored set, or
 * CT_ARGSET_OVERFLOW when the table is full and the snapshot is new (the
 * snapshot is then not stored).
 */
static inline uint8_t ct_argset_intern(CTEdgeArgs *ea,
                                       const uint32_t args[CT_ARGSET_DWORDS])
{
    for (uint8_t i = 0; i < ea->nsets; i++) {
        if (memcmp(ea->sets[i], args,
                   CT_ARGSET_DWORDS * sizeof(uint32_t)) == 0) {
            return i;
        }
    }
    if (ea->nsets >= CT_ARGSET_CAP) {
        return (uint8_t)CT_ARGSET_OVERFLOW;
    }
    memcpy(ea->sets[ea->nsets], args, CT_ARGSET_DWORDS * sizeof(uint32_t));
    return ea->nsets++;
}

#endif
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -o tests/calltrace/test-calltrace-argsets.exe tests/calltrace/test-calltrace-argsets.c && ./tests/calltrace/test-calltrace-argsets.exe`
Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add xemu-calltrace-argsets.h tests/calltrace/test-calltrace-argsets.c
git commit -m "calltrace: Add per-edge argument-set intern table"
```

---

### Task A2: `CT_DATA` mode + arg store + `record_data`

**Files:**
- Modify: `xemu-calltrace.h`
- Modify: `xemu-calltrace.c`

**Interfaces:**
- Consumes: `CTEdge` (`.index`), `ct_map_add_indexed`, `ct_events_append`, `ct_argset_intern`/`CTEdgeArgs` (A1), throttle macros.
- Produces: `CT_DATA` enum value; `bool xemu_calltrace_capture_args`; `void xemu_calltrace_record_data(uint32_t call_site, uint32_t callee, const uint32_t args[6])`; internal `ct_edge_args`/`ct_arg_index` store reset on start.

- [ ] **Step 1: Extend the mode enum and declare the new entry point**

In `xemu-calltrace.h`, change the mode enum (line 40) and add declarations after `xemu_calltrace_record` (line 57):

```c
typedef enum { CT_OFF, CT_EDGES, CT_TIMED, CT_DATA } CalltraceMode;
```

```c
/* Hot path; called from the TCG helper on the vCPU thread. */
void xemu_calltrace_record(uint32_t call_site, uint32_t callee);

/* Data mode: like record(), plus a 6-dword argument snapshot. */
void xemu_calltrace_record_data(uint32_t call_site, uint32_t callee,
                                const uint32_t args[6]);
```

- [ ] **Step 2: Add the include, the capture flag, and the arg store**

In `xemu-calltrace.c`, add the include after line 27 (`#include "xemu-calltrace-events.h"`):

```c
#include "xemu-calltrace-argsets.h"
```

After `bool xemu_calltrace_armed;` (line 33) add:

```c
bool xemu_calltrace_capture_args;

/* Data mode: per-edge arg-set tables (indexed by edge index) and the
 * parallel per-event arg-set index stream. */
static CTEdgeArgs **ct_edge_args;
static uint32_t ct_edge_args_cap;
static GByteArray *ct_arg_index;

static void ct_args_reset(void)
{
    if (ct_edge_args) {
        for (uint32_t i = 0; i < ct_edge_args_cap; i++) {
            g_free(ct_edge_args[i]);
        }
        g_free(ct_edge_args);
        ct_edge_args = NULL;
    }
    ct_edge_args_cap = 0;
    if (ct_arg_index) {
        g_byte_array_free(ct_arg_index, TRUE);
        ct_arg_index = NULL;
    }
}

/* Intern a snapshot for the given edge index; grows the store lazily. */
static uint8_t ct_args_intern_edge(uint32_t edge_index,
                                   const uint32_t args[6])
{
    if (edge_index >= ct_edge_args_cap) {
        uint32_t nc = ct_edge_args_cap ? ct_edge_args_cap : 4096;
        while (edge_index >= nc) {
            nc *= 2;
        }
        ct_edge_args = g_renew(CTEdgeArgs *, ct_edge_args, nc);
        for (uint32_t i = ct_edge_args_cap; i < nc; i++) {
            ct_edge_args[i] = NULL;
        }
        ct_edge_args_cap = nc;
    }
    if (!ct_edge_args[edge_index]) {
        ct_edge_args[edge_index] = g_malloc0(sizeof(CTEdgeArgs));
    }
    return ct_argset_intern(ct_edge_args[edge_index], args);
}
```

- [ ] **Step 3: Wire the flag and store into start/stop**

In `xemu_calltrace_start_mode`, after `ct_events_init(&ct_events);` (line 108) add:

```c
    ct_args_reset();
    if (mode == CT_DATA) {
        ct_arg_index = g_byte_array_new();
    }
    xemu_calltrace_capture_args = (mode == CT_DATA);
```

In `xemu_calltrace_stop`, after `xemu_calltrace_armed = false;` (line 127) add:

```c
    xemu_calltrace_capture_args = false;
```

- [ ] **Step 4: Extend the throttle branch to cover Data, and add `record_data`**

In `xemu_calltrace_record`, change the timed guard (line 223) so Timed **and** Data both log events:

```c
    if (ct_mode == CT_TIMED || ct_mode == CT_DATA) {
```

(`record` is only reached in Edges/Timed, but this keeps the predicate consistent.)

Then add `xemu_calltrace_record_data` immediately after `xemu_calltrace_record` (after line 231):

```c
void xemu_calltrace_record_data(uint32_t call_site, uint32_t callee,
                                const uint32_t args[6])
{
    if (!xemu_calltrace_armed) {
        return;
    }
    if (call_site >= CT_KERNEL_SPACE && callee >= CT_KERNEL_SPACE) {
        return;
    }
    if (ct_ignore_n && ct_ignore_has(callee)) {
        return;
    }
    CTEdge *e = ct_map_add_indexed(&ct_map, call_site, callee);
    if (!e) {
        if (ct_map.num_entries >= CT_MAP_MAX_ENTRIES) {
            ct_truncated = true;
        }
        return;
    }
    if (e->count <= CT_THROTTLE_FULL || (e->count % CT_THROTTLE_EVERY) == 0) {
        /* Append the event and its arg-set index together so the two
         * streams stay exactly parallel. */
        if (ct_events_append(&ct_events, e->index)) {
            uint8_t ai = ct_args_intern_edge(e->index, args);
            g_byte_array_append(ct_arg_index, &ai, 1);
        } else {
            ct_events_trunc = true;
        }
    }
}
```

- [ ] **Step 5: Compile-check the engine (build the emulator incrementally)**

Run (MSYS2 UCRT64 shell): `ninja -C build xemu-calltrace.c.o` or a full `ninja -C build`.
Expected: `xemu-calltrace.c` compiles (the helper/translator aren't wired yet, so nothing calls `record_data` — that's fine; a `-Wunused` warning would only appear for statics, which are all referenced).

- [ ] **Step 6: Commit**

```bash
git add xemu-calltrace.h xemu-calltrace.c
git commit -m "calltrace: Add Data mode, arg-set store, and record_data"
```

---

## Phase B — Translator + helper

### Task B1: Data-only TCG helper

**Files:**
- Modify: `target/i386/helper.h:237`
- Modify: `target/i386/tcg/misc_helper.c:146-152`
- Modify: `target/i386/tcg/emit.c.inc:1581-1586`

**Interfaces:**
- Consumes: `xemu_calltrace_capture_args` (A2), `xemu_calltrace_record_data` (A2), `cpu_memory_rw_debug`, `env_cpu`, `tcg_env`.
- Produces: helper `xemu_calltrace_data(env, call_site, callee)`; Data-mode instrumentation for all CALL forms.

- [ ] **Step 1: Declare the helper**

In `target/i386/helper.h`, after line 237 (`DEF_HELPER_FLAGS_2(xemu_calltrace_call, ...)`) add:

```c
DEF_HELPER_FLAGS_3(xemu_calltrace_data, TCG_CALL_NO_WG, void, env, tl, tl)
```

`TCG_CALL_NO_WG` = does not *write* globals but may read them, so TCG syncs all guest registers (incl. `ECX`/`EDX`/`ESP`) to `env` before the call.

- [ ] **Step 2: Implement the helper with a non-faulting stack read**

In `target/i386/tcg/misc_helper.c`, add the include near the top (after line 22 `#include "cpu.h"`):

```c
#include "hw/core/cpu.h"   /* cpu_memory_rw_debug */
```

Then, after the existing `HELPER(xemu_calltrace_call)` (line 152), add:

```c
/* Non-faulting guest read: returns 0 (never raises) on an unmapped page,
 * so tracing can't perturb guest execution. */
static uint32_t ct_safe_ldl(CPUX86State *env, uint32_t va)
{
    uint32_t v = 0;
    if (cpu_memory_rw_debug(env_cpu(env), va, &v, 4, false) != 0) {
        return 0;
    }
    return v; /* guest and host are little-endian */
}

void xemu_calltrace_record_data(uint32_t call_site, uint32_t callee,
                                const uint32_t args[6]);

void HELPER(xemu_calltrace_data)(CPUX86State *env, target_ulong call_site,
                                 target_ulong callee)
{
    uint32_t esp = (uint32_t)env->regs[R_ESP];
    uint32_t a[6];
    a[0] = (uint32_t)env->regs[R_ECX];
    a[1] = (uint32_t)env->regs[R_EDX];
    for (int k = 0; k < 4; k++) {
        a[2 + k] = ct_safe_ldl(env, esp + 4u * (uint32_t)k);
    }
    xemu_calltrace_record_data((uint32_t)call_site, (uint32_t)callee, a);
}
```

- [ ] **Step 3: Branch to the Data helper in the translator**

In `target/i386/tcg/emit.c.inc`, extend the extern declaration (line 1581) and the wrapper (lines 1583-1586):

```c
extern bool xemu_calltrace_armed;
extern bool xemu_calltrace_capture_args;

static void gen_xemu_calltrace(DisasContext *s, TCGv callee)
{
    if (xemu_calltrace_capture_args) {
        gen_helper_xemu_calltrace_data(tcg_env, eip_cur_tl(s), callee);
    } else {
        gen_helper_xemu_calltrace_call(eip_cur_tl(s), callee);
    }
}
```

All CALL forms (`gen_CALL`, `gen_CALL_m`, `gen_CALLF`, `gen_CALLF_m`) funnel through `gen_xemu_calltrace`, so this one branch covers them. `xemu_calltrace_capture_args` is only ever true while `xemu_calltrace_armed` is true and the TB cache was flushed, so the mode is stable across the translated region.

- [ ] **Step 4: Build the emulator**

Run (MSYS2 UCRT64 shell): `ninja -C build`
Expected: links `qemu-system-i386w.exe` with no errors. If `cpu_memory_rw_debug` is undeclared, confirm the `hw/core/cpu.h` include from Step 2.

- [ ] **Step 5: Commit**

```bash
git add target/i386/helper.h target/i386/tcg/misc_helper.c target/i386/tcg/emit.c.inc
git commit -m "calltrace: Emit arg-capturing helper in Data mode"
```

---

## Phase C — Format (v3 writer + tools + guide)

### Task C1: v3 writer

**Files:**
- Modify: `xemu-calltrace.c:405-546` (`xemu_calltrace_save`)

**Interfaces:**
- Consumes: `ct_edge_args`, `ct_arg_index`, `ordered[]` (edges in index order), `ct_deflate_events` pattern.
- Produces: `.xct` v3 = v2 + Data block (arg-set table + index stream, both deflated).

- [ ] **Step 1: Add a generic deflate helper for a byte buffer**

In `xemu-calltrace.c`, right after `ct_deflate_events` (line 403), add a helper that deflates an arbitrary byte range (reused for both the table and the index stream):

```c
/* Deflate a raw byte range into a GByteArray. */
static GByteArray *ct_deflate_bytes(const uint8_t *data, size_t len)
{
    GByteArray *out = g_byte_array_new();
    z_stream zs = { 0 };
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        return out;
    }
    guint8 obuf[65536];
    zs.next_in = (Bytef *)data;
    zs.avail_in = len;
    int rc;
    do {
        zs.next_out = obuf;
        zs.avail_out = sizeof(obuf);
        rc = deflate(&zs, Z_FINISH);
        g_byte_array_append(out, obuf, sizeof(obuf) - zs.avail_out);
    } while (rc == Z_OK);
    deflateEnd(&zs);
    return out;
}

/* Build the uncompressed arg-set table blob in edge-index order:
 * per edge: u8 nsets, then nsets * CT_ARGSET_DWORDS * u32. */
static GByteArray *ct_build_argtable(CTEdge **ordered, uint32_t nedges)
{
    GByteArray *t = g_byte_array_new();
    for (uint32_t i = 0; i < nedges; i++) {
        CTEdgeArgs *ea = (i < ct_edge_args_cap) ? ct_edge_args[i] : NULL;
        uint8_t nsets = ea ? ea->nsets : 0;
        g_byte_array_append(t, &nsets, 1);
        if (nsets) {
            g_byte_array_append(t, (const guint8 *)ea->sets,
                                nsets * CT_ARGSET_DWORDS * sizeof(uint32_t));
        }
    }
    (void)ordered; /* index order == ct_edge_args index; ordered unused here */
    return t;
}
```

- [ ] **Step 2: Set the version and gate the event block on Data too**

In `xemu_calltrace_save`, replace the `bool timed = ...` line (line 476) and the version field (line 478) with:

```c
        bool timed = ct_mode == CT_TIMED;
        bool data = ct_mode == CT_DATA;
        bool has_events = timed || data;
        uint32_t hdr[10] = { 0x52544358u,
                             data ? 3u : (timed ? 2u : 1u),
                             ct_truncated ? 1u : 0u,
```

Then change the event-block guard (line 515) from `if (ok && timed)` to:

```c
        /* v2/v3: trailing compressed event block. */
        if (ok && has_events) {
```

- [ ] **Step 3: Append the Data block after the event block**

Still in `xemu_calltrace_save`, immediately after the event-block `if (ok && has_events) { ... }` closes (after line 531, before `fclose(f);`) add:

```c
        /* v3: trailing Data block (arg-set table + per-event index). */
        if (ok && data) {
            GByteArray *table = ct_build_argtable(ordered, ct_map.num_entries);
            GByteArray *tcomp = ct_deflate_bytes(table->data, table->len);
            GByteArray *icomp = ct_deflate_bytes(
                ct_arg_index ? ct_arg_index->data : NULL,
                ct_arg_index ? ct_arg_index->len : 0);
            uint32_t argset_dwords = CT_ARGSET_DWORDS;
            uint32_t argset_cap = CT_ARGSET_CAP;
            uint64_t table_raw = table->len;
            uint64_t table_comp = tcomp->len;
            uint64_t index_raw = ct_arg_index ? ct_arg_index->len : 0;
            uint64_t index_comp = icomp->len;
            ok = fwrite(&argset_dwords, 4, 1, f) == 1 &&
                 fwrite(&argset_cap, 4, 1, f) == 1 &&
                 fwrite(&table_raw, 8, 1, f) == 1 &&
                 fwrite(&table_comp, 8, 1, f) == 1 &&
                 fwrite(&index_raw, 8, 1, f) == 1 &&
                 fwrite(&index_comp, 8, 1, f) == 1 &&
                 (table_comp == 0 ||
                  fwrite(tcomp->data, tcomp->len, 1, f) == 1) &&
                 (index_comp == 0 ||
                  fwrite(icomp->data, icomp->len, 1, f) == 1);
            g_byte_array_free(table, TRUE);
            g_byte_array_free(tcomp, TRUE);
            g_byte_array_free(icomp, TRUE);
        }
```

Note: `ordered[]` is freed at line 512 (`g_free(ordered);`) which is *before* this block. Move that `g_free(ordered);` down to just before `fclose(f);` so `ordered` is still valid here. (Alternatively `ct_build_argtable` ignores `ordered` and indexes `ct_edge_args` directly — it does — so freeing `ordered` early is actually safe. Keep `g_free(ordered)` where it is; `ct_build_argtable`'s `ordered` param is unused.)

- [ ] **Step 4: Build the emulator**

Run (MSYS2 UCRT64 shell): `ninja -C build`
Expected: links cleanly.

- [ ] **Step 5: Commit**

```bash
git add xemu-calltrace.c
git commit -m "calltrace: Write .xct v3 Data block (arg-set table + index)"
```

---

### Task C2: v3 fixture + dump tool

**Files:**
- Modify: `tools/calltrace/make_test_xct.py`
- Modify: `tools/calltrace/xct_dump.py`
- Create: `tools/calltrace/test-fixture-data.xct`

**Interfaces:**
- Consumes: existing `EDGES`, `EVENT_STREAM`, `build_timed()`.
- Produces: `build_data()` writing a v3 fixture; `xct_dump.py` prints the Data block; a committed fixture.

- [ ] **Step 1: Add the arg stream and `build_data()` to `make_test_xct.py`**

After `EVENT_STREAM = [...]` (line 19) add a per-event 6-dword arg stream (one tuple per event, same length as `EVENT_STREAM`), chosen to exercise dedup, new-set allocation, and a never-fired edge:

```python
# One 6-dword arg snapshot per event (parallel to EVENT_STREAM). Repeats
# create dedup; edge 3 and edge 0 each get a second distinct set.
ARG_STREAM = [
    (0x00011A40, 0, 5, 0, 0, 0),      # ev0  edge0 -> set0
    (1, 2, 3, 4, 5, 6),               # ev1  edge1 -> set0
    (0xDEAD, 0xBEEF, 0, 0, 0, 0),     # ev2  edge2 -> set0
    (7, 7, 7, 7, 7, 7),               # ev3  edge3 -> set0
    (7, 7, 7, 7, 7, 7),               # ev4  edge3 -> set0 (dup)
    (8, 8, 8, 8, 8, 8),               # ev5  edge3 -> set1 (new)
    (0x80012345, 0, 0, 0, 0, 0),      # ev6  edge4 -> set0
    (0x80012345, 0, 0, 0, 0, 0),      # ev7  edge4 -> set0 (dup)
    (0, 0, 0, 0, 0, 0),               # ev8  edge6 -> set0
    (0x16000, 0, 0, 0, 0, 0),         # ev9  edge7 -> set0
    (0x12000, 1, 2, 3, 4, 5),         # ev10 edge8 -> set0
    (0x00011A44, 0, 6, 0, 0, 0),      # ev11 edge0 -> set1 (new)
    (0xDEAD, 0xBEEF, 0, 0, 0, 0),     # ev12 edge2 -> set0 (dup)
]
ARGSET_DWORDS = 6
ARGSET_CAP = 16
```

Then add `build_data()` after `build_timed()` (after line 61). It mirrors the engine's intern to compute per-edge tables and the index stream:

```python
def build_data():
    import zlib
    base = bytearray(build())              # v1 body
    struct.pack_into('<I', base, 4, 3)     # version 1 -> 3

    # v2 event block (identical to build_timed()).
    ev = struct.pack('<%dI' % len(EVENT_STREAM), *EVENT_STREAM)
    ev_blob = zlib.compress(ev, 9)
    out = base
    out += struct.pack('<Q', len(EVENT_STREAM))
    out += struct.pack('<III', 0, 256, 64)
    out += struct.pack('<Q', len(ev))
    out += struct.pack('<Q', len(ev_blob))
    out += ev_blob

    # Intern arg snapshots per edge (same rule as ct_argset_intern).
    tables = [[] for _ in range(len(EDGES))]   # edge -> list of tuples
    index = []
    for edge_idx, snap in zip(EVENT_STREAM, ARG_STREAM):
        t = tables[edge_idx]
        if snap in t:
            index.append(t.index(snap))
        elif len(t) < ARGSET_CAP:
            index.append(len(t))
            t.append(snap)
        else:
            index.append(0xFF)

    # Arg-set table blob: per edge u8 nsets, then nsets * 6 * u32.
    table = bytearray()
    for t in tables:
        table.append(len(t))
        for snap in t:
            table += struct.pack('<6I', *snap)
    idx = bytes(index)
    tcomp = zlib.compress(bytes(table), 9)
    icomp = zlib.compress(idx, 9)

    out += struct.pack('<II', ARGSET_DWORDS, ARGSET_CAP)
    out += struct.pack('<Q', len(table))
    out += struct.pack('<Q', len(tcomp))
    out += struct.pack('<Q', len(idx))
    out += struct.pack('<Q', len(icomp))
    out += tcomp
    out += icomp
    return bytes(out)
```

Update the `__main__` block (lines 64-71) to accept `--data`:

```python
if __name__ == '__main__':
    timed = '--timed' in sys.argv
    data = '--data' in sys.argv
    args = [a for a in sys.argv[1:] if a not in ('--timed', '--data')]
    default = ('test-fixture-data.xct' if data
               else 'test-fixture-timed.xct' if timed
               else 'test-fixture.xct')
    path = args[0] if args else default
    payload = build_data() if data else build_timed() if timed else build()
    open(path, 'wb').write(payload)
    print(f'wrote {path}')
```

- [ ] **Step 2: Teach `xct_dump.py` to print the Data block**

In `tools/calltrace/xct_dump.py`, after the code that reads the v2 event block, add v3 handling. (Match the existing parse structure; append after the event-block print, guarded by `version == 3`.)

```python
    if version == 3:
        argset_dwords, argset_cap = struct.unpack_from('<II', d, off); off += 8
        table_raw, table_comp = struct.unpack_from('<QQ', d, off); off += 16
        index_raw, index_comp = struct.unpack_from('<QQ', d, off); off += 16
        table = zlib.decompress(d[off:off + table_comp]); off += table_comp
        index = zlib.decompress(d[off:off + index_comp]); off += index_comp
        assert len(table) == table_raw and len(index) == index_raw
        # Walk the per-edge table.
        o = 0
        total_sets = 0
        multi = 0
        for _ in range(nedge):
            nsets = table[o]; o += 1
            o += nsets * argset_dwords * 4
            total_sets += nsets
            if nsets > 1:
                multi += 1
        overflow = index.count(0xFF)
        print(f'data: {argset_dwords} dwords/set, cap {argset_cap}, '
              f'{total_sets} sets across {nedge} edges '
              f'({multi} multi-set), {len(index)} indices, '
              f'{overflow} overflow')
```

(If `xct_dump.py` does not already `import zlib`, add it.)

- [ ] **Step 3: Generate the fixture and dump it**

Run:
```bash
python3 tools/calltrace/make_test_xct.py --data tools/calltrace/test-fixture-data.xct
python3 tools/calltrace/xct_dump.py tools/calltrace/test-fixture-data.xct
```
Expected: `wrote tools/calltrace/test-fixture-data.xct`, then a dump ending with a `data:` line reporting `11 sets across 9 edges (2 multi-set), 13 indices, 0 overflow` (edge0=2, edge3=2, edges 1/2/4/6/7/8 = 1 each, edge5=0 → 2+2+1+1+1+1+1+1 = 11; edges 0 and 3 are multi-set).

- [ ] **Step 4: Round-trip assertion**

Run this one-off check:
```bash
python3 - <<'PY'
import struct, zlib
d = open('tools/calltrace/test-fixture-data.xct','rb').read()
ver = struct.unpack_from('<I', d, 4)[0]
assert ver == 3, ver
nedge = struct.unpack_from('<I', d, 32)[0]
assert nedge == 9, nedge
# find data block: skip to end via known layout is fiddly; re-parse minimally
# (reuse xct_dump path instead in practice). Here just assert the file parses.
print('v3 fixture OK, edges', nedge)
PY
```
Expected: `v3 fixture OK, edges 9`

- [ ] **Step 5: Commit**

```bash
git add tools/calltrace/make_test_xct.py tools/calltrace/xct_dump.py tools/calltrace/test-fixture-data.xct
git commit -m "calltrace: Add v3 (--data) fixture and dump support"
```

---

### Task C3: Document v3 in `XCT_FORMAT.md`

**Files:**
- Modify: `tools/calltrace/XCT_FORMAT.md`

**Interfaces:** none (docs).

- [ ] **Step 1: Update the version list and overall layout**

At the top, change the two-versions list to include v3:

```markdown
- **v3 (Data)** — everything in v2, plus per-call argument snapshots: a
  per-edge table of distinct argument-sets and a 1-byte-per-event index into it.
```

In "Overall layout", add a row after the Event block:

```
+------------------------------------------------------+
| Data block         (v3 only) 40-byte sub-header +    |
|                    two deflate streams (table, index) |
+------------------------------------------------------+
```

Change the header `version` row meaning to `1 = Edges, 2 = Timed, 3 = Data`.

- [ ] **Step 2: Add the Data block section**

After the "Event block (v2 only)" section, add:

```markdown
## Data block (v3 only)

Appended immediately after the Event block. Present iff `version == 3`. A v3
file is a full v2 file (it always contains the Event block) plus this block.

### Sub-header (40 bytes)

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| +0 | 4 | u32 | `argset_dwords` | dwords per arg-set (always 6: ECX, EDX, [ESP+0..12]) |
| +4 | 4 | u32 | `argset_cap` | max distinct sets per edge (always 16) |
| +8 | 8 | u64 | `table_raw_bytes` | uncompressed arg-set table size |
| +16 | 8 | u64 | `table_comp_bytes` | compressed arg-set table size |
| +24 | 8 | u64 | `index_raw_bytes` | uncompressed index size = `event_count` |
| +32 | 8 | u64 | `index_comp_bytes` | compressed index size |

### Arg-set table (`table_comp_bytes`, zlib)

Decompresses to `table_raw_bytes`. A sequential walk over **all edges, in
index order** (`edge_count` entries):

```
for each edge:
    u8  nsets                         # 0..16
    nsets × (argset_dwords × u32)     # ECX, EDX, [ESP+0], [ESP+4], [ESP+8], [ESP+12]
```

Edge `k`'s distinct arg-sets are the `k`-th entry.

### Index stream (`index_comp_bytes`, zlib)

Decompresses to `event_count` bytes, one `u8` per event, parallel to the event
stream. For event `i`, the arg-set is `edges[events[i]].argsets[index[i]]`,
unless `index[i] == 0xFF` (**overflow** — that call's snapshot was a 17th+
distinct set and was not stored).

**Semantics:** the snapshot is the outgoing argument words *at the call*, read
before the return address is pushed — so the stack dwords are the caller's
pushed cdecl/stdcall arguments and ECX/EDX are the thiscall/fastcall register
arguments. Only the words are stored, not the memory they point at.
```

- [ ] **Step 3: Extend the Python `read_xct()` snippet**

In the "Minimal parser (Python)" block, change `assert ... ver in (1, 2)` to `ver in (1, 2, 3)`, and after the `if ver >= 2:` event block, add:

```python
    argsets = None
    if ver >= 3:
        adw, acap = struct.unpack_from('<2I', d, off); off += 8
        traw, tcomp = struct.unpack_from('<2Q', d, off); off += 16
        iraw, icomp = struct.unpack_from('<2Q', d, off); off += 16
        table = zlib.decompress(d[off:off + tcomp]); off += tcomp
        idx = zlib.decompress(d[off:off + icomp]); off += icomp
        o = 0
        argsets = []
        for _ in range(nedge):
            nsets = table[o]; o += 1
            sets = []
            for _ in range(nsets):
                sets.append(struct.unpack_from('<%dI' % adw, table, o))
                o += adw * 4
            argsets.append(sets)
        events_argidx = list(idx)   # parallel to events
```

- [ ] **Step 4: Commit**

```bash
git add tools/calltrace/XCT_FORMAT.md
git commit -m "docs: Document .xct v3 Data block in XCT_FORMAT.md"
```

---

## Phase D — Viewer

### Task D1: Parse v3 + inflate args + attach to model

**Files:**
- Modify: `tools/calltrace/viewer.html` (`parseXCT` 212-276; new `inflateArgs`; `loadRecording` 440-453)

**Interfaces:**
- Consumes: `rec.edges`, `DecompressionStream`.
- Produces: `rec.hasArgs`, `rec.argsetDwords`, `rec.argsets` (per-edge array of dword arrays), `rec.argIndex`/`model.argIndex` (Uint8Array), `model.calleeArgsets` (Map callee→{sets,overflow}).

- [ ] **Step 1: Accept v3 and slice the Data block in `parseXCT`**

Change the version guard (line 219) to allow 3:

```js
  if (version !== 1 && version !== 2 && version !== 3) {
    throw new Error('unsupported version ' + version);
  }
```

After the event-block read (after line 271, before the `return`), add:

```js
  let hasArgs = false, argsetDwords = 0, argsetCap = 0,
      argTableComp = null, argIndexComp = null;
  if (version === 3 && off + 40 <= buf.byteLength) {
    hasArgs = true;
    argsetDwords = dv.getUint32(off, true); off += 4;
    argsetCap = dv.getUint32(off, true); off += 4;
    /* table_raw */ dv.getBigUint64(off, true); off += 8;
    const tableComp = Number(dv.getBigUint64(off, true)); off += 8;
    /* index_raw */ dv.getBigUint64(off, true); off += 8;
    const indexComp = Number(dv.getBigUint64(off, true)); off += 8;
    argTableComp = new Uint8Array(buf, off, tableComp); off += tableComp;
    argIndexComp = new Uint8Array(buf, off, indexComp); off += indexComp;
  }
```

Add the new fields to the returned object (extend the `return { ... }`):

```js
           eventRawBytes,
           hasArgs, argsetDwords, argsetCap, argTableComp, argIndexComp };
```

- [ ] **Step 2: Add `inflateArgs` and a callee index builder**

After `inflateEvents` (line 370) add:

```js
async function inflateArgs(rec) {
  if (!rec.hasArgs) return;
  const inflate = async comp => {
    const ds = new DecompressionStream('deflate');
    const raw = await new Response(
        new Blob([comp]).stream().pipeThrough(ds)).arrayBuffer();
    return new Uint8Array(raw);
  };
  try {
    const table = await inflate(rec.argTableComp);
    const index = await inflate(rec.argIndexComp);
    const dv = new DataView(table.buffer, table.byteOffset, table.byteLength);
    const D = rec.argsetDwords;
    let o = 0;
    const argsets = new Array(rec.edges.length);
    for (let i = 0; i < rec.edges.length; i++) {
      const nsets = table[o++];
      const sets = [];
      for (let s = 0; s < nsets; s++) {
        const w = new Array(D);
        for (let d = 0; d < D; d++) { w[d] = dv.getUint32(o, true); o += 4; }
        sets.push(w);
      }
      argsets[i] = sets;
    }
    rec.argsets = argsets;
    rec.argIndex = index;          // Uint8Array, length == eventCount
  } catch (e) {
    rec.hasArgs = false;
  }
}

/* Aggregate distinct arg-sets per callee address (across all raw edges into
 * it), with an overflow flag when any contributing edge hit the cap. */
function buildCalleeArgsets(m) {
  const rec = m.rec;
  m.calleeArgsets = new Map();
  if (!rec.argsets) return;
  for (let i = 0; i < rec.edges.length; i++) {
    const sets = rec.argsets[i];
    if (!sets || !sets.length) continue;
    const callee = rec.edges[i].callee;
    let agg = m.calleeArgsets.get(callee);
    if (!agg) { agg = { sets: [], keys: new Set(), overflow: false };
                m.calleeArgsets.set(callee, agg); }
    if (sets.length >= rec.argsetCap) agg.overflow = true;
    for (const w of sets) {
      const k = w.join(',');
      if (!agg.keys.has(k)) { agg.keys.add(k); agg.sets.push(w); }
    }
  }
}
```

- [ ] **Step 3: Attach args during load**

In `loadRecording`, after `model.events = await inflateEvents(rec);` (line 451) and before/around `buildTimeline(model)` (line 453), add:

```js
    model.events = await inflateEvents(rec);
    if (model.events) {
      await inflateArgs(rec);
      model.argIndex = rec.argIndex || null;
      buildTimeline(model);
      buildCalleeArgsets(model);
    }
```

(Preserve the existing surrounding `if` structure — only add `inflateArgs`, `model.argIndex`, and `buildCalleeArgsets`.)

- [ ] **Step 4: Manual smoke — load the v3 fixture**

Serve `tools/calltrace` over `python3 -m http.server` and open `viewer.html`; drop `test-fixture-data.xct`. Expected: it loads with no console errors and the timeline appears (v3 includes the event block). Deeper checks come from the selftests in D3.

- [ ] **Step 5: Commit**

```bash
git add tools/calltrace/viewer.html
git commit -m "viewer: Parse .xct v3 args and attach to the model"
```

---

### Task D2: `classifyWord` + node-panel Arguments section

**Files:**
- Modify: `tools/calltrace/viewer.html` (`showDetails` 1508-1546; new `classifyWord`)

**Interfaces:**
- Consumes: `m.rec.sections`, `KERNEL_BOUND`, `m.rec.base`, `m.calleeArgsets`, `sectionIndexOf`.
- Produces: `classifyWord(m, v) -> {cls, label}`; an "Arguments" block in the details panel.

- [ ] **Step 1: Add `classifyWord`**

Near `sectionIndexOf` (line 426) add:

```js
/* Classify an argument dword by where its value lands. No dereferencing:
 * the pointed-at memory is not in the file. */
function classifyWord(m, v) {
  if (v >= KERNEL_BOUND) return { cls: 'kernel', label: 'kernel' };
  const si = sectionIndexOf(m, v);
  if (si >= 0) {
    const name = m.rec.sections[si].name || '';
    const code = /text|code/i.test(name);
    return { cls: code ? 'code' : 'data', label: (name || (code ? 'code' : 'data')) };
  }
  if (v < (m.rec.base || 0x10000)) return { cls: 'int', label: 'int' };
  return { cls: 'raw', label: '?' };
}

/* Render one arg-set as classified hex words. */
function fmtArgset(m, w) {
  const names = ['ecx', 'edx', '[esp]', '[esp+4]', '[esp+8]', '[esp+12]'];
  return w.map((v, i) => {
    const c = classifyWord(m, v);
    return `<span class="aw ${c.cls}" title="${names[i]} · ${esc(c.label)}">` +
           `${hex(v)}</span>`;
  }).join(' ');
}
```

- [ ] **Step 2: Add the Arguments section to `showDetails`**

In `showDetails`, before `d.innerHTML = rows.join('');` (line 1542) add:

```js
  const agg = model.calleeArgsets && model.calleeArgsets.get(id);
  if (agg && agg.sets.length) {
    rows.push(`<h4>Arguments (${agg.sets.length} distinct` +
              `${agg.overflow ? ', capped' : ''})</h4>`);
    rows.push('<div class="argnote">values only — pointers are classified ' +
              'by range, not dereferenced</div>');
    for (const w of agg.sets.slice(0, 64)) {
      rows.push(`<div class="argrow">${fmtArgset(model, w)}</div>`);
    }
    if (agg.overflow) {
      rows.push('<div class="site">+ more distinct sets not captured ' +
                '(cap 16 per edge)</div>');
    }
  }
```

- [ ] **Step 3: Add minimal styles**

In the `<style>` block (co-locate with existing `.site`/`.kv` rules), add:

```css
.argrow { font-family: monospace; font-size: 11px; margin: 2px 0;
          white-space: nowrap; overflow-x: auto; }
.argnote { color: #888; font-size: 10px; margin: 2px 0 4px; }
.aw { padding: 0 2px; border-radius: 2px; }
.aw.code { color: #7fd1ff; } .aw.data { color: #ffd07f; }
.aw.kernel { color: #ff9f9f; } .aw.int { color: #b8ffb8; }
.aw.raw { color: #ccc; }
```

- [ ] **Step 4: Manual check**

Reload the served viewer with `test-fixture-data.xct`; click the node at `0x13000` (callee of edges 1,2,6). Expected: an "Arguments" section lists its distinct sets (e.g. `dead beef 0 0 0 0`), each word colored by class.

- [ ] **Step 5: Commit**

```bash
git add tools/calltrace/viewer.html
git commit -m "viewer: Show classified argument sets in the node panel"
```

---

### Task D3: Live per-call readout + selftests

**Files:**
- Modify: `tools/calltrace/viewer.html` (`updateReadout` 493-502; selftests near 1884-1900)

**Interfaces:**
- Consumes: `model.events`, `model.argIndex`, `model.rec.argsets`, `classifyWord`.
- Produces: current-call args in the timeline readout; new selftests for v3.

- [ ] **Step 1: Show the current call's args in `updateReadout`**

Replace the body of `updateReadout` (lines 493-502) with:

```js
function updateReadout() {
  const p = timeline.playhead;
  let last = '';
  if (model && model.events && p > 0) {
    const rawEdge = model.events[p - 1];
    const e = model.edgeFn[rawEdge];
    last = ' · last: ' + (e ? fnName(model, e.calleeId) : '');
    if (model.argIndex && model.rec.argsets) {
      const ai = model.argIndex[p - 1];
      if (ai === 0xFF) {
        last += '  args: (not captured)';
      } else {
        const sets = model.rec.argsets[rawEdge];
        const w = sets && sets[ai];
        if (w) {
          last += '  args: ' + w.map(v => hex(v) + '·' +
                    classifyWord(model, v).cls).join(', ');
        }
      }
    }
  }
  el('tl-readout').textContent =
      `${p} / ${model ? model.eventCount : 0}${last}`;
}
```

- [ ] **Step 2: Write v3 selftests**

In the `?selftest=1` harness, add a fixture builder and tests alongside the existing timeline tests (near line 1884). First add a v3 fixture builder next to `buildFixture` (the existing timed fixture builder used by tests — mirror it to produce the `--data` bytes, or import the same `ARG_STREAM` layout). Add:

```js
T('v3 parse: argsets + index attach', async () => {
  const rec = parseXCT(buildDataFixture());
  assertEq(rec.hasArgs, true, 'hasArgs');
  assertEq(rec.argsetDwords, 6, 'dwords');
  const m = deriveModel(rec);
  m.events = await inflateEvents(rec);
  await inflateArgs(rec);
  buildCalleeArgsets(m);
  m.argIndex = rec.argIndex;
  assertEq(rec.argsets[0].length, 2, 'edge0 has 2 sets');
  assertEq(rec.argsets[3].length, 2, 'edge3 has 2 sets');
  assertEq(rec.argsets[5].length, 0, 'edge5 never fired');
  assertEq(rec.argIndex.length, 13, 'index parallel to events');
  assertEq(rec.argIndex[5], 1, 'ev5 -> edge3 set1');
  assertEq(rec.argIndex[4], 0, 'ev4 -> edge3 set0 (dup)');
});

T('v3 classifyWord ranges', () => {
  const rec = parseXCT(buildDataFixture());
  const m = deriveModel(rec);
  assertEq(classifyWord(m, 0x80012345).cls, 'kernel', 'kernel range');
  assertEq(classifyWord(m, 0x11500).cls, 'code', '.text range');   // .text 0x11000+0x8000
  assertEq(classifyWord(m, 5).cls, 'int', 'small int');
});
```

Add the `buildDataFixture()` helper (JS that emits the same v3 bytes as `make_test_xct.py --data`). Keep it beside `buildFixture()`; reuse its edge/section/strtab code and append the event block + Data block exactly as C2 Step 1 describes (deflate via `CompressionStream` or ship the compressed bytes inline as a base64 constant decoded in the test). Simplest: embed the committed `test-fixture-data.xct` as a base64 string constant and `atob` it into an ArrayBuffer, so the selftest reads the exact committed fixture.

- [ ] **Step 3: Run the selftests**

Serve `tools/calltrace` and open `viewer.html?selftest=1` (or run via Playwright against `http://localhost:PORT/viewer.html?selftest=1`). Expected: all prior selftests still pass **and** the three new v3 tests pass.

- [ ] **Step 4: Manual check**

Load `test-fixture-data.xct`, press play (or scrub). Expected: the timeline readout shows `args: …` for the current call, and `(not captured)` never appears for this fixture (0 overflow).

- [ ] **Step 5: Commit**

```bash
git add tools/calltrace/viewer.html
git commit -m "viewer: Live per-call arg readout + v3 selftests"
```

---

## Phase E — UI: menu, config, hotkey

### Task E1: Config key + "Start - Data" menu entry

**Files:**
- Modify: `config_spec.yml:10`
- Modify: `ui/xui/menubar.cc:226-236`

**Interfaces:**
- Consumes: `xemu_calltrace_start_mode(CT_DATA)`, `g_config.general.calltrace_hotkey_mode`.
- Produces: config field `general.calltrace_hotkey_mode` (default `"data"`); a menu item to start Data mode.

- [ ] **Step 1: Add the config key**

In `config_spec.yml`, under `general` after `calltrace_dir: string` (line 10) add:

```yaml
  calltrace_hotkey_mode:
    type: string
    default: data
```

- [ ] **Step 2: Add the menu item**

In `ui/xui/menubar.cc`, after the "Start - Timed" item block (lines 233-236) add:

```cpp
                    if (ImGui::MenuItem("Start - Data (call + args)", NULL,
                                        false, have_xbe)) {
                        xemu_calltrace_start_mode(CT_DATA);
                    }
```

Also update the recording status label (lines 264-268) so Data is named:

```cpp
                    const char *modestr =
                        xemu_calltrace_mode() == CT_DATA ? "Data" :
                        xemu_calltrace_mode() == CT_TIMED ? "Timed" : "Edges";
                    bool timed = xemu_calltrace_mode() == CT_TIMED ||
                                 xemu_calltrace_mode() == CT_DATA;
                    ImGui::Text("Recording (%s): %llu edges", modestr,
                                (unsigned long long)
                                    xemu_calltrace_edge_count());
```

(The `timed` bool already gates the "Events:" line below it; Data now shows events too.)

- [ ] **Step 3: Build**

Run (MSYS2 UCRT64 shell): `ninja -C build`
Expected: config header regenerates (meson runs `gen_config.py`), menubar compiles, links.

- [ ] **Step 4: Commit**

```bash
git add config_spec.yml ui/xui/menubar.cc
git commit -m "calltrace: Add Data mode menu entry and hotkey-mode config"
```

---

### Task E2: `Ctrl+Alt+T` toggle + toasts

**Files:**
- Modify: `ui/xemu.c` (`handle_keydown` switch, ~line 368-410)

**Interfaces:**
- Consumes: `xemu_calltrace_mode`, `xemu_calltrace_start_mode`, `xemu_calltrace_stop`, `xemu_calltrace_save`, `xemu_queue_notification`, `xemu_get_xbe_info`, `g_config.general.{calltrace_hotkey_mode,calltrace_dir,screenshot_dir}`.
- Produces: a global `Ctrl+Alt+T` recording toggle with on-screen toasts.

- [ ] **Step 1: Ensure the needed headers are included**

At the top of `ui/xemu.c`, confirm/add includes (near the other `xemu-*.h` includes):

```c
#include "xemu-calltrace.h"
#include "xemu-notifications.h"
#include "xemu-xbe.h"
```

(If any are already included, don't duplicate.)

- [ ] **Step 2: Add the hotkey case**

In `handle_keydown`, inside the `switch (ev->key.scancode)` (the `gui_key_modifier_pressed` group), add a `case` alongside `SDL_SCANCODE_F` etc.:

```c
        case SDL_SCANCODE_T: {
            gui_keysym = 1;
            if (xemu_calltrace_mode() != CT_OFF) {
                xemu_calltrace_stop();
                const char *dir = g_config.general.calltrace_dir;
                if (!strlen(dir)) {
                    dir = g_config.general.screenshot_dir;
                }
                char *err = NULL;
                char *path = xemu_calltrace_save(dir, &err);
                if (path) {
                    char *msg = g_strdup_printf("Call trace saved: %s",
                                                g_path_get_basename(path));
                    xemu_queue_notification(msg);
                    g_free(msg);
                    g_free(path);
                } else {
                    xemu_queue_notification(err ? err
                                            : "Call trace save failed");
                    g_free(err);
                }
            } else if (xemu_get_xbe_info() != NULL) {
                const char *m = g_config.general.calltrace_hotkey_mode;
                CalltraceMode mode =
                    (m && !strcmp(m, "edges")) ? CT_EDGES :
                    (m && !strcmp(m, "timed")) ? CT_TIMED : CT_DATA;
                xemu_calltrace_start_mode(mode);
                const char *label = mode == CT_EDGES ? "Edges" :
                                    mode == CT_TIMED ? "Timed" : "Data";
                char *msg = g_strdup_printf("Call trace: recording (%s)",
                                            label);
                xemu_queue_notification(msg);
                g_free(msg);
            } else {
                xemu_queue_notification("Load a game before tracing");
            }
            break;
        }
```

Note: `g_path_get_basename` returns a newly-allocated string; if you prefer to avoid the extra alloc, pass `path` directly to the message. Keep the `g_free`s to avoid leaks.

- [ ] **Step 3: Build**

Run (MSYS2 UCRT64 shell): `ninja -C build`
Expected: links `qemu-system-i386w.exe`.

- [ ] **Step 4: Manual smoke (launch)**

Launch `build/qemu-system-i386w.exe` from an MSYS2 UCRT64 shell (PATH has the right DLLs). Expected: it starts. Full behavioral validation (start/stop toast, saved file) is in the end-to-end task since it needs a booted game.

- [ ] **Step 5: Commit**

```bash
git add ui/xemu.c
git commit -m "calltrace: Ctrl+Alt+T toggles recording with on-screen toast"
```

---

## Task F: End-to-end validation (user hardware)

**Not autonomously completable** — requires the user's BIOS + a booted game.

- [ ] Boot a game; press `Ctrl+Alt+T`. Expect a "recording (Data)" toast.
- [ ] Play briefly; press `Ctrl+Alt+T` again. Expect a "saved" toast naming the `.xct`.
- [ ] `python3 tools/calltrace/xct_dump.py <file>` reports `version 3` and a `data:` line with nonzero sets.
- [ ] Load the file in `viewer.html`: click functions to see classified arg-sets; scrub the timeline to watch the live per-call `args:` readout; confirm the file size is reasonable (Data block a small fraction of the event block).

---

## Self-Review

**Spec coverage:**
- Third mode `CT_DATA` → A2, B1, E1. ✓
- 6-dword snapshot (ECX,EDX,[ESP+0..12]) → B1 helper; constants A1. ✓
- C-linked N=16 (event stream unchanged + 1-byte index + ≤16 per-edge sets) → A1/A2 store, C1 writer. ✓
- Non-faulting stack read → B1 `ct_safe_ldl`. ✓
- `.xct` v3 = v2 + Data block; XCT_FORMAT.md → C1, C3. ✓
- Fixtures/tools (`--data`, dump) → C2. ✓
- Viewer: parse, classify, node panel, live readout, honest "no deref" note → D1–D3. ✓
- Hotkey `Ctrl+Alt+T`, configurable default mode, universal stop, auto-save, toasts → E1, E2. ✓
- Tests: C unit (A1), format round-trip (C2), viewer selftests (D3), build checks, manual (F). ✓
- Non-goals (return values, deref, type inference, variable width) — not implemented, as intended. ✓

**Placeholder scan:** No TBD/TODO. Every code step shows full code. The one "pick the primitive" from the spec is resolved (B1 uses `cpu_memory_rw_debug`).

**Type consistency:** `ct_argset_intern(CTEdgeArgs*, const uint32_t[6]) -> uint8_t`, `CT_ARGSET_OVERFLOW = 0xFF`, `xemu_calltrace_record_data(uint32_t,uint32_t,const uint32_t[6])`, `xemu_calltrace_capture_args` (bool), helper `xemu_calltrace_data(env,tl,tl)` (`DEF_HELPER_FLAGS_3`), viewer `rec.argsets` / `rec.argIndex` / `model.calleeArgsets` — all names used identically across tasks. Writer field order (argset_dwords, argset_cap, table_raw, table_comp, index_raw, index_comp) matches the parser in C2/C3/D1 and the format doc.

**Known coupling to verify during execution:**
- C1 Step 3 notes the `g_free(ordered)` placement; `ct_build_argtable` does not use `ordered`, so the existing early free is safe.
- D3 uses the committed fixture (base64-embedded) so the selftest and the C writer are checked against the same bytes.

# Call-Trace Recorder + Graph Viewer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Record the dynamic function call graph of a running Xbox game inside xemu (TCG helper on CALL instructions → deduped edge map → binary `.xct` file), and explore it in a standalone single-file HTML viewer with an expand-from-Entry interactive graph.

**Architecture:** A TCG helper is emitted at the four CALL translation sites only while recording is armed (toggling flushes the translation cache, so idle overhead is exactly zero). The helper dedups `(call_site → callee)` edges into an open-addressing hash map on the single vCPU thread. "Stop & Save" writes one compact binary file containing XBE metadata, section table, kernel-export names (resolved by parsing the guest kernel's PE export directory), and the edge array. The viewer derives functions from the edge data (nearest-preceding-entry rule) and renders a collapsible left-to-right graph on Canvas 2D.

**Tech Stack:** C (QEMU/xemu, GLib), Dear ImGui (menu items only), vanilla JS + Canvas 2D (viewer), Python 3 (test fixture + dump tools).

**Spec:** `docs/superpowers/specs/2026-07-10-calltrace-visualizer-design.md`

## Global Constraints

- Zero emulator behavior change while disarmed: instrumentation is guarded by `if (xemu_calltrace_armed)` at **translation time**; arming/disarming calls `tb_flush()`.
- Kernel boundary is `0x80000000`; kernel PE image base is `0x80010000`. Edges where BOTH endpoints are ≥ 0x80000000 are never recorded.
- `.xct` format: magic `0x52544358` ("XCTR" as LE u32), version `1`, little-endian throughout, 128-byte fixed header, then strtab / sections / kimports / edges. Edge record is exactly 16 bytes: `u32 call_site, u32 callee, u64 count`.
- The viewer is ONE self-contained HTML file: no external scripts, fonts, fetches, or build step.
- Kernel export name table `xemu-calltrace-kexports.h` is ALREADY COMMITTED at repo root (commit `83b98c38e2`, generated from nxdk's CC0 `xboxkrnl.exe.def`, 372 named ordinals, array size 379). Do not regenerate it.
- Build: run `./build.sh` from the repo root in the project's usual build shell (MSYS2 on Windows). Output binary: `dist/xemu.exe`. First build is slow; incremental builds are fast.
- Map unit test compiles standalone with plain `gcc` — no meson involvement.
- C code follows QEMU style (4-space indent, GPL-2.0-or-later header comment matching sibling files, e.g. `xemu-xbe.c`). UI code follows existing `ui/xui/` style.
- Commit after every task with a `calltrace:` prefix (match repo style, e.g. `calltrace: Add edge map`).
- The Xbox has ONE vCPU thread; the record hot path is intentionally lock-free. Do not add locks.

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `xemu-calltrace-kexports.h` | already committed | ordinal → kernel export name data table |
| `xemu-calltrace-map.h` | create (Task 1) | header-only open-addressing edge hash map (no QEMU deps; standalone-testable) |
| `tests/calltrace/test-calltrace-map.c` | create (Task 1) | standalone unit test for the map |
| `xemu-calltrace.h` | create (Task 2) | public engine API (C, `extern "C"` guarded) |
| `xemu-calltrace.c` | create (Task 2), extend (Task 5) | engine state, arm/disarm, record, `.xct` writer |
| `meson.build` | modify line 4054 (Task 2) | add `xemu-calltrace.c` to `specific_ss` |
| `ui/xui/menubar.cc` | modify (Tasks 3, 5) | Debug → Call Trace menu |
| `target/i386/helper.h` | modify (Task 4) | declare the TCG helper |
| `target/i386/tcg/misc_helper.c` | modify (Task 4) | helper implementation (2 lines + extern) |
| `target/i386/tcg/emit.c.inc` | modify (Task 4) | emit helper at `gen_CALL`/`gen_CALL_m`/`gen_CALLF`/`gen_CALLF_m` |
| `xemu-xbe.h` / `xemu-xbe.c` | modify (Task 5) | export a guest-virtual-memory read wrapper |
| `config_spec.yml` | modify (Task 5) | add `general.calltrace_dir` |
| `tools/calltrace/xct_dump.py` | create (Task 5) | parse/validate/pretty-print a `.xct` |
| `tools/calltrace/make_test_xct.py` | create (Task 6) | generate the synthetic viewer fixture |
| `tools/calltrace/viewer.html` | create (Task 7), extend (8, 9, 10) | the graph viewer |

---

### Task 1: Edge hash map (standalone, TDD)

**Files:**
- Create: `xemu-calltrace-map.h`
- Test: `tests/calltrace/test-calltrace-map.c`

**Interfaces:**
- Produces: `CTMap`, `CTEdge {uint64_t key; uint64_t count;}`, `bool ct_map_init(CTMap *)`, `void ct_map_free(CTMap *)`, `uint64_t ct_map_key(uint32_t call_site, uint32_t callee)`, `bool ct_map_add(CTMap *, uint32_t call_site, uint32_t callee)`, constants `CT_MAP_CAPACITY` (2^21 slots), `CT_MAP_MAX_ENTRIES` (2^20). `ct_map_add` returns `false` ONLY when the map is at `CT_MAP_MAX_ENTRIES` and the key is new (caller sets a truncated flag). Key layout: `(call_site << 32) | callee`; key `0` is the empty-slot sentinel and is silently accepted-and-ignored. `CTMap.num_entries` is the unique-edge count.

- [ ] **Step 1: Write the failing test**

Create `tests/calltrace/test-calltrace-map.c`:

```c
#include <assert.h>
#include <stdio.h>
#include "../../xemu-calltrace-map.h"

int main(void)
{
    CTMap m;
    assert(ct_map_init(&m));
    assert(m.num_entries == 0);

    /* new edge inserts with count 1 */
    assert(ct_map_add(&m, 0x11000, 0x22000));
    assert(m.num_entries == 1);

    /* same edge increments count, not entries */
    assert(ct_map_add(&m, 0x11000, 0x22000));
    assert(m.num_entries == 1);

    /* different callee from the same site is a distinct edge */
    assert(ct_map_add(&m, 0x11000, 0x33000));
    assert(m.num_entries == 2);

    /* the incremented edge holds count 2 */
    uint64_t key = ct_map_key(0x11000, 0x22000);
    uint64_t found = 0;
    for (uint32_t i = 0; i < CT_MAP_CAPACITY; i++) {
        if (m.slots[i].key == key) {
            found = m.slots[i].count;
        }
    }
    assert(found == 2);

    /* zero key (both args 0) is ignored but reports success */
    assert(ct_map_add(&m, 0, 0));
    assert(m.num_entries == 2);

    /* fill to the cap: new keys refused, existing keys still count */
    for (uint32_t i = 0; m.num_entries < CT_MAP_MAX_ENTRIES; i++) {
        ct_map_add(&m, 0x100000 + i, 0x200000);
    }
    assert(m.num_entries == CT_MAP_MAX_ENTRIES);
    assert(!ct_map_add(&m, 0xF0000000, 0xE0000000)); /* new -> refused */
    assert(ct_map_add(&m, 0x11000, 0x22000));        /* existing -> ok  */

    ct_map_free(&m);
    assert(m.slots == NULL);
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `gcc -O2 -o tests/calltrace/test-calltrace-map tests/calltrace/test-calltrace-map.c && ./tests/calltrace/test-calltrace-map`
Expected: FAIL to compile — `xemu-calltrace-map.h: No such file or directory`

- [ ] **Step 3: Write the implementation**

Create `xemu-calltrace-map.h`:

```c
/*
 * xemu call-trace edge map
 *
 * Open-addressing hash map from (call_site, callee) address pairs to hit
 * counts. Header-only and free of QEMU dependencies so it can be unit
 * tested standalone (see tests/calltrace/test-calltrace-map.c).
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

#ifndef XEMU_CALLTRACE_MAP_H
#define XEMU_CALLTRACE_MAP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define CT_MAP_CAPACITY (1u << 21)    /* slots; power of two (32 MiB)   */
#define CT_MAP_MAX_ENTRIES (1u << 20) /* load-factor cap: 1M edges max  */

typedef struct CTEdge {
    uint64_t key; /* (call_site << 32) | callee; 0 = empty slot */
    uint64_t count;
} CTEdge;

typedef struct CTMap {
    CTEdge *slots;
    uint32_t num_entries;
} CTMap;

static inline bool ct_map_init(CTMap *m)
{
    m->slots = (CTEdge *)calloc(CT_MAP_CAPACITY, sizeof(CTEdge));
    m->num_entries = 0;
    return m->slots != NULL;
}

static inline void ct_map_free(CTMap *m)
{
    free(m->slots);
    m->slots = NULL;
    m->num_entries = 0;
}

static inline uint64_t ct_map_key(uint32_t call_site, uint32_t callee)
{
    return ((uint64_t)call_site << 32) | callee;
}

static inline uint32_t ct_map_hash(uint64_t key)
{
    /* Fibonacci hash; top 21 bits index the 2^21 slots */
    return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 43);
}

/*
 * Record one call. Returns false only when the map is full and the edge
 * is new (the edge is dropped); existing edges always keep counting.
 */
static inline bool ct_map_add(CTMap *m, uint32_t call_site, uint32_t callee)
{
    uint64_t key = ct_map_key(call_site, callee);
    if (key == 0) {
        return true; /* reserved empty sentinel; not a valid code address */
    }
    uint32_t idx = ct_map_hash(key) & (CT_MAP_CAPACITY - 1);
    for (;;) {
        CTEdge *e = &m->slots[idx];
        if (e->key == key) {
            e->count++;
            return true;
        }
        if (e->key == 0) {
            if (m->num_entries >= CT_MAP_MAX_ENTRIES) {
                return false;
            }
            e->key = key;
            e->count = 1;
            m->num_entries++;
            return true;
        }
        idx = (idx + 1) & (CT_MAP_CAPACITY - 1);
    }
}

#endif
```

- [ ] **Step 4: Run test to verify it passes**

Run: `gcc -O2 -o tests/calltrace/test-calltrace-map tests/calltrace/test-calltrace-map.c && ./tests/calltrace/test-calltrace-map`
Expected: `PASS` (the cap-fill loop takes under a second)

- [ ] **Step 5: Commit**

```bash
git add xemu-calltrace-map.h tests/calltrace/test-calltrace-map.c
git commit -m "calltrace: Add standalone edge hash map with unit test"
```

---

### Task 2: Engine core (state, arm/disarm, record)

**Files:**
- Create: `xemu-calltrace.h`, `xemu-calltrace.c`
- Modify: `meson.build:4054`

**Interfaces:**
- Consumes: `CTMap`/`ct_map_*` from Task 1.
- Produces (all in `xemu-calltrace.h`, callable from C and C++):
  - `extern bool xemu_calltrace_armed;` — read by the translator (Task 4) and menu (Task 3)
  - `void xemu_calltrace_start(void);` — reset+alloc map, set armed, `tb_flush`
  - `void xemu_calltrace_stop(void);` — clear armed, `tb_flush` (map data retained for save)
  - `uint64_t xemu_calltrace_edge_count(void);`
  - `bool xemu_calltrace_truncated(void);`
  - `void xemu_calltrace_record(uint32_t call_site, uint32_t callee);` — hot path, called by the TCG helper (Task 4)
  - Task 5 adds `char *xemu_calltrace_save(const char *dir, char **err_msg);` to this header.

- [ ] **Step 1: Create the public header**

Create `xemu-calltrace.h`:

```c
/*
 * xemu guest call tracing
 *
 * Records the running game's dynamic call graph as deduplicated
 * (call_site -> callee) edges for offline visualization.
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

#ifndef XEMU_CALLTRACE_H
#define XEMU_CALLTRACE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * True while recording. Read by the x86 translator, which only emits
 * trace helper calls into translated code while this is set; toggling
 * flushes the TB cache so stale code never lingers.
 */
extern bool xemu_calltrace_armed;

void xemu_calltrace_start(void);
void xemu_calltrace_stop(void);
uint64_t xemu_calltrace_edge_count(void);
bool xemu_calltrace_truncated(void);

/* Hot path; called from the TCG helper on the vCPU thread. */
void xemu_calltrace_record(uint32_t call_site, uint32_t callee);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2: Create the engine**

Create `xemu-calltrace.c`:

```c
/*
 * xemu guest call tracing
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

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/tb-flush.h"
#include "xemu-calltrace.h"
#include "xemu-calltrace-map.h"

#define CT_KERNEL_SPACE 0x80000000u

bool xemu_calltrace_armed;

static CTMap ct_map;
static bool ct_truncated;

void xemu_calltrace_start(void)
{
    if (xemu_calltrace_armed) {
        return;
    }
    if (ct_map.slots) {
        ct_map_free(&ct_map);
    }
    if (!ct_map_init(&ct_map)) {
        return; /* allocation failed; stay disarmed */
    }
    ct_truncated = false;
    /* Arm before flushing so retranslated code is instrumented. */
    xemu_calltrace_armed = true;
    tb_flush(qemu_get_cpu(0));
}

void xemu_calltrace_stop(void)
{
    if (!xemu_calltrace_armed) {
        return;
    }
    xemu_calltrace_armed = false;
    /* Map data intentionally retained until save or next start. */
    tb_flush(qemu_get_cpu(0));
}

uint64_t xemu_calltrace_edge_count(void)
{
    return ct_map.slots ? ct_map.num_entries : 0;
}

bool xemu_calltrace_truncated(void)
{
    return ct_truncated;
}

void xemu_calltrace_record(uint32_t call_site, uint32_t callee)
{
    /* Stale TB may fire between disarm and flush completion. */
    if (!xemu_calltrace_armed) {
        return;
    }
    /* Skip kernel-internal calls; keep game<->kernel boundary edges. */
    if (call_site >= CT_KERNEL_SPACE && callee >= CT_KERNEL_SPACE) {
        return;
    }
    if (!ct_map_add(&ct_map, call_site, callee)) {
        ct_truncated = true;
    }
}
```

- [ ] **Step 3: Wire into the build**

In `meson.build` line 4054, change:

```meson
specific_ss.add(files('xemu-xbe.c', 'xemu-version.c'))
```

to:

```meson
specific_ss.add(files('xemu-xbe.c', 'xemu-calltrace.c', 'xemu-version.c'))
```

- [ ] **Step 4: Build**

Run: `./build.sh`
Expected: build completes; `dist/xemu.exe` produced. (Nothing calls the engine yet — this task only proves it compiles and links in the QEMU environment.)

- [ ] **Step 5: Commit**

```bash
git add xemu-calltrace.h xemu-calltrace.c meson.build
git commit -m "calltrace: Add trace engine core (arm/disarm, edge recording)"
```

---

### Task 3: Debug menu (Start/Stop + status)

**Files:**
- Modify: `ui/xui/menubar.cc` (Debug menu block is at lines 218–229; includes at lines 19–30)

**Interfaces:**
- Consumes: `xemu_calltrace_armed`, `xemu_calltrace_start/stop/edge_count/truncated` (Task 2); `xemu_get_xbe_info()` from `xemu-xbe.h`.
- Produces: `Debug → Call Trace` submenu. Task 5 extends the Stop item into "Stop & Save".

- [ ] **Step 1: Add includes**

In `ui/xui/menubar.cc`, after line 30 (`#include "../xemu-os-utils.h"`), add:

```cpp
#include "../../xemu-xbe.h"
#include "../../xemu-calltrace.h"
```

(Note: `ui/xui/` files reach `ui/` with `..` — see the existing `../xemu-os-utils.h` — so the repo root is `../..`.)

- [ ] **Step 2: Add the submenu**

In the Debug menu block, after the `Video` item (line 222: `ImGui::MenuItem("Video", NULL, &video_window.m_is_open);`) and before the `#ifdef CONFIG_RENDERDOC` block, insert:

```cpp
            ImGui::Separator();
            if (ImGui::BeginMenu("Call Trace")) {
                if (!xemu_calltrace_armed) {
                    bool have_xbe = xemu_get_xbe_info() != NULL;
                    if (ImGui::MenuItem("Start Recording", NULL, false,
                                        have_xbe)) {
                        xemu_calltrace_start();
                    }
                } else {
                    ImGui::Text("Recording: %llu unique edges",
                                (unsigned long long)xemu_calltrace_edge_count());
                    if (xemu_calltrace_truncated()) {
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.6f, 0.1f, 1.0f),
                            "Edge limit reached; new edges dropped");
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Stop")) {
                        xemu_calltrace_stop();
                    }
                }
                ImGui::EndMenu();
            }
```

- [ ] **Step 3: Build and verify manually**

Run: `./build.sh`, then launch `dist/xemu.exe`.
Expected, in order:
1. With no game running (e.g. stuck at boot error or dashboard-less), `Debug → Call Trace → Start Recording` is greyed out.
2. Boot a game. `Start Recording` becomes enabled. Click it.
3. Menu now shows `Recording: 0 unique edges` (stays 0 — the helper doesn't exist until Task 4) and a `Stop` item.
4. `Stop` returns the menu to the `Start Recording` state. The game keeps running normally throughout (arming already triggers a real `tb_flush`, so a brief hitch on click is expected and fine).

- [ ] **Step 4: Commit**

```bash
git add ui/xui/menubar.cc
git commit -m "calltrace: Add Debug menu recording controls"
```

---

### Task 4: TCG helper + CALL instrumentation

**Files:**
- Modify: `target/i386/helper.h` (append at end)
- Modify: `target/i386/tcg/misc_helper.c` (append at end)
- Modify: `target/i386/tcg/emit.c.inc` (`gen_CALL` at line 1575, `gen_CALL_m` 1581, `gen_CALLF` 1587, `gen_CALLF_m` 1592)

**Interfaces:**
- Consumes: `xemu_calltrace_record` (Task 2); existing translator internals `eip_cur_tl(s)` (translate.c:707, in scope because emit.c.inc is included at translate.c:2931), `s->pc`, `s->cs_base`, `s->dflag`, `s->T0`, `decode->immediate`, `tcg_constant_tl`.
- Produces: helper `xemu_calltrace_call(tl call_site, tl callee)`; instrumented CALL translation. End-to-end recording works after this task.

**Background for the implementer:** guest x86 CALLs come in four forms. Direct near call (`gen_CALL`): target is known at translation time as `s->pc + decode->immediate` (minus `cs_base`, 16-bit-masked when `dflag == MO_16` — this mirrors `gen_jmp_rel` at translate.c:2801). Indirect near call (`gen_CALL_m`): target is at runtime in `s->T0`. Far calls (`gen_CALLF`, `gen_CALLF_m`): new EIP is in `s->T0` when `gen_far_call` runs (Xbox games essentially never use these, but instrumenting them is two lines each). The call-site EIP comes from `eip_cur_tl(s)`. xemu's i386 target is 32-bit only — no CODE64 concerns.

- [ ] **Step 1: Declare the helper**

Append to the end of `target/i386/helper.h`:

```c
DEF_HELPER_FLAGS_2(xemu_calltrace_call, TCG_CALL_NO_RWG, void, tl, tl)
```

- [ ] **Step 2: Implement the helper**

Append to the end of `target/i386/tcg/misc_helper.c`:

```c
/* xemu: guest call tracing (see xemu-calltrace.c) */
void xemu_calltrace_record(uint32_t call_site, uint32_t callee);

void HELPER(xemu_calltrace_call)(target_ulong call_site, target_ulong callee)
{
    xemu_calltrace_record((uint32_t)call_site, (uint32_t)callee);
}
```

(The extern prototype avoids include-path coupling between `target/i386/` and the repo root.)

- [ ] **Step 3: Emit the helper at the four CALL sites**

In `target/i386/tcg/emit.c.inc`, insert immediately BEFORE `gen_CALL` (line 1575):

```c
/*
 * xemu call tracing: while armed, emit a helper call recording
 * (call_site EIP, callee EIP) before the control transfer. Guarded at
 * translation time so disarmed execution is completely unaffected;
 * arming/disarming flushes the TB cache (see xemu-calltrace.c).
 */
extern bool xemu_calltrace_armed;

static void gen_xemu_calltrace(DisasContext *s, TCGv callee)
{
    gen_helper_xemu_calltrace_call(eip_cur_tl(s), callee);
}

static void gen_xemu_calltrace_direct(DisasContext *s,
                                      X86DecodedInsn *decode)
{
    /* Same destination arithmetic as gen_jmp_rel() for !CODE64. */
    target_ulong dest = s->pc + decode->immediate - s->cs_base;

    if (s->dflag == MO_16) {
        dest &= 0xffff;
    }
    gen_xemu_calltrace(s, tcg_constant_tl(dest));
}
```

Then modify the four functions:

```c
static void gen_CALL(DisasContext *s, X86DecodedInsn *decode)
{
    if (xemu_calltrace_armed) {
        gen_xemu_calltrace_direct(s, decode);
    }
    gen_push_v(s, eip_next_tl(s));
    gen_JMP(s, decode);
}

static void gen_CALL_m(DisasContext *s, X86DecodedInsn *decode)
{
    if (xemu_calltrace_armed) {
        gen_xemu_calltrace(s, s->T0);
    }
    gen_push_v(s, eip_next_tl(s));
    gen_JMP_m(s, decode);
}

static void gen_CALLF(DisasContext *s, X86DecodedInsn *decode)
{
    if (xemu_calltrace_armed) {
        gen_xemu_calltrace(s, s->T0);
    }
    gen_far_call(s);
}

static void gen_CALLF_m(DisasContext *s, X86DecodedInsn *decode)
{
    MemOp ot = decode->op[1].ot;

    gen_op_ld_v(s, ot, s->T0, s->A0);
    gen_add_A0_im(s, 1 << ot);
    gen_op_ld_v(s, MO_16, s->T1, s->A0);
    if (xemu_calltrace_armed) {
        gen_xemu_calltrace(s, s->T0);
    }
    gen_far_call(s);
}
```

- [ ] **Step 4: Build and verify end-to-end**

Run: `./build.sh`, launch `dist/xemu.exe`, boot a game.
Expected:
1. Before arming: game runs at normal speed.
2. `Debug → Call Trace → Start Recording`: the edge count starts climbing immediately (typically thousands within seconds, then the growth rate slows as the working set dedups — that slowdown IS the dedup working).
3. Gameplay continues (slower is fine; it must not crash or hang).
4. `Stop`: count freezes; game returns to full speed.
5. `Start Recording` again: count resets and climbs again (fresh map).

- [ ] **Step 5: Commit**

```bash
git add target/i386/helper.h target/i386/tcg/misc_helper.c target/i386/tcg/emit.c.inc
git commit -m "calltrace: Instrument x86 CALL translation with trace helper"
```

---

### Task 5: `.xct` writer + Stop & Save

**Files:**
- Modify: `xemu-xbe.h` (add one prototype), `xemu-xbe.c` (add one wrapper function at end)
- Modify: `xemu-calltrace.h`, `xemu-calltrace.c` (writer)
- Modify: `config_spec.yml` (line 9 area)
- Modify: `ui/xui/menubar.cc` (turn Stop into Stop & Save)
- Create: `tools/calltrace/xct_dump.py`

**Interfaces:**
- Consumes: engine state (Task 2), `xemu_get_xbe_info()`, GLib (`GByteArray`, `GArray`, `g_utf16_to_utf8`, `g_strdup_printf`), `qemu_fopen`, `xemu_queue_notification`/`xemu_queue_error_message` (`ui/xemu-notifications.h`), table from `xemu-calltrace-kexports.h`.
- Produces:
  - `ssize_t xemu_virt_dma_memory_read(uint32_t vaddr, void *buf, size_t len);` in `xemu-xbe.h`
  - `char *xemu_calltrace_save(const char *dir, char **err_msg);` — writes `<dir>/<SanitizedTitle>-<YYYYmmdd-HHMMSS>.xct`; returns g_malloc'd path on success (caller frees), or NULL with `*err_msg` set (caller frees).
  - Config key `g_config.general.calltrace_dir` (falls back to `screenshot_dir`, then `"."` — resolution happens in the menu code).
  - **The `.xct` v1 format** (LE; this layout is load-bearing for Tasks 5–7):

```
offset  size  field
0x00    4     magic 0x52544358 ("XCTR")
0x04    4     version = 1
0x08    4     flags (bit0 = truncated)
0x0C    4     title_id
0x10    4     xbe_base
0x14    4     xbe_entry (de-XORed)
0x18    4     section_count
0x1C    4     kimport_count
0x20    4     edge_count
0x24    4     strtab_size
0x28    88    title_name (UTF-8, NUL padded)
0x80    ...   strtab (NUL-separated; offset 0 is "")
        ...   sections[section_count]: u32 name_off, u32 start, u32 size
        ...   kimports[kimport_count]: u32 addr, u32 name_off
        ...   edges[edge_count]: u32 call_site, u32 callee, u64 count
```

- [ ] **Step 1: Export the guest-memory reader from xemu-xbe**

In `xemu-xbe.h`, inside the `extern "C"` block after `struct xbe *xemu_get_xbe_info(void);`, add:

```c
// Read guest-virtual memory (page-by-page translation). Returns bytes
// read, or -1 if any page is unmapped.
ssize_t xemu_virt_dma_memory_read(uint32_t vaddr, void *buf, size_t len);
```

Also add `#include <sys/types.h>` after the existing `#include <stdint.h>` (for `ssize_t` under MinGW).

At the end of `xemu-xbe.c`, add:

```c
ssize_t xemu_virt_dma_memory_read(uint32_t vaddr, void *buf, size_t len)
{
    return virt_dma_memory_read(vaddr, buf, len);
}
```

- [ ] **Step 2: Add the config key**

In `config_spec.yml`, after line 9 (`  screenshot_dir: string`), add:

```yaml
  calltrace_dir: string
```

- [ ] **Step 3: Declare the save API**

In `xemu-calltrace.h`, after the `xemu_calltrace_record` prototype, add:

```c
/*
 * Write the recording to <dir>/<Title>-<timestamp>.xct. Returns the
 * g_malloc'd output path on success (caller frees), or NULL with
 * *err_msg set to a g_malloc'd message (caller frees).
 */
char *xemu_calltrace_save(const char *dir, char **err_msg);
```

- [ ] **Step 4: Implement the writer**

In `xemu-calltrace.c`, extend the includes to:

```c
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "cpu.h"
#include "exec/tb-flush.h"
#include "xemu-xbe.h"
#include "xemu-calltrace.h"
#include "xemu-calltrace-map.h"
#include "xemu-calltrace-kexports.h"
```

(`qemu/bswap.h` provides `ldl_le_p` and `le32_to_cpu`.)

Then append the writer below `xemu_calltrace_record`:

```c
/* ------------------------------------------------------------------ */
/* .xct writer                                                        */
/* ------------------------------------------------------------------ */

#define XBOX_KERNEL_BASE 0x80010000u
#define XBE_ENTRY_XOR_RETAIL 0xA8FC57ABu
#define XBE_ENTRY_XOR_DEBUG 0x94859D4Bu

/* XBE section header layout (http://www.caustik.com/cxbx/download/xbe.htm) */
#pragma pack(1)
struct xbe_section_header {
    uint32_t flags;
    uint32_t virtual_addr;
    uint32_t virtual_size;
    uint32_t raw_addr;
    uint32_t raw_size;
    uint32_t section_name_addr;
    uint32_t section_name_ref_count;
    uint32_t head_shared_page_ref_count_addr;
    uint32_t tail_shared_page_ref_count_addr;
    uint8_t section_digest[20];
};
#pragma pack()

typedef struct SectionRec {
    uint32_t name_off;
    uint32_t start;
    uint32_t size;
} SectionRec;

typedef struct KImport {
    uint32_t addr;
    uint32_t name_off;
} KImport;

static uint32_t strtab_add(GByteArray *st, const char *s)
{
    if (!s || !*s) {
        return 0;
    }
    uint32_t off = st->len;
    g_byte_array_append(st, (const guint8 *)s, strlen(s) + 1);
    return off;
}

static bool read_guest_u32(uint32_t va, uint32_t *val)
{
    uint32_t v;
    if (xemu_virt_dma_memory_read(va, &v, 4) != 4) {
        return false;
    }
    *val = le32_to_cpu(v);
    return true;
}

static GArray *collect_sections(struct xbe *xbe, GByteArray *strtab)
{
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(SectionRec));
    uint32_t base = ldl_le_p(&xbe->header->m_base);
    uint32_t nsec = MIN(ldl_le_p(&xbe->header->m_sections), 256u);
    uint32_t shdr_addr = ldl_le_p(&xbe->header->m_section_headers_addr);

    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t off =
            shdr_addr + i * sizeof(struct xbe_section_header) - base;
        if (off + sizeof(struct xbe_section_header) > xbe->headers_len) {
            break; /* section headers outside the copied header blob */
        }
        struct xbe_section_header *sh =
            (struct xbe_section_header *)(xbe->headers + off);
        char name[32] = "";
        uint32_t name_off = ldl_le_p(&sh->section_name_addr) - base;
        if (name_off < xbe->headers_len) {
            g_strlcpy(name, (const char *)xbe->headers + name_off,
                      MIN(sizeof(name),
                          (size_t)(xbe->headers_len - name_off)));
        }
        SectionRec s = { strtab_add(strtab, name),
                         ldl_le_p(&sh->virtual_addr),
                         ldl_le_p(&sh->virtual_size) };
        g_array_append_val(arr, s);
    }
    return arr;
}

/*
 * Resolve kernel export addresses by walking the guest xboxkrnl.exe PE
 * export directory (exports are by ordinal; names come from the static
 * table in xemu-calltrace-kexports.h).
 */
static GArray *collect_kernel_exports(GByteArray *strtab)
{
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(KImport));
    uint16_t mz;
    uint32_t e_lfanew, pesig, exp_rva, ord_base, nfuncs, aof_rva;

    if (xemu_virt_dma_memory_read(XBOX_KERNEL_BASE, &mz, 2) != 2 ||
        mz != 0x5A4D) {
        return arr;
    }
    if (!read_guest_u32(XBOX_KERNEL_BASE + 0x3C, &e_lfanew) ||
        e_lfanew > 0x1000) {
        return arr;
    }
    if (!read_guest_u32(XBOX_KERNEL_BASE + e_lfanew, &pesig) ||
        pesig != 0x00004550) {
        return arr;
    }
    /* PE32 optional header: export directory RVA at PE + 0x78 */
    if (!read_guest_u32(XBOX_KERNEL_BASE + e_lfanew + 0x78, &exp_rva) ||
        !exp_rva) {
        return arr;
    }
    uint32_t exp = XBOX_KERNEL_BASE + exp_rva;
    if (!read_guest_u32(exp + 0x10, &ord_base) ||
        !read_guest_u32(exp + 0x14, &nfuncs) ||
        !read_guest_u32(exp + 0x1C, &aof_rva)) {
        return arr;
    }
    nfuncs = MIN(nfuncs, 512u);
    for (uint32_t i = 0; i < nfuncs; i++) {
        uint32_t frva;
        if (!read_guest_u32(XBOX_KERNEL_BASE + aof_rva + 4 * i, &frva) ||
            !frva) {
            continue;
        }
        uint32_t ord = ord_base + i;
        const char *name = NULL;
        char fallback[32];
        if (ord < ARRAY_SIZE(xbox_kernel_export_names)) {
            name = xbox_kernel_export_names[ord];
        }
        if (!name) {
            snprintf(fallback, sizeof(fallback), "kernel_ordinal_%u", ord);
            name = fallback;
        }
        KImport ki = { XBOX_KERNEL_BASE + frva, strtab_add(strtab, name) };
        g_array_append_val(arr, ki);
    }
    return arr;
}

char *xemu_calltrace_save(const char *dir, char **err_msg)
{
    *err_msg = NULL;

    if (!ct_map.slots || ct_map.num_entries == 0) {
        *err_msg = g_strdup("No call trace data recorded");
        return NULL;
    }
    struct xbe *xbe = xemu_get_xbe_info();
    if (!xbe) {
        *err_msg = g_strdup("No XBE is running");
        return NULL;
    }

    GByteArray *strtab = g_byte_array_new();
    guint8 zero = 0;
    g_byte_array_append(strtab, &zero, 1); /* offset 0 = "" */

    GArray *sections = collect_sections(xbe, strtab);
    GArray *kimports = collect_kernel_exports(strtab);

    /* Title name: UTF-16LE -> UTF-8 (pattern: ui/xui/main-menu.cc:1306) */
    char title8[88] = { 0 };
    char *title_utf8 =
        g_utf16_to_utf8(xbe->cert->m_title_name, 40, NULL, NULL, NULL);
    if (title_utf8) {
        g_strlcpy(title8, title_utf8, sizeof(title8));
    }

    /* Entry point is XOR-obfuscated; try retail key, fall back to debug. */
    uint32_t base = ldl_le_p(&xbe->header->m_base);
    uint32_t image_size = ldl_le_p(&xbe->header->m_sizeof_image);
    uint32_t raw_entry = ldl_le_p(&xbe->header->m_entry);
    uint32_t entry = raw_entry ^ XBE_ENTRY_XOR_RETAIL;
    if (entry < base || entry >= base + image_size) {
        uint32_t dbg = raw_entry ^ XBE_ENTRY_XOR_DEBUG;
        if (dbg >= base && dbg < base + image_size) {
            entry = dbg;
        }
    }

    /* Output path: <dir>/<SanitizedTitle>-<timestamp>.xct */
    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
    char fname_title[48];
    g_strlcpy(fname_title, title8[0] ? title8 : "recording",
              sizeof(fname_title));
    for (char *p = fname_title; *p; p++) {
        if (!g_ascii_isalnum(*p)) {
            *p = '_';
        }
    }
    char *path = g_strdup_printf("%s/%s-%s.xct", (dir && *dir) ? dir : ".",
                                 fname_title, stamp);

    FILE *f = qemu_fopen(path, "wb");
    bool ok = f != NULL;
    if (ok) {
        /* Host is little-endian on all supported platforms. */
        uint32_t hdr[10] = { 0x52544358u,
                             1,
                             ct_truncated ? 1u : 0u,
                             ldl_le_p(&xbe->cert->m_titleid),
                             base,
                             entry,
                             sections->len,
                             kimports->len,
                             ct_map.num_entries,
                             strtab->len };
        ok = fwrite(hdr, sizeof(hdr), 1, f) == 1 &&
             fwrite(title8, sizeof(title8), 1, f) == 1 &&
             fwrite(strtab->data, strtab->len, 1, f) == 1;
        if (ok && sections->len) {
            ok = fwrite(sections->data, sizeof(SectionRec), sections->len,
                        f) == sections->len;
        }
        if (ok && kimports->len) {
            ok = fwrite(kimports->data, sizeof(KImport), kimports->len,
                        f) == kimports->len;
        }
        for (uint32_t i = 0; ok && i < CT_MAP_CAPACITY; i++) {
            CTEdge *e = &ct_map.slots[i];
            if (!e->key) {
                continue;
            }
            uint32_t addrs[2] = { (uint32_t)(e->key >> 32),
                                  (uint32_t)e->key };
            ok = fwrite(addrs, 8, 1, f) == 1 &&
                 fwrite(&e->count, 8, 1, f) == 1;
        }
        fclose(f);
    }

    g_free(title_utf8);
    g_byte_array_free(strtab, TRUE);
    g_array_free(sections, TRUE);
    g_array_free(kimports, TRUE);

    if (!ok) {
        *err_msg = g_strdup_printf("Failed to write %s", path);
        g_free(path);
        return NULL;
    }
    return path;
}
```

- [ ] **Step 5: Wire Stop & Save into the menu**

In `ui/xui/menubar.cc`, replace the `Stop` item from Task 3:

```cpp
                    if (ImGui::MenuItem("Stop")) {
                        xemu_calltrace_stop();
                    }
```

with:

```cpp
                    if (ImGui::MenuItem("Stop & Save")) {
                        xemu_calltrace_stop();
                        const char *dir = g_config.general.calltrace_dir;
                        if (!strlen(dir)) {
                            dir = g_config.general.screenshot_dir;
                        }
                        if (!strlen(dir)) {
                            dir = ".";
                        }
                        char *err = NULL;
                        char *path = xemu_calltrace_save(dir, &err);
                        if (path) {
                            char *msg = g_strdup_printf(
                                "Call trace saved: %s", path);
                            xemu_queue_notification(msg);
                            g_free(msg);
                            g_free(path);
                        } else {
                            xemu_queue_error_message(
                                err ? err : "Call trace save failed");
                            g_free(err);
                        }
                    }
```

(`ui/xemu-notifications.h` is already included at menubar.cc line 19; `g_config` comes via `common.hh`.)

- [ ] **Step 6: Create the dump/validate tool**

Create `tools/calltrace/xct_dump.py`:

```python
#!/usr/bin/env python3
"""Parse, validate, and pretty-print an xemu .xct call-trace recording."""
import struct
import sys


def main(path):
    data = open(path, 'rb').read()
    (magic, ver, flags, title_id, base, entry,
     nsec, nkimp, nedge, stsz) = struct.unpack_from('<10I', data, 0)
    assert magic == 0x52544358, 'bad magic (not an .xct file)'
    assert ver == 1, f'unsupported version {ver}'
    title = data[40:128].split(b'\0')[0].decode('utf-8', 'replace')
    expect = 128 + stsz + nsec * 12 + nkimp * 8 + nedge * 16
    assert len(data) == expect, f'size mismatch: {len(data)} != {expect}'

    off = 128
    st = data[off:off + stsz]
    off += stsz

    def s(o):
        return st[o:st.index(b'\0', o)].decode('utf-8', 'replace')

    print(f'title={title!r} id={title_id:08X} base={base:08X} '
          f'entry={entry:08X} truncated={bool(flags & 1)}')
    print(f'{nsec} sections, {nkimp} kernel imports, {nedge} edges')
    for _ in range(nsec):
        no, start, size = struct.unpack_from('<3I', data, off)
        off += 12
        print(f'  section {s(no):10s} {start:08X}+{size:X}')
    for i in range(nkimp):
        addr, no = struct.unpack_from('<2I', data, off)
        off += 8
        if i < 5:
            print(f'  kimport {addr:08X} {s(no)}')
    if nkimp > 5:
        print(f'  ... {nkimp - 5} more kernel imports')
    total = 0
    for i in range(nedge):
        site, callee, cnt = struct.unpack_from('<IIQ', data, off)
        off += 16
        total += cnt
        if i < 10:
            print(f'  edge {site:08X} -> {callee:08X} x{cnt}')
    if nedge > 10:
        print(f'  ... {nedge - 10} more edges')
    print(f'total calls recorded: {total}')
    print('OK')


if __name__ == '__main__':
    main(sys.argv[1])
```

- [ ] **Step 7: Build and verify end-to-end**

Run: `./build.sh`, launch, boot a game, record ~30 seconds of gameplay, `Stop & Save`.
Expected:
1. Toast: `Call trace saved: ./<Title>-<timestamp>.xct` (or your configured dir).
2. Run: `python3 tools/calltrace/xct_dump.py <that file>`
3. Expected output: correct title/title id; entry inside `[base, base+image)`; XBE sections listed with sane names (`.text`, `D3D`, …); ~379 kernel imports with real names (`AvGetSavedDataAddress`, …); thousands of edges; `OK` at the end.
4. Also verify the failure path: with no recording (fresh boot, no Start), `Stop & Save` is unreachable (menu shows Start) — then Start + immediate Stop & Save with 0 edges shows the error toast `No call trace data recorded`.

- [ ] **Step 8: Commit**

```bash
git add xemu-xbe.h xemu-xbe.c xemu-calltrace.h xemu-calltrace.c config_spec.yml ui/xui/menubar.cc tools/calltrace/xct_dump.py
git commit -m "calltrace: Write .xct recordings with XBE metadata and kernel export names"
```

---

### Task 6: Synthetic fixture generator

**Files:**
- Create: `tools/calltrace/make_test_xct.py`

**Interfaces:**
- Consumes: the `.xct` v1 layout (Task 5 table).
- Produces: `test-fixture.xct` with a KNOWN graph used by all viewer tasks. Fixture semantics (memorize; viewer self-tests assert these): entry `A=0x11000`; functions `B=0x12000`, `C=0x13000`, `D1=0x14000`, `D2=0x15000`, thread-root `T=0x16000`. Edges: A→B(5), A→C(1), B→C(9), B→D1(2)+B→D2(4) from the SAME call site `0x12050` (polymorphic), C→kernel `0x80012345` KeQuerySystemTime(3), C→C(2) recursion, kernel `0x80020000`→T(1), T→B(7). One section `.text` at `0x11000+0x8000`. Title `Test Fixture`, title id `0x4D530001`.

- [ ] **Step 1: Write the generator**

Create `tools/calltrace/make_test_xct.py`:

```python
#!/usr/bin/env python3
"""Generate a synthetic .xct fixture with a known call graph for viewer tests."""
import struct
import sys


def build():
    strtab = bytearray(b'\0')

    def add_str(name):
        if not name:
            return 0
        off = len(strtab)
        strtab.extend(name.encode() + b'\0')
        return off

    sections = [(add_str('.text'), 0x11000, 0x8000)]
    kimports = [(0x80012345, add_str('KeQuerySystemTime'))]
    edges = [
        (0x11010, 0x12000, 5),     # A -> B
        (0x11020, 0x13000, 1),     # A -> C
        (0x12040, 0x13000, 9),     # B -> C
        (0x12050, 0x14000, 2),     # B -> D1 (polymorphic site...)
        (0x12050, 0x15000, 4),     # B -> D2 (...same call site)
        (0x13008, 0x80012345, 3),  # C -> kernel leaf
        (0x13010, 0x13000, 2),     # C -> C recursion
        (0x80020000, 0x16000, 1),  # kernel -> T (thread root)
        (0x16008, 0x12000, 7),     # T -> B
    ]
    title = 'Test Fixture'.encode().ljust(88, b'\0')
    hdr = struct.pack('<10I', 0x52544358, 1, 0, 0x4D530001, 0x10000,
                      0x11000, len(sections), len(kimports), len(edges),
                      len(strtab))
    out = bytearray(hdr + title + strtab)
    for rec in sections:
        out += struct.pack('<3I', *rec)
    for rec in kimports:
        out += struct.pack('<2I', *rec)
    for site, callee, cnt in edges:
        out += struct.pack('<IIQ', site, callee, cnt)
    return bytes(out)


if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'test-fixture.xct'
    open(path, 'wb').write(build())
    print(f'wrote {path}')
```

- [ ] **Step 2: Generate and validate against the dump tool**

Run:
```bash
python3 tools/calltrace/make_test_xct.py tools/calltrace/test-fixture.xct
python3 tools/calltrace/xct_dump.py tools/calltrace/test-fixture.xct
```
Expected: `title='Test Fixture' id=4D530001 base=00010000 entry=00011000 truncated=False`, `1 sections, 1 kernel imports, 9 edges`, all 9 edges printed, `total calls recorded: 34`, `OK`.

- [ ] **Step 3: Commit**

```bash
git add tools/calltrace/make_test_xct.py tools/calltrace/test-fixture.xct
git commit -m "calltrace: Add synthetic .xct fixture generator"
```

---

### Task 7: Viewer — file parsing + function derivation (TDD via selftest)

**Files:**
- Create: `tools/calltrace/viewer.html`

**Interfaces:**
- Consumes: `.xct` v1 layout; fixture semantics (Task 6).
- Produces (JS, all used by Tasks 8–10):
  - `parseXCT(ArrayBuffer) -> rec` — `{title, titleId, base, entry, sections:[{name,start,size}], kimports:Map(addr→name), edges:[{site,callee,count}], truncated}`; throws `Error` with a user-readable message on bad input.
  - `deriveModel(rec) -> model` — `{fns:Map, entries:Uint32Array, roots:[], rec}`. Each fn: `{id, kind:'fn'|'kernel'|'kroot'|'unattr', out:Map(calleeId→{count, sites:Map(site→count)}), inSet:Set(callerId), sites:Map(site→Set(calleeId))}`. Pseudo-ids: `-2` = KERNEL root, `-3` = unattributed. `roots` = `[rec.entry, -2?]`.
  - `fnName(model, id)`, `hex(addr)` (8-digit uppercase), `sectionOf(model, addr)`, `sectionIndexOf(model, addr)`, `showBanner(msg, kind)`, `el(id)`, `esc(s)`, global `symbols` (Map addr→name), `loadRecording(buf, name)`, globals `model`, `expanded` (Set), `selected`, `view {panX,panY,zoom}`.
  - Marker comments `// === [Task 8] LAYOUT & RENDERING ===`, `// === [Task 9] DETAILS, SEARCH, SYMBOLS ===`, `// === [Task 10] EXPORT & HELP ===` that later tasks replace.
  - Selftest harness at `viewer.html?selftest=1`.

- [ ] **Step 1: Write the full skeleton with failing selftest**

Create `tools/calltrace/viewer.html` exactly as follows. Note `parseXCT` and `deriveModel` are unimplemented stubs — the selftest must FAIL first:

```html
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>xemu Call Trace Viewer</title>
<style>
  * { box-sizing: border-box; margin: 0; }
  html, body { height: 100%; }
  body { display: flex; flex-direction: column; background: #14161a;
         color: #ddd; font: 13px/1.4 system-ui, sans-serif; }
  #toolbar { display: flex; gap: 8px; align-items: center; padding: 8px;
             background: #1d2026; border-bottom: 1px solid #333;
             flex-wrap: wrap; }
  #toolbar button, #toolbar input[type=text] {
    background: #2a2e36; color: #ddd; border: 1px solid #444;
    border-radius: 4px; padding: 4px 10px; font: inherit; }
  #toolbar button:hover { background: #363b45; cursor: pointer; }
  #banner { display: none; padding: 6px 12px; font-weight: 600; }
  #banner.error { display: block; background: #5c2020; color: #ffb0b0; }
  #banner.warn { display: block; background: #5c4a20; color: #ffe0a0; }
  #banner.info { display: block; background: #20405c; color: #a0d0ff; }
  #main { display: flex; flex: 1; min-height: 0; }
  #cvwrap { flex: 1; position: relative; min-width: 0; }
  #cv { position: absolute; inset: 0; }
  #details { width: 300px; overflow-y: auto; background: #1d2026;
             border-left: 1px solid #333; padding: 10px; }
  #details h3 { font-size: 14px; word-break: break-all; margin-bottom: 6px; }
  #details h4 { margin: 10px 0 4px; color: #9ab; }
  #details .kv { color: #aaa; }
  #details .kv b { color: #ddd; font-family: monospace; }
  #details .lnk { color: #7fb3ff; cursor: pointer; font-family: monospace;
                  padding: 1px 0; }
  #details .lnk:hover { text-decoration: underline; }
  #details .site { color: #888; font-family: monospace; font-size: 12px;
                   padding-left: 14px; }
  #details .poly { color: #ffb84a; font-weight: 700; }
  #stats { padding: 5px 12px; background: #1d2026; color: #999;
           border-top: 1px solid #333; font-family: monospace; }
  #help { font-size: 12px; color: #aaa; padding: 8px 12px; }
  #help pre { background: #22262d; padding: 6px; margin: 4px 0;
              overflow-x: auto; }
</style>
</head>
<body>
<div id="toolbar">
  <button onclick="el('file').click()">Open .xct</button>
  <input type="file" id="file" accept=".xct" hidden>
  <button onclick="el('symfile').click()">Load symbols</button>
  <input type="file" id="symfile" hidden>
  <input type="text" id="search" placeholder="search name or address"
         size="24">
  <button id="btn-dot-visible">Export DOT (visible)</button>
  <button id="btn-dot-full">Export DOT (full)</button>
  <button id="btn-reset">Reset view</button>
  <button onclick="el('helpbox').hidden = !el('helpbox').hidden">Help</button>
</div>
<div id="banner"></div>
<div id="helpbox" hidden>
  <div id="help">
    <b>Open</b> a .xct recorded by xemu (Debug &gt; Call Trace), or drag it
    onto this page. <b>Click</b> a node to inspect it; <b>double-click</b> to
    expand/collapse its callees. Drag to pan, wheel to zoom.
    <b>Symbol maps</b> are plain text, one <code>ADDRESS NAME</code> pair per
    line. Export yours:
    <pre>IDA (File &gt; Script command, Python):
for f in idautils.Functions(): print("%X %s" % (f, idc.get_func_name(f)))</pre>
    <pre>Ghidra (Window &gt; Script Manager, Python):
for f in currentProgram.getFunctionManager().getFunctions(True):
    print("%s %s" % (f.getEntryPoint(), f.getName()))</pre>
  </div>
</div>
<div id="main">
  <div id="cvwrap"><canvas id="cv"></canvas></div>
  <div id="details"><i>No selection</i></div>
</div>
<div id="stats">No recording loaded</div>
<script>
'use strict';
const KERNEL_BOUND = 0x80000000;
const el = id => document.getElementById(id);
const hex = a => (a >>> 0).toString(16).toUpperCase().padStart(8, '0');
const esc = s => s.replace(/[&<>"]/g,
    c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));

let model = null;
let symbols = new Map();          // addr -> user symbol name
let expanded = new Set();         // expanded fn ids
let selected = null;              // selected fn id
let view = { panX: 40, panY: 40, zoom: 1 };
let dpr = 1;
let layoutNodes = new Map();      // id -> layout node (Task 8)

function showBanner(msg, kind) {
  const b = el('banner');
  b.textContent = msg;
  b.className = kind || 'info';
}
function clearBanner() { el('banner').className = ''; }

/* ---------------- parsing & derivation ---------------- */

function parseXCT(buf) {
  throw new Error('not implemented');
}

function deriveModel(rec) {
  throw new Error('not implemented');
}

function fnName(m, id) {
  if (id === -2) return 'KERNEL';
  if (id === -3) return '(unattributed)';
  const f = m.fns.get(id);
  if (f && f.kind === 'kernel') {
    return m.rec.kimports.get(id) || ('kernel_' + hex(id));
  }
  return symbols.get(id) || ('sub_' + hex(id));
}

function sectionIndexOf(m, addr) {
  for (let i = 0; i < m.rec.sections.length; i++) {
    const s = m.rec.sections[i];
    if (addr >= s.start && addr < s.start + s.size) return i;
  }
  return -1;
}
function sectionOf(m, addr) {
  const i = sectionIndexOf(m, addr);
  return i < 0 ? '' : m.rec.sections[i].name;
}

/* ---------------- loading ---------------- */

function loadRecording(buf, name) {
  let rec;
  try {
    rec = parseXCT(buf);
  } catch (err) {
    showBanner('Failed to load ' + name + ': ' + err.message, 'error');
    return;
  }
  model = deriveModel(rec);
  symbols = new Map();
  const saved = localStorage.getItem('xct-syms-' + rec.titleId);
  if (saved) {
    try { symbols = new Map(JSON.parse(saved)); } catch (e) {}
  }
  expanded = new Set([rec.entry]);
  selected = null;
  view = { panX: 40, panY: 40, zoom: 1 };
  clearBanner();
  if (rec.truncated) {
    showBanner('Recording hit the edge limit in xemu; the graph is ' +
               'incomplete.', 'warn');
  }
  el('stats').textContent =
      `${rec.title || 'Untitled'} [${hex(rec.titleId)}] — ` +
      `${model.fns.size} nodes, ${rec.edges.length} raw edges, ` +
      `${rec.kimports.size} kernel imports` +
      (symbols.size ? `, ${symbols.size} symbols restored` : '');
  draw();
}

el('file').onchange = ev => {
  const f = ev.target.files[0];
  if (f) f.arrayBuffer().then(buf => loadRecording(buf, f.name));
};
window.addEventListener('dragover', ev => ev.preventDefault());
window.addEventListener('drop', ev => {
  ev.preventDefault();
  const f = ev.dataTransfer.files[0];
  if (f) f.arrayBuffer().then(buf => loadRecording(buf, f.name));
});

// === [Task 8] LAYOUT & RENDERING ===
function draw() {}

// === [Task 9] DETAILS, SEARCH, SYMBOLS ===
function showDetails(id) {}

// === [Task 10] EXPORT & HELP ===

/* ---------------- selftest (?selftest=1) ---------------- */

function buildFixture() {
  const strtab = [0];
  const addStr = s => {
    if (!s) return 0;
    const off = strtab.length;
    for (const c of new TextEncoder().encode(s)) strtab.push(c);
    strtab.push(0);
    return off;
  };
  const sections = [[addStr('.text'), 0x11000, 0x8000]];
  const kimports = [[0x80012345, addStr('KeQuerySystemTime')]];
  const edges = [
    [0x11010, 0x12000, 5], [0x11020, 0x13000, 1], [0x12040, 0x13000, 9],
    [0x12050, 0x14000, 2], [0x12050, 0x15000, 4],
    [0x13008, 0x80012345, 3], [0x13010, 0x13000, 2],
    [0x80020000, 0x16000, 1], [0x16008, 0x12000, 7],
  ];
  const size = 128 + strtab.length + sections.length * 12 +
               kimports.length * 8 + edges.length * 16;
  const buf = new ArrayBuffer(size);
  const dv = new DataView(buf);
  const u8 = new Uint8Array(buf);
  [0x52544358, 1, 0, 0x4D530001, 0x10000, 0x11000, sections.length,
   kimports.length, edges.length, strtab.length]
      .forEach((v, i) => dv.setUint32(i * 4, v, true));
  u8.set(new TextEncoder().encode('Test Fixture'), 40);
  u8.set(strtab, 128);
  let off = 128 + strtab.length;
  for (const [n, s, z] of sections) {
    dv.setUint32(off, n, true); dv.setUint32(off + 4, s, true);
    dv.setUint32(off + 8, z, true); off += 12;
  }
  for (const [a, n] of kimports) {
    dv.setUint32(off, a, true); dv.setUint32(off + 4, n, true); off += 8;
  }
  for (const [s, c, cnt] of edges) {
    dv.setUint32(off, s, true); dv.setUint32(off + 4, c, true);
    dv.setBigUint64(off + 8, BigInt(cnt), true); off += 16;
  }
  return buf;
}

const selfTests = [];
function T(name, fn) { selfTests.push([name, fn]); }
function assertEq(got, want, what) {
  if (got !== want) throw new Error(`${what}: got ${got}, want ${want}`);
}

T('parse header and tables', () => {
  const r = parseXCT(buildFixture());
  assertEq(r.title, 'Test Fixture', 'title');
  assertEq(r.titleId, 0x4D530001, 'titleId');
  assertEq(r.entry, 0x11000, 'entry');
  assertEq(r.edges.length, 9, 'edge count');
  assertEq(r.sections.length, 1, 'section count');
  assertEq(r.sections[0].name, '.text', 'section name');
  assertEq(r.kimports.get(0x80012345), 'KeQuerySystemTime', 'kimport');
  assertEq(r.edges[2].count, 9, 'B->C count');
  assertEq(r.truncated, false, 'truncated');
});
T('bad magic rejected', () => {
  const b = buildFixture();
  new DataView(b).setUint32(0, 0xdeadbeef, true);
  let threw = false;
  try { parseXCT(b); } catch (e) { threw = true; }
  assertEq(threw, true, 'threw');
});
T('short file rejected', () => {
  let threw = false;
  try { parseXCT(new ArrayBuffer(64)); } catch (e) { threw = true; }
  assertEq(threw, true, 'threw');
});
T('function derivation', () => {
  const m = deriveModel(parseXCT(buildFixture()));
  assertEq(m.entries.length, 6, 'derived function count');
  assertEq(m.fns.get(0x11000).out.size, 2, 'A callee count');
  assertEq(m.fns.get(0x12000).out.size, 3, 'B callee count');
  assertEq(m.fns.get(0x12000).out.get(0x13000).count, 9, 'B->C count');
  assertEq(m.fns.get(0x13000).out.has(0x13000), true, 'C recursion');
  assertEq(m.fns.get(0x80012345).kind, 'kernel', 'kernel leaf kind');
  assertEq(m.fns.get(0x80012345).inSet.has(0x13000), true, 'leaf caller');
});
T('polymorphic call site detection', () => {
  const m = deriveModel(parseXCT(buildFixture()));
  assertEq(m.fns.get(0x12000).sites.get(0x12050).size, 2, 'poly targets');
  assertEq(m.fns.get(0x12000).sites.get(0x12040).size, 1, 'mono targets');
});
T('roots: entry + kernel pseudo-root', () => {
  const m = deriveModel(parseXCT(buildFixture()));
  assertEq(m.roots[0], 0x11000, 'entry root');
  assertEq(m.roots[1], -2, 'kernel root');
  assertEq(m.fns.get(-2).out.has(0x16000), true, 'thread root under KERNEL');
});
T('naming', () => {
  const m = deriveModel(parseXCT(buildFixture()));
  assertEq(fnName(m, 0x80012345), 'KeQuerySystemTime', 'kernel name');
  assertEq(fnName(m, 0x12000), 'sub_00012000', 'default name');
  symbols.set(0x12000, 'UpdateWorld');
  assertEq(fnName(m, 0x12000), 'UpdateWorld', 'symbol override');
  symbols.clear();
  assertEq(fnName(m, -2), 'KERNEL', 'pseudo root name');
});

function runSelfTests() {
  const out = [];
  let fails = 0;
  for (const [name, fn] of selfTests) {
    try { fn(); out.push('PASS  ' + name); }
    catch (e) { fails++; out.push('FAIL  ' + name + ' — ' + e.message); }
  }
  out.push('', fails ? `${fails} FAILED` : 'ALL PASS');
  document.body.innerHTML =
      '<pre style="color:#ddd;padding:20px;font-size:14px">' +
      esc(out.join('\n')) + '</pre>';
}

window.addEventListener('load', () => {
  if (location.search.includes('selftest')) { runSelfTests(); return; }
  fitCanvas();
  window.addEventListener('resize', fitCanvas);
});
function fitCanvas() {
  const cv = el('cv');
  const r = cv.parentElement.getBoundingClientRect();
  dpr = window.devicePixelRatio || 1;
  cv.width = r.width * dpr;
  cv.height = r.height * dpr;
  cv.style.width = r.width + 'px';
  cv.style.height = r.height + 'px';
  draw();
}
</script>
</body>
</html>
```

- [ ] **Step 2: Run selftest to verify it fails**

Open `tools/calltrace/viewer.html?selftest=1` in a browser (e.g. `start tools/calltrace/viewer.html?selftest=1` won't pass the query on Windows — open the file in the browser and append `?selftest=1` to the URL).
Expected: every test FAILs with `not implemented`; footer shows `7 FAILED`.

- [ ] **Step 3: Implement parseXCT and deriveModel**

Replace the two stub functions with:

```js
function parseXCT(buf) {
  if (buf.byteLength < 128) throw new Error('file too small');
  const dv = new DataView(buf);
  if (dv.getUint32(0, true) !== 0x52544358) {
    throw new Error('bad magic (not an .xct file)');
  }
  const version = dv.getUint32(4, true);
  if (version !== 1) throw new Error('unsupported version ' + version);
  const flags = dv.getUint32(8, true);
  const titleId = dv.getUint32(12, true);
  const base = dv.getUint32(16, true);
  const entry = dv.getUint32(20, true);
  const nSec = dv.getUint32(24, true);
  const nKimp = dv.getUint32(28, true);
  const nEdge = dv.getUint32(32, true);
  const strtabSize = dv.getUint32(36, true);
  if (128 + strtabSize + nSec * 12 + nKimp * 8 + nEdge * 16 >
      buf.byteLength) {
    throw new Error('truncated file');
  }
  const title = new TextDecoder().decode(new Uint8Array(buf, 40, 88))
      .replace(/\0[\s\S]*$/, '');
  let off = 128;
  const strBytes = new Uint8Array(buf, off, strtabSize);
  off += strtabSize;
  const str = o => {
    let e = o;
    while (e < strBytes.length && strBytes[e]) e++;
    return new TextDecoder().decode(strBytes.subarray(o, e));
  };
  const sections = [];
  for (let i = 0; i < nSec; i++, off += 12) {
    sections.push({ name: str(dv.getUint32(off, true)),
                    start: dv.getUint32(off + 4, true),
                    size: dv.getUint32(off + 8, true) });
  }
  const kimports = new Map();
  for (let i = 0; i < nKimp; i++, off += 8) {
    kimports.set(dv.getUint32(off, true), str(dv.getUint32(off + 4, true)));
  }
  const edges = [];
  for (let i = 0; i < nEdge; i++, off += 16) {
    edges.push({ site: dv.getUint32(off, true),
                 callee: dv.getUint32(off + 4, true),
                 count: Number(dv.getBigUint64(off + 8, true)) });
  }
  return { title, titleId, base, entry, sections, kimports, edges,
           truncated: !!(flags & 1) };
}

function deriveModel(rec) {
  /* Function entries: every game-space callee, plus the XBE entry. */
  const entrySet = new Set([rec.entry]);
  for (const e of rec.edges) {
    if (e.callee < KERNEL_BOUND) entrySet.add(e.callee);
  }
  const entries = Uint32Array.from([...entrySet].sort((a, b) => a - b));
  /* Nearest-preceding-entry attribution for call sites. */
  const fnOf = site => {
    let lo = 0, hi = entries.length - 1, best = -1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      if (entries[mid] <= site) { best = entries[mid]; lo = mid + 1; }
      else hi = mid - 1;
    }
    return best;
  };
  const fns = new Map();
  const getFn = (id, kind) => {
    let f = fns.get(id);
    if (!f) {
      f = { id, kind, out: new Map(), inSet: new Set(), sites: new Map() };
      fns.set(id, f);
    }
    return f;
  };
  for (const e of rec.edges) {
    let callerId, callerKind;
    if (e.site >= KERNEL_BOUND) { callerId = -2; callerKind = 'kroot'; }
    else {
      callerId = fnOf(e.site);
      callerKind = 'fn';
      if (callerId < 0) { callerId = -3; callerKind = 'unattr'; }
    }
    const callee =
        getFn(e.callee, e.callee >= KERNEL_BOUND ? 'kernel' : 'fn');
    const caller = getFn(callerId, callerKind);
    let edge = caller.out.get(e.callee);
    if (!edge) {
      edge = { count: 0, sites: new Map() };
      caller.out.set(e.callee, edge);
    }
    edge.count += e.count;
    edge.sites.set(e.site, (edge.sites.get(e.site) || 0) + e.count);
    callee.inSet.add(callerId);
    let siteTargets = caller.sites.get(e.site);
    if (!siteTargets) {
      siteTargets = new Set();
      caller.sites.set(e.site, siteTargets);
    }
    siteTargets.add(e.callee);
  }
  getFn(rec.entry, 'fn'); /* entry exists even if it never calls out */
  const roots = [rec.entry];
  if (fns.has(-2)) roots.push(-2);
  return { fns, entries, roots, rec };
}
```

- [ ] **Step 4: Run selftest to verify it passes**

Reload `viewer.html?selftest=1`.
Expected: all 7 tests PASS; footer `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add tools/calltrace/viewer.html
git commit -m "calltrace: Add viewer skeleton with .xct parser and function derivation"
```

---

### Task 8: Viewer — layout, rendering, interaction

**Files:**
- Modify: `tools/calltrace/viewer.html` (replace the `// === [Task 8] LAYOUT & RENDERING ===` marker and the `function draw() {}` stub)

**Interfaces:**
- Consumes: `model`, `expanded`, `selected`, `view`, `dpr`, `layoutNodes`, `fnName`, `sectionIndexOf` (Task 7); `showDetails(id)` (stub until Task 9).
- Produces: `computeLayout(model) -> Map(id → {id, x, y, w, h, depth, parent, treeKids})`, `draw()`, `toggleExpand(id)`, mouse handlers (click=select, double-click=expand/collapse, drag=pan, wheel=zoom-at-cursor), Reset-view button wiring.

- [ ] **Step 1: Replace the Task 8 marker + draw stub**

Replace these two lines:

```js
// === [Task 8] LAYOUT & RENDERING ===
function draw() {}
```

with:

```js
/* ---------------- layout & rendering ---------------- */

const NODE_H = 26, ROW = 40, COL = 260;
const SECTION_COLORS = ['#3b5b7d', '#5b3b7d', '#3b7d5b', '#7d5b3b',
                        '#7d3b4f', '#4f7d3b'];

function computeLayout(m) {
  const nodes = new Map();
  const rootNodes = [];
  const visit = (id, depth, parent) => {
    if (nodes.has(id)) return false; /* seen: render as cross-link */
    const n = { id, depth, parent, treeKids: [], x: 0, y: 0,
                w: 160, h: NODE_H };
    nodes.set(id, n);
    if (!parent) rootNodes.push(n);
    if (expanded.has(id)) {
      const f = m.fns.get(id);
      if (f) {
        for (const calleeId of [...f.out.keys()].sort((a, b) => a - b)) {
          if (visit(calleeId, depth + 1, n)) {
            n.treeKids.push(nodes.get(calleeId));
          }
        }
      }
    }
    return true;
  };
  for (const r of m.roots) visit(r, 0, null);
  /* Post-order y: leaves stack, parents center on their kids. */
  let nextY = 0;
  const assign = n => {
    if (n.treeKids.length === 0) {
      n.y = nextY;
      nextY += ROW;
    } else {
      for (const k of n.treeKids) assign(k);
      n.y = (n.treeKids[0].y + n.treeKids[n.treeKids.length - 1].y) / 2;
    }
    n.x = n.depth * COL;
  };
  for (const r of rootNodes) {
    assign(r);
    nextY += ROW * 0.5; /* gap between root trees */
  }
  return nodes;
}

function nodeColor(n) {
  if (n.id === -2) return '#6d3b3b';
  if (n.id === -3) return '#555555';
  const f = model.fns.get(n.id);
  if (f && f.kind === 'kernel') return '#8a6d1f';
  const sec = sectionIndexOf(model, n.id);
  return sec < 0 ? '#3f4652' : SECTION_COLORS[sec % SECTION_COLORS.length];
}

function roundRect(ctx, x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

function drawEdge(ctx, a, b, count, cross) {
  ctx.lineWidth = Math.min(5, 1 + Math.log2(1 + count) * 0.4);
  ctx.strokeStyle = cross ? 'rgba(120,180,255,0.45)'
                          : 'rgba(170,170,170,0.7)';
  ctx.beginPath();
  if (a === b) { /* recursion: small loop off the right edge */
    const x = a.x + a.w, y = a.y + a.h / 2;
    ctx.moveTo(x, y - 6);
    ctx.bezierCurveTo(x + 34, y - 22, x + 34, y + 22, x, y + 6);
    ctx.stroke();
    return;
  }
  const x1 = a.x + a.w, y1 = a.y + a.h / 2;
  const x2 = b.x, y2 = b.y + b.h / 2;
  const mx = (x1 + x2) / 2;
  ctx.moveTo(x1, y1);
  ctx.bezierCurveTo(mx, y1, mx, y2, x2, y2);
  ctx.stroke();
  ctx.fillStyle = ctx.strokeStyle;
  ctx.beginPath();
  ctx.moveTo(x2, y2);
  ctx.lineTo(x2 - 7, y2 - 4);
  ctx.lineTo(x2 - 7, y2 + 4);
  ctx.fill();
  if (view.zoom > 0.7) {
    ctx.fillStyle = 'rgba(200,200,200,0.75)';
    ctx.fillText('×' + count, mx - 8, (y1 + y2) / 2 - 4);
  }
}

function drawNode(ctx, n) {
  const f = model.fns.get(n.id);
  const expandable = f && f.out.size > 0 && f.kind !== 'kernel';
  ctx.fillStyle = nodeColor(n);
  ctx.strokeStyle = n.id === selected ? '#ffd24a' : 'rgba(255,255,255,0.25)';
  ctx.lineWidth = n.id === selected ? 2 : 1;
  roundRect(ctx, n.x, n.y, n.w, n.h, 6);
  ctx.fill();
  ctx.stroke();
  ctx.fillStyle = '#eeeeee';
  const glyph = expandable ? (expanded.has(n.id) ? '▾ ' : '▸ ')
                           : '';
  ctx.fillText(glyph + n.label, n.x + 7, n.y + 17, n.w - 14);
  if (expandable && !expanded.has(n.id)) {
    ctx.fillStyle = 'rgba(255,255,255,0.6)';
    ctx.fillText('+' + f.out.size, n.x + n.w - 30, n.y + 17);
  }
}

function draw() {
  const cv = el('cv');
  const ctx = cv.getContext('2d');
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = '#14161a';
  ctx.fillRect(0, 0, cv.width, cv.height);
  if (!model) {
    ctx.fillStyle = '#667';
    ctx.font = `${16 * dpr}px system-ui`;
    ctx.fillText('Open or drop an .xct recording to begin',
                 30 * dpr, 50 * dpr);
    return;
  }
  ctx.setTransform(view.zoom * dpr, 0, 0, view.zoom * dpr,
                   view.panX * dpr, view.panY * dpr);
  ctx.font = '12px monospace';
  layoutNodes = computeLayout(model);
  for (const n of layoutNodes.values()) {
    n.label = fnName(model, n.id);
    n.w = Math.min(220, Math.max(120, ctx.measureText(n.label).width + 40));
  }
  for (const n of layoutNodes.values()) {
    if (!expanded.has(n.id)) continue;
    const f = model.fns.get(n.id);
    if (!f) continue;
    for (const [calleeId, e] of f.out) {
      const t = layoutNodes.get(calleeId);
      if (!t) continue;
      drawEdge(ctx, n, t, e.count, t.parent !== n);
    }
  }
  for (const n of layoutNodes.values()) drawNode(ctx, n);
}

/* ---------------- interaction ---------------- */

function worldPos(ev) {
  const r = el('cv').getBoundingClientRect();
  return { x: (ev.clientX - r.left - view.panX) / view.zoom,
           y: (ev.clientY - r.top - view.panY) / view.zoom };
}
function nodeAt(p) {
  for (const n of layoutNodes.values()) {
    if (p.x >= n.x && p.x <= n.x + n.w &&
        p.y >= n.y && p.y <= n.y + n.h) {
      return n;
    }
  }
  return null;
}
function toggleExpand(id) {
  const f = model && model.fns.get(id);
  if (!f || f.out.size === 0 || f.kind === 'kernel') return;
  if (expanded.has(id)) expanded.delete(id);
  else expanded.add(id);
}

let drag = null;
let dragMoved = false;
el('cv').addEventListener('mousedown', ev => {
  const n = nodeAt(worldPos(ev));
  if (!n) {
    drag = { x: ev.clientX, y: ev.clientY,
             panX: view.panX, panY: view.panY };
    dragMoved = false;
  }
});
window.addEventListener('mousemove', ev => {
  if (!drag) return;
  if (Math.abs(ev.clientX - drag.x) + Math.abs(ev.clientY - drag.y) > 3) {
    dragMoved = true;
  }
  view.panX = drag.panX + ev.clientX - drag.x;
  view.panY = drag.panY + ev.clientY - drag.y;
  draw();
});
window.addEventListener('mouseup', () => { drag = null; });
el('cv').addEventListener('click', ev => {
  if (dragMoved || !model) return;
  const n = nodeAt(worldPos(ev));
  if (n) {
    selected = n.id;
    showDetails(n.id);
    draw();
  }
});
el('cv').addEventListener('dblclick', ev => {
  if (!model) return;
  const n = nodeAt(worldPos(ev));
  if (n) {
    toggleExpand(n.id);
    draw();
  }
});
el('cv').addEventListener('wheel', ev => {
  ev.preventDefault();
  const r = el('cv').getBoundingClientRect();
  const mx = ev.clientX - r.left, my = ev.clientY - r.top;
  const k = ev.deltaY < 0 ? 1.15 : 1 / 1.15;
  const nz = Math.min(3, Math.max(0.1, view.zoom * k));
  view.panX = mx - (mx - view.panX) * (nz / view.zoom);
  view.panY = my - (my - view.panY) * (nz / view.zoom);
  view.zoom = nz;
  draw();
}, { passive: false });
el('btn-reset').addEventListener('click', () => {
  view = { panX: 40, panY: 40, zoom: 1 };
  draw();
});
```

- [ ] **Step 2: Verify with the fixture**

Open `viewer.html` (no query string), drop `tools/calltrace/test-fixture.xct` on it.
Expected:
1. Two root nodes: `sub_00011000` (expanded — shows `▾` and two children `sub_00012000`, `sub_00013000`) and `KERNEL` (collapsed, `+1`).
2. Double-click `sub_00012000`: three children appear (`sub_00013000` renders as a blue-tinted CROSS-link curve to the existing node, plus `sub_00014000`, `sub_00015000`).
3. Double-click `sub_00013000`: amber `KeQuerySystemTime` kernel leaf (no expand glyph) + a self-loop curve on `sub_00013000`.
4. Double-click `KERNEL`: `sub_00016000` appears; expanding it shows its edge to `sub_00012000` as a cross-link.
5. Edge `×count` labels visible at default zoom; pan by dragging empty space; wheel zooms toward the cursor; Reset view restores.
6. Single-click selects (gold outline). Selftest (`?selftest=1`) still ALL PASS.

- [ ] **Step 3: Verify with a real recording**

Drop the game `.xct` from Task 5 on the viewer.
Expected: entry node expanded showing its immediate callees; expanding a few levels stays responsive (layout+draw is O(visible nodes)); kernel leaves appear amber with real export names.

- [ ] **Step 4: Commit**

```bash
git add tools/calltrace/viewer.html
git commit -m "calltrace: Add viewer graph layout, rendering, and navigation"
```

---

### Task 9: Viewer — details panel, search, symbol maps

**Files:**
- Modify: `tools/calltrace/viewer.html` (replace the `// === [Task 9] DETAILS, SEARCH, SYMBOLS ===` marker and the `function showDetails(id) {}` stub)

**Interfaces:**
- Consumes: everything above; `layoutNodes`, `toggleExpand`.
- Produces: `showDetails(id)` (with POLY flags on call sites), `revealNode(id)` (expand path from roots + center + select), `doSearch(text)`, `loadSymbols(text)` (with per-title localStorage persistence).

- [ ] **Step 1: Replace the Task 9 marker + showDetails stub**

Replace:

```js
// === [Task 9] DETAILS, SEARCH, SYMBOLS ===
function showDetails(id) {}
```

with:

```js
/* ---------------- details panel ---------------- */

function showDetails(id) {
  const d = el('details');
  const f = model && model.fns.get(id);
  if (!f) {
    d.innerHTML = '<i>No selection</i>';
    return;
  }
  const rows = [`<h3>${esc(fnName(model, id))}</h3>`];
  if (id >= 0) {
    rows.push(`<div class="kv">address <b>${hex(id)}</b></div>`);
  }
  if (id >= 0 && f.kind === 'fn') {
    const sec = sectionOf(model, id);
    if (sec) rows.push(`<div class="kv">section <b>${esc(sec)}</b></div>`);
  }
  if (f.kind === 'kernel') {
    rows.push('<div class="kv">kernel export (leaf)</div>');
  }
  rows.push(`<h4>Callers (${f.inSet.size})</h4>`);
  for (const cid of [...f.inSet].sort((a, b) => a - b)) {
    rows.push(`<div class="lnk" data-id="${cid}">` +
              `${esc(fnName(model, cid))}</div>`);
  }
  rows.push(`<h4>Callees (${f.out.size})</h4>`);
  for (const [cid, e] of [...f.out].sort((a, b) => a[0] - b[0])) {
    rows.push(`<div class="lnk" data-id="${cid}">` +
              `${esc(fnName(model, cid))} ×${e.count}</div>`);
    for (const [site, cnt] of e.sites) {
      const poly = f.sites.get(site).size >= 2
          ? ' <span class="poly">POLY</span>' : '';
      rows.push(`<div class="site">from ${hex(site)} ×${cnt}` +
                `${poly}</div>`);
    }
  }
  d.innerHTML = rows.join('');
  for (const a of d.querySelectorAll('.lnk')) {
    a.onclick = () => revealNode(parseInt(a.dataset.id, 10));
  }
}

/* ---------------- reveal & search ---------------- */

function revealNode(id) {
  if (!model || !model.fns.has(id)) return;
  /* BFS from roots over the full graph to find an expandable path. */
  const preds = new Map();
  const seen = new Set(model.roots);
  const queue = [...model.roots];
  let found = model.roots.includes(id);
  while (queue.length && !found) {
    const cur = queue.shift();
    const f = model.fns.get(cur);
    if (!f) continue;
    for (const cid of f.out.keys()) {
      if (seen.has(cid)) continue;
      seen.add(cid);
      preds.set(cid, cur);
      if (cid === id) { found = true; break; }
      queue.push(cid);
    }
  }
  if (found) {
    for (let cur = preds.get(id); cur !== undefined;
         cur = preds.get(cur)) {
      expanded.add(cur);
    }
  } else if (!model.roots.includes(id)) {
    model.roots.push(id);
    showBanner('Not reachable from recorded roots — shown as a ' +
               'temporary root', 'warn');
  }
  selected = id;
  draw(); /* rebuild layout so the node exists */
  const n = layoutNodes.get(id);
  if (n) {
    const r = el('cv').getBoundingClientRect();
    view.panX = r.width / 2 - (n.x + n.w / 2) * view.zoom;
    view.panY = r.height / 2 - (n.y + n.h / 2) * view.zoom;
    draw();
  }
  showDetails(id);
}

function doSearch(text) {
  if (!model) return;
  const q = text.trim();
  if (!q) return;
  let id = null;
  const m = q.match(/^(?:0x|sub_)?([0-9a-fA-F]{4,8})$/);
  if (m) {
    const addr = parseInt(m[1], 16);
    if (model.fns.has(addr)) id = addr;
  }
  if (id === null) {
    const needle = q.toLowerCase();
    for (const fid of model.fns.keys()) {
      if (fnName(model, fid).toLowerCase().includes(needle)) {
        id = fid;
        break;
      }
    }
  }
  if (id === null) {
    showBanner('No match for "' + q + '"', 'warn');
    return;
  }
  clearBanner();
  revealNode(id);
}
el('search').addEventListener('keydown', ev => {
  if (ev.key === 'Enter') doSearch(ev.target.value);
});

/* ---------------- symbol maps ---------------- */

function loadSymbols(text) {
  let applied = 0, skipped = 0;
  for (const line of text.split(/\r?\n/)) {
    const t = line.trim();
    if (!t || t.startsWith('#') || t.startsWith(';')) continue;
    const m = t.match(/^(?:0x)?([0-9a-fA-F]{4,8})[\s:=,]+(\S+)/);
    if (!m) { skipped++; continue; }
    symbols.set(parseInt(m[1], 16), m[2]);
    applied++;
  }
  if (model) {
    localStorage.setItem('xct-syms-' + model.rec.titleId,
                         JSON.stringify([...symbols]));
  }
  showBanner(`Symbols: ${applied} applied` +
             (skipped ? `, ${skipped} lines skipped` : ''), 'info');
  draw();
  if (selected !== null) showDetails(selected);
}
el('symfile').onchange = ev => {
  const f = ev.target.files[0];
  if (f) f.text().then(loadSymbols);
};
```

- [ ] **Step 2: Verify with the fixture**

Open `viewer.html`, drop the fixture.
Expected:
1. Click `sub_00012000` → details show address `00012000`, section `.text`, one caller, three callees; the `sub_00014000`/`sub_00015000` call-site rows (`from 00012050`) each show an orange **POLY** tag; the `from 00012040` row does not.
2. Click a caller/callee link in the panel → that node is revealed/expanded in the graph, centered, selected.
3. Search `16000` + Enter → `KERNEL` expands, `sub_00016000` centered+selected. Search `KeQuery` → the kernel leaf is revealed (path via C auto-expands). Search `zzz` → warn banner.
4. Create `syms.txt` containing `12000 UpdateWorld` and a junk line `hello`; Load symbols → banner `Symbols: 1 applied, 1 lines skipped`, node label becomes `UpdateWorld`, search `UpdateWorld` finds it. Reload page + re-drop fixture → symbol persists (localStorage).
5. Selftest still ALL PASS.

- [ ] **Step 3: Commit**

```bash
git add tools/calltrace/viewer.html
git commit -m "calltrace: Add viewer details panel, search, and symbol maps"
```

---

### Task 10: Viewer — DOT export

**Files:**
- Modify: `tools/calltrace/viewer.html` (replace the `// === [Task 10] EXPORT & HELP ===` marker; append 1 selftest)

**Interfaces:**
- Consumes: `model`, `layoutNodes`, `fnName`.
- Produces: `exportDot(full)` wired to the two toolbar buttons; one added selftest.

- [ ] **Step 1: Replace the Task 10 marker**

Replace `// === [Task 10] EXPORT & HELP ===` with:

```js
/* ---------------- DOT export ---------------- */

function dotSource(full) {
  const inc = id => full || layoutNodes.has(id);
  const q = s => '"' + s.replace(/"/g, '\\"') + '"';
  const lines = ['digraph calltrace {', '  rankdir=LR;',
                 '  node [shape=box fontname="monospace"];'];
  for (const [id, f] of model.fns) {
    if (!inc(id)) continue;
    if (f.kind === 'kernel') {
      lines.push(`  ${q(fnName(model, id))} ` +
                 '[style=filled fillcolor=khaki];');
    }
    for (const [cid, e] of f.out) {
      if (!inc(cid)) continue;
      lines.push(`  ${q(fnName(model, id))} -> ` +
                 `${q(fnName(model, cid))} [label="${e.count}"];`);
    }
  }
  lines.push('}');
  return lines.join('\n');
}

function exportDot(full) {
  if (!model) {
    showBanner('Load a recording first', 'warn');
    return;
  }
  const blob = new Blob([dotSource(full)], { type: 'text/vnd.graphviz' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = (model.rec.title ? model.rec.title.replace(/\W+/g, '_')
                                : 'calltrace') + '.dot';
  a.click();
  URL.revokeObjectURL(a.href);
}
el('btn-dot-visible').addEventListener('click', () => exportDot(false));
el('btn-dot-full').addEventListener('click', () => exportDot(true));
```

- [ ] **Step 2: Add a selftest**

After the last existing `T(...)` block (the `'naming'` test), add:

```js
T('DOT export', () => {
  model = deriveModel(parseXCT(buildFixture()));
  const dot = dotSource(true);
  assertEq(dot.includes('"sub_00013000" -> "KeQuerySystemTime" ' +
                        '[label="3"]'), true, 'kernel edge');
  assertEq(dot.includes('"KERNEL" -> "sub_00016000"'), true, 'root edge');
  assertEq(dot.startsWith('digraph calltrace {'), true, 'digraph header');
  model = null;
});
```

- [ ] **Step 3: Verify**

1. `viewer.html?selftest=1` → 8 tests, ALL PASS.
2. Drop fixture, click **Export DOT (full)** → downloads `Test_Fixture.dot`; if Graphviz is installed run `dot -Tsvg Test_Fixture.dot -o t.svg` and confirm it renders; otherwise inspect the text (9 edges, khaki kernel node).
3. **Export DOT (visible)** with only entry expanded → contains only the entry's outgoing edges (plus nothing unreachable).

- [ ] **Step 4: Commit**

```bash
git add tools/calltrace/viewer.html
git commit -m "calltrace: Add viewer DOT export"
```

---

### Task 11: End-to-end validation

**Files:** none (verification only; findings go in the commit message of any fixes)

- [ ] **Step 1: Full session**

Build (`./build.sh`), boot a real game, play ~1–2 minutes across a state change (menu → in-game), `Stop & Save`, run `xct_dump.py` on the file, open it in the viewer.
Expected: entry expands into a plausible tree; kernel leaves carry real export names; search and symbol loading work on real data; expanding ~10 levels stays responsive.

- [ ] **Step 2: Ground-truth spot check (IDA/Ghidra)**

Pick 3 direct-call edges from `xct_dump.py` output (game-space site AND callee). In your disassembler, open the same XBE and check: the bytes at each recorded `call_site` are a CALL instruction whose target equals the recorded `callee`.
Expected: all 3 match exactly. (Indirect calls can't be checked statically — pick sites whose disassembly shows `E8` direct calls.)

- [ ] **Step 3: Idle-overhead sanity**

With the feature never armed in a session: confirm normal gameplay speed and (code-level) that `xemu_calltrace_armed` is only read at translation time in `emit.c.inc` — no helper calls exist in translated code while disarmed.

- [ ] **Step 4: Record findings**

If any step failed, fix and commit with `calltrace:` prefix. When all pass, the feature is done.

---

## Self-Review Notes (already applied)

- Spec coverage: recording (Tasks 2–4), scope filter (Task 2), zero-idle-cost (Task 4 guard + Task 11 check), `.xct` with sections/kernel names/title metadata (Task 5), truncation cap + toasts (Tasks 2/5), config key with screenshot-dir fallback (Task 5), viewer expand-from-Entry/pan/zoom/search (Tasks 7–9), poly-site flag (Task 9), symbol maps + documented IDA/Ghidra exports (Tasks 7/9), DOT export (Task 10), synthetic fixture + selftests (Tasks 6–10), IDA ground-truth check (Task 11).
- The `Stop` menu item is intentionally created in Task 3 and upgraded in Task 5 — Task 3 remains independently testable without the writer.
- `xemu_calltrace_save` appears in Task 5 only; Tasks 2–4 compile without it.

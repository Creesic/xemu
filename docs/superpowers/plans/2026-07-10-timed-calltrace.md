# Timed Call-Trace + Timeline Viewer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a "Timed" recording mode to the xemu call-trace engine that captures an ordered, deflate-compressed per-call event log (with adaptive spam throttling and a callee ignore-list), extend the `.xct` format to v2, and give the HTML viewer a timeline scrubber that replays the graph splintering out from Entry with synapse-style pulse animation, plus a Noisiest-Functions panel that exports an ignore-list.

**Architecture:** The edge map gains a stable per-edge index; in Timed mode each recorded call appends that index to a chunked event buffer (throttled per edge). The writer appends a zlib-deflated event block after the existing edges (format v2). The viewer parses v2 (inflating events with the browser's `DecompressionStream`), computes each edge's first-fire event index, and drives an auto-reveal timeline: the visible graph at playhead P is the set of edges first fired by P, so scrubbing is O(edges) and playing forward grows the graph while pulses ride each firing edge.

**Tech Stack:** C (QEMU/xemu, zlib, GLib), Dear ImGui (menu), vanilla JS + Canvas 2D + `DecompressionStream`/`CompressionStream` (viewer), Python 3 (fixture + dump).

**Spec:** `docs/superpowers/specs/2026-07-10-timed-calltrace-design.md`

## Global Constraints

- Builds on the existing calltrace feature (branch `calltrace-visualizer`): `xemu-calltrace-map.h`, `xemu-calltrace.c/.h`, `.xct` v1 writer, `tools/calltrace/{viewer.html,make_test_xct.py,xct_dump.py}`.
- Constants (exact): `CT_THROTTLE_FULL = 256`, `CT_THROTTLE_EVERY = 64`, `CT_EVENT_CAP = 20000000`, `CT_EVENT_CHUNK = (1u << 20)`. Viewer: `MAX_PULSES = 200`, `PULSE_LIFE_MS = 400`, speed presets `[1, 10, 100, 1000, 10000]`.
- `.xct` v2: `version = 2` when a Timed recording is saved; Edges mode still writes `version = 1`, byte-compatible with today. Kernel boundary `0x80000000` unchanged.
- Event = one `u32` edge index. `events[k]` indexes `edges[k']` where edges are written in **index order** in v2. Event stream is zlib (`compress`/`deflate`) format, inflated in-browser with `DecompressionStream('deflate')`.
- Recording runs on the single vCPU thread; hot path stays lock-free. No new locks.
- Build: `./build.sh` (first time) or incremental `ninja qemu-system-i386w.exe` in `build/`, via the project's MSYS2 UCRT64 shell. Standalone C unit tests compile with plain `gcc` (use the MSYS2 `ucrt64` gcc, not a stray PATH gcc). Viewer selftests run at `viewer.html?selftest=1`; serve the folder over `python3 -m http.server` (browsers block `file://` for `fetch`).
- C style: QEMU (4-space indent, GPL-2.0-or-later header matching siblings). Commit each task with a `calltrace:` prefix.
- Do NOT re-introduce the standalone/embedding packer removed earlier; the viewer stays a general file-loading tool.

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `xemu-calltrace-map.h` | modify (T1) | add `index` to `CTEdge`; add `ct_map_add_indexed` |
| `tests/calltrace/test-calltrace-map.c` | modify (T1) | cover indexing |
| `xemu-calltrace-events.h` | create (T2) | header-only chunked `u32` event buffer |
| `tests/calltrace/test-calltrace-events.c` | create (T2) | unit test the buffer |
| `xemu-calltrace.h` | modify (T3,T4) | mode enum, timed API, ignore-list API |
| `xemu-calltrace.c` | modify (T3,T5) | modes, throttle, ignore-set, record path; v2 writer |
| `ui/xui/menubar.cc` | modify (T4) | depth items + Load/Clear ignore list |
| `tools/calltrace/make_test_xct.py` | modify (T6) | emit v2 fixtures with events |
| `tools/calltrace/xct_dump.py` | modify (T6) | dump v2 event block |
| `tools/calltrace/viewer.html` | modify (T7,T8,T9,T10) | v2 parse+inflate, timeline model, playback, noise panel |

---

### Task 1: Edge index in the map

**Files:**
- Modify: `xemu-calltrace-map.h` (`CTEdge` at lines 34-37; add function after `ct_map_add`)
- Test: `tests/calltrace/test-calltrace-map.c`

**Interfaces:**
- Consumes: existing `CTMap`, `ct_map_key`, `ct_map_hash`, `CT_MAP_CAPACITY`, `CT_MAP_MAX_ENTRIES`.
- Produces: `CTEdge.index` (`uint32_t`, stable first-seen ordinal 0..num_entries-1); `CTEdge *ct_map_add_indexed(CTMap*, uint32_t call_site, uint32_t callee)` returning the edge (count already incremented, `index` set), or `NULL` when the key is the 0 sentinel or the map is full and the edge is new.

- [ ] **Step 1: Add the failing test**

Append to `tests/calltrace/test-calltrace-map.c` just before `printf("PASS\n");`:

```c
    /* ct_map_add_indexed assigns stable, dense indices */
    CTMap m2;
    assert(ct_map_init(&m2));
    CTEdge *e0 = ct_map_add_indexed(&m2, 0xAAAA, 0xBBBB);
    CTEdge *e1 = ct_map_add_indexed(&m2, 0xCCCC, 0xDDDD);
    assert(e0 && e0->index == 0 && e0->count == 1);
    assert(e1 && e1->index == 1 && e1->count == 1);
    CTEdge *e0b = ct_map_add_indexed(&m2, 0xAAAA, 0xBBBB);
    assert(e0b == e0 && e0b->index == 0 && e0b->count == 2);
    assert(ct_map_add_indexed(&m2, 0, 0) == NULL); /* sentinel */
    assert(m2.num_entries == 2);
    ct_map_free(&m2);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -o tests/calltrace/test-calltrace-map.exe tests/calltrace/test-calltrace-map.c && ./tests/calltrace/test-calltrace-map.exe`
Expected: FAIL to compile — `ct_map_add_indexed` undeclared, `index` not a member.

- [ ] **Step 3: Add the field and function**

In `xemu-calltrace-map.h`, change `CTEdge` to:

```c
typedef struct CTEdge {
    uint64_t key; /* (call_site << 32) | callee; 0 = empty slot */
    uint64_t count;
    uint32_t index; /* stable first-seen ordinal; used by the event log */
} CTEdge;
```

After `ct_map_add` (after line 97), add:

```c
/*
 * Like ct_map_add, but returns the edge so callers can read its stable
 * index and current count (for the timed event log). Returns NULL for the
 * 0-key sentinel or when the map is full and the edge is new.
 */
static inline CTEdge *ct_map_add_indexed(CTMap *m, uint32_t call_site,
                                         uint32_t callee)
{
    uint64_t key = ct_map_key(call_site, callee);
    if (key == 0) {
        return NULL;
    }
    uint32_t idx = ct_map_hash(key) & (CT_MAP_CAPACITY - 1);
    for (;;) {
        CTEdge *e = &m->slots[idx];
        if (e->key == key) {
            e->count++;
            return e;
        }
        if (e->key == 0) {
            if (m->num_entries >= CT_MAP_MAX_ENTRIES) {
                return NULL;
            }
            e->key = key;
            e->count = 1;
            e->index = m->num_entries;
            m->num_entries++;
            return e;
        }
        idx = (idx + 1) & (CT_MAP_CAPACITY - 1);
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -o tests/calltrace/test-calltrace-map.exe tests/calltrace/test-calltrace-map.c && ./tests/calltrace/test-calltrace-map.exe`
Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add xemu-calltrace-map.h tests/calltrace/test-calltrace-map.c
git commit -m "calltrace: Add stable edge index to the map"
```

---

### Task 2: Chunked event buffer (standalone, TDD)

**Files:**
- Create: `xemu-calltrace-events.h`
- Test: `tests/calltrace/test-calltrace-events.c`

**Interfaces:**
- Produces: `CTEvents`; `void ct_events_init(CTEvents*)`; `void ct_events_free(CTEvents*)`; `bool ct_events_append(CTEvents*, uint32_t v)` (returns false and drops the value once `count >= CT_EVENT_CAP`); `uint64_t` field `CTEvents.count`; iteration via `CTEventChunk` linked list (`head`, per-chunk `fill`, `next`) so the writer can stream chunks. `CT_EVENT_CHUNK`, `CT_EVENT_CAP` constants. For the test, use a small cap override so the test is fast.

- [ ] **Step 1: Write the failing test**

Create `tests/calltrace/test-calltrace-events.c`:

```c
#include <assert.h>
#include <stdio.h>
#define CT_EVENT_CAP 2500u        /* small cap for the test */
#define CT_EVENT_CHUNK 1024u      /* small chunk for the test */
#include "../../xemu-calltrace-events.h"

int main(void)
{
    CTEvents ev;
    ct_events_init(&ev);
    assert(ev.count == 0);

    for (uint32_t i = 0; i < 3000; i++) {
        bool ok = ct_events_append(&ev, i);
        assert(ok == (i < CT_EVENT_CAP));
    }
    assert(ev.count == CT_EVENT_CAP);

    /* walk the chunks back into a flat check */
    uint64_t seen = 0;
    for (CTEventChunk *c = ev.head; c; c = c->next) {
        for (uint32_t j = 0; j < c->fill; j++) {
            assert(c->data[j] == (uint32_t)seen);
            seen++;
        }
    }
    assert(seen == CT_EVENT_CAP);

    ct_events_free(&ev);
    assert(ev.head == NULL && ev.count == 0);
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -o tests/calltrace/test-calltrace-events.exe tests/calltrace/test-calltrace-events.c && ./tests/calltrace/test-calltrace-events.exe`
Expected: FAIL — `xemu-calltrace-events.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `xemu-calltrace-events.h`:

```c
/*
 * xemu call-trace event log
 *
 * Append-only, chunked buffer of u32 edge indices (one per recorded call,
 * in execution order). Header-only and QEMU-independent for standalone
 * unit testing (tests/calltrace/test-calltrace-events.c).
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

#ifndef XEMU_CALLTRACE_EVENTS_H
#define XEMU_CALLTRACE_EVENTS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef CT_EVENT_CHUNK
#define CT_EVENT_CHUNK (1u << 20) /* u32 events per chunk (4 MiB) */
#endif
#ifndef CT_EVENT_CAP
#define CT_EVENT_CAP 20000000u    /* max events (~80 MB) before truncation */
#endif

typedef struct CTEventChunk {
    struct CTEventChunk *next;
    uint32_t fill;                 /* used entries in data */
    uint32_t data[CT_EVENT_CHUNK];
} CTEventChunk;

typedef struct CTEvents {
    CTEventChunk *head;
    CTEventChunk *tail;
    uint64_t count;                /* total appended events */
} CTEvents;

static inline void ct_events_init(CTEvents *e)
{
    e->head = e->tail = NULL;
    e->count = 0;
}

static inline void ct_events_free(CTEvents *e)
{
    CTEventChunk *c = e->head;
    while (c) {
        CTEventChunk *n = c->next;
        free(c);
        c = n;
    }
    e->head = e->tail = NULL;
    e->count = 0;
}

/* Append one event. Returns false (and drops it) once the cap is reached. */
static inline bool ct_events_append(CTEvents *e, uint32_t v)
{
    if (e->count >= CT_EVENT_CAP) {
        return false;
    }
    if (!e->tail || e->tail->fill == CT_EVENT_CHUNK) {
        CTEventChunk *c = (CTEventChunk *)malloc(sizeof(CTEventChunk));
        if (!c) {
            return false;
        }
        c->next = NULL;
        c->fill = 0;
        if (e->tail) {
            e->tail->next = c;
        } else {
            e->head = c;
        }
        e->tail = c;
    }
    e->tail->data[e->tail->fill++] = v;
    e->count++;
    return true;
}

#endif
```

- [ ] **Step 4: Run test to verify it passes**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -o tests/calltrace/test-calltrace-events.exe tests/calltrace/test-calltrace-events.c && ./tests/calltrace/test-calltrace-events.exe`
Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add xemu-calltrace-events.h tests/calltrace/test-calltrace-events.c
git commit -m "calltrace: Add chunked event buffer with unit test"
```

---

### Task 3: Engine — modes, throttle, ignore-list, record path

**Files:**
- Modify: `xemu-calltrace.h`, `xemu-calltrace.c`

**Interfaces:**
- Consumes: `ct_map_add_indexed` (T1), `CTEvents`/`ct_events_*` (T2), existing `xemu_calltrace_armed`, `ct_map`, `ct_truncated`, `queue_tb_flush`, `qemu_get_cpu`.
- Produces (in `xemu-calltrace.h`):
  - `typedef enum { CT_OFF, CT_EDGES, CT_TIMED } CalltraceMode;`
  - `void xemu_calltrace_start_mode(CalltraceMode mode);` (replaces the internals of `xemu_calltrace_start`, which stays as `start_mode(CT_EDGES)`)
  - `CalltraceMode xemu_calltrace_mode(void);`
  - `uint64_t xemu_calltrace_event_count(void);`
  - `bool xemu_calltrace_events_truncated(void);`
  - `void xemu_calltrace_load_ignore(const char *path, int *added, int *skipped);`
  - `void xemu_calltrace_clear_ignore(void);`
  - `uint32_t xemu_calltrace_ignore_count(void);`
- Internal to `.c`: `static CalltraceMode ct_mode;`, `static CTEvents ct_events;`, `static bool ct_events_trunc;`, a small open-addressing `static uint32_t *ct_ignore; static uint32_t ct_ignore_cap, ct_ignore_n;` callee-address set with `ct_ignore_has`.

- [ ] **Step 1: Update the header**

In `xemu-calltrace.h`, replace the block from `extern bool xemu_calltrace_armed;` through `bool xemu_calltrace_truncated(void);` with:

```c
extern bool xemu_calltrace_armed;

typedef enum { CT_OFF, CT_EDGES, CT_TIMED } CalltraceMode;

void xemu_calltrace_start_mode(CalltraceMode mode);
void xemu_calltrace_start(void); /* legacy: start in CT_EDGES */
void xemu_calltrace_stop(void);
CalltraceMode xemu_calltrace_mode(void);
uint64_t xemu_calltrace_edge_count(void);
bool xemu_calltrace_truncated(void);
uint64_t xemu_calltrace_event_count(void);
bool xemu_calltrace_events_truncated(void);

/* Ignore-list: callee addresses to drop entirely from recording. */
void xemu_calltrace_load_ignore(const char *path, int *added, int *skipped);
void xemu_calltrace_clear_ignore(void);
uint32_t xemu_calltrace_ignore_count(void);
```

- [ ] **Step 2: Add state, throttle constants, and the ignore set**

In `xemu-calltrace.c`, after `#include "xemu-calltrace-map.h"` add:

```c
#include "xemu-calltrace-events.h"
```

and after the existing `static bool ct_truncated;` add:

```c
#define CT_THROTTLE_FULL 256   /* log every call up to this many per edge */
#define CT_THROTTLE_EVERY 64   /* then 1-in-this-many afterwards          */

static CalltraceMode ct_mode;
static CTEvents ct_events;
static bool ct_events_trunc;

/* Open-addressing set of callee addresses to ignore (0 = empty slot). */
static uint32_t *ct_ignore;
static uint32_t ct_ignore_cap;
static uint32_t ct_ignore_n;

static bool ct_ignore_has(uint32_t addr)
{
    if (!ct_ignore_cap || addr == 0) {
        return false;
    }
    uint32_t i = (addr * 2654435761u) & (ct_ignore_cap - 1);
    while (ct_ignore[i]) {
        if (ct_ignore[i] == addr) {
            return true;
        }
        i = (i + 1) & (ct_ignore_cap - 1);
    }
    return false;
}

static void ct_ignore_put(uint32_t addr)
{
    if (addr == 0) {
        return;
    }
    /* grow/rehash at 50% load */
    if ((ct_ignore_n + 1) * 2 > ct_ignore_cap) {
        uint32_t newcap = ct_ignore_cap ? ct_ignore_cap * 2 : 1024;
        uint32_t *old = ct_ignore;
        uint32_t oldcap = ct_ignore_cap;
        ct_ignore = g_malloc0(newcap * sizeof(uint32_t));
        ct_ignore_cap = newcap;
        ct_ignore_n = 0;
        for (uint32_t k = 0; k < oldcap; k++) {
            if (old && old[k]) {
                ct_ignore_put(old[k]);
            }
        }
        g_free(old);
    }
    uint32_t i = (addr * 2654435761u) & (ct_ignore_cap - 1);
    while (ct_ignore[i]) {
        if (ct_ignore[i] == addr) {
            return;
        }
        i = (i + 1) & (ct_ignore_cap - 1);
    }
    ct_ignore[i] = addr;
    ct_ignore_n++;
}
```

- [ ] **Step 3: Rewrite start/stop/record and add the accessors**

Replace `xemu_calltrace_start`, `xemu_calltrace_stop`, `xemu_calltrace_edge_count`, `xemu_calltrace_truncated`, and `xemu_calltrace_record` with:

```c
void xemu_calltrace_start_mode(CalltraceMode mode)
{
    if (xemu_calltrace_armed || mode == CT_OFF) {
        return;
    }
    if (ct_map.slots) {
        ct_map_free(&ct_map);
    }
    if (!ct_map_init(&ct_map)) {
        return;
    }
    ct_events_free(&ct_events);
    ct_events_init(&ct_events);
    ct_truncated = false;
    ct_events_trunc = false;
    ct_mode = mode;
    xemu_calltrace_armed = true;
    queue_tb_flush(qemu_get_cpu(0));
}

void xemu_calltrace_start(void)
{
    xemu_calltrace_start_mode(CT_EDGES);
}

void xemu_calltrace_stop(void)
{
    if (!xemu_calltrace_armed) {
        return;
    }
    xemu_calltrace_armed = false;
    queue_tb_flush(qemu_get_cpu(0));
}

CalltraceMode xemu_calltrace_mode(void)
{
    return ct_mode;
}

uint64_t xemu_calltrace_edge_count(void)
{
    return ct_map.slots ? ct_map.num_entries : 0;
}

bool xemu_calltrace_truncated(void)
{
    return ct_truncated;
}

uint64_t xemu_calltrace_event_count(void)
{
    return ct_events.count;
}

bool xemu_calltrace_events_truncated(void)
{
    return ct_events_trunc;
}

uint32_t xemu_calltrace_ignore_count(void)
{
    return ct_ignore_n;
}

void xemu_calltrace_clear_ignore(void)
{
    g_free(ct_ignore);
    ct_ignore = NULL;
    ct_ignore_cap = 0;
    ct_ignore_n = 0;
}

void xemu_calltrace_load_ignore(const char *path, int *added, int *skipped)
{
    int a = 0, s = 0;
    FILE *f = qemu_fopen(path, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' ||
                *p == '\0') {
                continue;
            }
            uint32_t addr = (uint32_t)strtoul(p, NULL, 16);
            if (addr) {
                ct_ignore_put(addr);
                a++;
            } else {
                s++;
            }
        }
        fclose(f);
    }
    if (added) {
        *added = a;
    }
    if (skipped) {
        *skipped = s;
    }
}

void xemu_calltrace_record(uint32_t call_site, uint32_t callee)
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
    if (ct_mode == CT_TIMED) {
        if (e->count <= CT_THROTTLE_FULL ||
            (e->count % CT_THROTTLE_EVERY) == 0) {
            if (!ct_events_append(&ct_events, e->index)) {
                ct_events_trunc = true;
            }
        }
    }
}
```

(`CT_KERNEL_SPACE` is already defined at the top of the file. `g_malloc0`/`g_free`/`qemu_fopen`/`fgets`/`strtoul` come via `qemu/osdep.h`.)

- [ ] **Step 4: Build**

Run: `MSYSTEM=UCRT64 /c/msys64/usr/bin/bash.exe -lc 'cd /c/Users/Tera/Documents/GitHub/xemu/build && ninja qemu-system-i386w.exe' 2>&1 | tail -5`
Expected: `Linking target qemu-system-i386w.exe`. (The engine compiles; nothing calls the new mode yet — T4 wires the menu.)

- [ ] **Step 5: Commit**

```bash
git add xemu-calltrace.h xemu-calltrace.c
git commit -m "calltrace: Add timed mode, adaptive throttle, and ignore-list"
```

---

### Task 4: Menu — depth items + ignore-list actions

**Files:**
- Modify: `ui/xui/menubar.cc` (the `Call Trace` submenu added earlier)

**Interfaces:**
- Consumes: `xemu_calltrace_start_mode`, `xemu_calltrace_mode`, `xemu_calltrace_event_count`, `xemu_calltrace_events_truncated`, `xemu_calltrace_load_ignore`, `xemu_calltrace_clear_ignore`, `xemu_calltrace_ignore_count` (T3); existing `xemu_calltrace_edge_count`, `xemu_calltrace_stop`, `xemu_calltrace_save`, `xemu_get_xbe_info`; ImGui; `xemu_queue_notification`/`_error_message`.
- Produces: depth-aware Call Trace menu. A file dialog for the ignore list uses the existing xemu file-picker helper if present; otherwise read a fixed path (below).

- [ ] **Step 1: Replace the Call Trace submenu body**

In `ui/xui/menubar.cc`, replace the whole `if (ImGui::BeginMenu("Call Trace")) { ... }` block with:

```cpp
            if (ImGui::BeginMenu("Call Trace")) {
                if (!xemu_calltrace_armed) {
                    bool have_xbe = xemu_get_xbe_info() != NULL;
                    if (ImGui::MenuItem("Start - Edges", NULL, false,
                                        have_xbe)) {
                        xemu_calltrace_start_mode(CT_EDGES);
                    }
                    if (ImGui::MenuItem("Start - Timed (call timeline)", NULL,
                                        false, have_xbe)) {
                        xemu_calltrace_start_mode(CT_TIMED);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Load ignore list...")) {
                        const char *dir = g_config.general.calltrace_dir;
                        if (!strlen(dir)) {
                            dir = g_config.general.screenshot_dir;
                        }
                        if (!strlen(dir)) {
                            dir = ".";
                        }
                        char *path = g_strdup_printf("%s/ignore.txt", dir);
                        int added = 0, skipped = 0;
                        xemu_calltrace_load_ignore(path, &added, &skipped);
                        char *msg = g_strdup_printf(
                            "Ignore list: %d loaded, %d skipped (%s)",
                            added, skipped, path);
                        xemu_queue_notification(msg);
                        g_free(msg);
                        g_free(path);
                    }
                    if (xemu_calltrace_ignore_count() > 0) {
                        ImGui::Text("Ignoring %u addresses",
                                    xemu_calltrace_ignore_count());
                        if (ImGui::MenuItem("Clear ignore list")) {
                            xemu_calltrace_clear_ignore();
                        }
                    }
                } else {
                    bool timed = xemu_calltrace_mode() == CT_TIMED;
                    ImGui::Text("Recording (%s): %llu edges",
                                timed ? "Timed" : "Edges",
                                (unsigned long long)
                                    xemu_calltrace_edge_count());
                    if (timed) {
                        ImGui::Text("Events: %llu",
                                    (unsigned long long)
                                        xemu_calltrace_event_count());
                        if (xemu_calltrace_events_truncated()) {
                            ImGui::TextColored(
                                ImVec4(1.0f, 0.6f, 0.1f, 1.0f),
                                "Event cap reached; timeline is partial");
                        }
                    }
                    if (xemu_calltrace_truncated()) {
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.6f, 0.1f, 1.0f),
                            "Edge limit reached; new edges dropped");
                    }
                    ImGui::Separator();
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
                }
                ImGui::EndMenu();
            }
```

(This keeps the ignore file at `<calltrace_dir or screenshot_dir>/ignore.txt` — the same folder the viewer's Export writes to. No new file-dialog dependency.)

- [ ] **Step 2: Build and manually verify menu states**

Run: `MSYSTEM=UCRT64 /c/msys64/usr/bin/bash.exe -lc 'cd /c/Users/Tera/Documents/GitHub/xemu/build && ninja qemu-system-i386w.exe' 2>&1 | tail -3`, then `cp build/qemu-system-i386w.exe dist/xemu.exe` and launch, boot a game.
Expected: `Debug → Call Trace` shows **Start - Edges** and **Start - Timed**; starting Timed shows `Recording (Timed): N edges` and `Events: M` climbing; **Load ignore list…** reports a count (0 if no `ignore.txt`); Stop & Save writes a file.

- [ ] **Step 3: Commit**

```bash
git add ui/xui/menubar.cc
git commit -m "calltrace: Add depth selection and ignore-list menu actions"
```

---

### Task 5: v2 writer — index-ordered edges + compressed event block

**Files:**
- Modify: `xemu-calltrace.c` (the `xemu_calltrace_save` writer)

**Interfaces:**
- Consumes: `ct_events` (T2/T3), `CTEdge.index` (T1), zlib (`<zlib.h>`), `GByteArray`.
- Produces: `.xct` v2 layout per the spec — `version = 2` when `ct_mode == CT_TIMED`; edges written in index order; a trailing block `{ u64 event_count, u32 event_flags, u32 throttle_full, u32 throttle_every, u64 raw_bytes, u64 comp_bytes, u8 blob[comp_bytes] }` where blob is zlib-deflated `u32` events.

- [ ] **Step 1: Include zlib and add a streaming deflate helper**

At the top of `xemu-calltrace.c` add `#include <zlib.h>` after the other includes. Above `xemu_calltrace_save`, add:

```c
/* Deflate the event log (u32 indices, in order) into a GByteArray. */
static GByteArray *ct_deflate_events(void)
{
    GByteArray *out = g_byte_array_new();
    z_stream zs = { 0 };
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        return out;
    }
    guint8 obuf[65536];
    for (CTEventChunk *c = ct_events.head; c; c = c->next) {
        zs.next_in = (Bytef *)c->data;
        zs.avail_in = c->fill * sizeof(uint32_t);
        while (zs.avail_in) {
            zs.next_out = obuf;
            zs.avail_out = sizeof(obuf);
            deflate(&zs, Z_NO_FLUSH);
            g_byte_array_append(out, obuf, sizeof(obuf) - zs.avail_out);
        }
    }
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
```

- [ ] **Step 2: Write edges in index order and append the events block**

In `xemu_calltrace_save`, find the header array and the edge-writing loop. Change the version field and the edge loop, then append the block. Replace:

```c
        uint32_t hdr[10] = { 0x52544358u,
                             1,
```

with:

```c
        bool timed = ct_mode == CT_TIMED;
        uint32_t hdr[10] = { 0x52544358u,
                             timed ? 2u : 1u,
```

Replace the edge-writing loop:

```c
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
```

with an index-ordered write (build a pointer array, sort by index):

```c
        /* Emit edges in index order so events[k] indexes edges[k]. */
        CTEdge **ordered = g_new(CTEdge *, ct_map.num_entries);
        for (uint32_t i = 0; i < CT_MAP_CAPACITY; i++) {
            if (ct_map.slots[i].key) {
                ordered[ct_map.slots[i].index] = &ct_map.slots[i];
            }
        }
        for (uint32_t i = 0; ok && i < ct_map.num_entries; i++) {
            CTEdge *e = ordered[i];
            uint32_t addrs[2] = { (uint32_t)(e->key >> 32),
                                  (uint32_t)e->key };
            ok = fwrite(addrs, 8, 1, f) == 1 &&
                 fwrite(&e->count, 8, 1, f) == 1;
        }
        g_free(ordered);

        /* v2: trailing compressed event block. */
        if (ok && timed) {
            GByteArray *blob = ct_deflate_events();
            uint64_t event_count = ct_events.count;
            uint32_t event_flags = ct_events_trunc ? 1u : 0u;
            uint32_t tfull = CT_THROTTLE_FULL, tevery = CT_THROTTLE_EVERY;
            uint64_t raw_bytes = event_count * sizeof(uint32_t);
            uint64_t comp_bytes = blob->len;
            ok = fwrite(&event_count, 8, 1, f) == 1 &&
                 fwrite(&event_flags, 4, 1, f) == 1 &&
                 fwrite(&tfull, 4, 1, f) == 1 &&
                 fwrite(&tevery, 4, 1, f) == 1 &&
                 fwrite(&raw_bytes, 8, 1, f) == 1 &&
                 fwrite(&comp_bytes, 8, 1, f) == 1 &&
                 (comp_bytes == 0 ||
                  fwrite(blob->data, blob->len, 1, f) == 1);
            g_byte_array_free(blob, TRUE);
        }
```

- [ ] **Step 3: Build**

Run: `MSYSTEM=UCRT64 /c/msys64/usr/bin/bash.exe -lc 'cd /c/Users/Tera/Documents/GitHub/xemu/build && ninja qemu-system-i386w.exe' 2>&1 | tail -3`
Expected: links clean. (`-lz` is already in QEMU's link line.)

- [ ] **Step 4: Manual end-to-end capture**

`cp build/qemu-system-i386w.exe dist/xemu.exe`, launch, boot a game, `Start - Timed`, play ~15 s, `Stop & Save`.
Expected: a `.xct` written; its size is far smaller than `events × 4` bytes (compression working). Full validation happens in T6 once the dump tool understands v2.

- [ ] **Step 5: Commit**

```bash
git add xemu-calltrace.c
git commit -m "calltrace: Write v2 recordings with compressed event log"
```

---

### Task 6: v2 fixtures + dump tool

**Files:**
- Modify: `tools/calltrace/make_test_xct.py`, `tools/calltrace/xct_dump.py`

**Interfaces:**
- Consumes: the `.xct` v1 layout + v2 event block (T5).
- Produces: `make_test_xct.py --timed` emits a v2 file whose events reference the fixture's edges by index, with a KNOWN event stream (memorize for viewer selftests): the fixture edges in index order are `[A→B, A→C, B→C, B→D1, B→D2, C→kernel, C→C, KERNEL→T, T→B]` (indices 0..8); the event stream is `[0,1,2,3,3,3,4,4,6,7,8,0,2]` (13 events; edge 3 fired 3×, edge 4 twice) so first-fire indices are edge0=0, edge1=1, edge2=2, edge3=3, edge4=6, edge6=8, edge7=9(→KERNEL→T? see below), etc. `xct_dump.py` prints the event block for v1 (absent) and v2.

Note: keep the existing v1 fixture (`build()`); add a `build_timed()` that reuses the same edges (so both fixtures share addresses) and appends the event block.

- [ ] **Step 1: Extend the dump tool to read v2**

In `tools/calltrace/xct_dump.py`, after the final `print(f'total calls recorded: {total}')` line and before `print('OK')`, add (reads `ecount` u64, then `eflags,tfull,tevery` 3×u32, then `raw_bytes,comp_bytes` 2×u64, then inflates and validates):

```python
    if ver >= 2:
        import zlib
        ecount, = struct.unpack_from('<Q', data, off); off += 8
        eflags, tfull, tevery = struct.unpack_from('<III', data, off); off += 12
        raw_bytes, = struct.unpack_from('<Q', data, off); off += 8
        comp_bytes, = struct.unpack_from('<Q', data, off); off += 8
        ev = zlib.decompress(data[off:off + comp_bytes])
        assert len(ev) == raw_bytes, f'event size {len(ev)} != {raw_bytes}'
        idxs = struct.unpack('<%dI' % ecount, ev) if ecount else ()
        assert all(i < nedge for i in idxs), 'event index out of range'
        print(f'events: {ecount} (flags={eflags} throttle={tfull}/{tevery}, '
              f'{comp_bytes} compressed bytes)')
        print('first 20 event edge-indices:', list(idxs[:20]))
```

(The dump script already unpacks `ver` in the header tuple.)

- [ ] **Step 2: Add the timed fixture builder**

In `tools/calltrace/make_test_xct.py`, refactor so the edge list is shared, then add a timed variant. Replace the `edges = [...]` literal inside `build()` with a module-level constant above `build()`:

```python
# Shared edge list (index order matters for the timed event stream).
EDGES = [
    (0x11010, 0x12000, 5),     # 0: A -> B
    (0x11020, 0x13000, 1),     # 1: A -> C
    (0x12040, 0x13000, 9),     # 2: B -> C
    (0x12050, 0x14000, 2),     # 3: B -> D1 (polymorphic site...)
    (0x12050, 0x15000, 4),     # 4: B -> D2 (...same call site)
    (0x13008, 0x80012345, 3),  # 5: C -> kernel leaf
    (0x13010, 0x13000, 2),     # 6: C -> C recursion
    (0x80020000, 0x16000, 1),  # 7: kernel -> T (thread root)
    (0x16008, 0x12000, 7),     # 8: T -> B
]
# A known event stream referencing EDGES by index.
EVENT_STREAM = [0, 1, 2, 3, 3, 3, 4, 4, 6, 7, 8, 0, 2]
```

and use `EDGES` where the old `edges` literal was. Then add, at the end of the file (before `if __name__`):

```python
def build_timed():
    import zlib
    base = bytearray(build())              # v1 body
    # bump version 1 -> 2 at offset 4
    struct.pack_into('<I', base, 4, 2)
    ev = struct.pack('<%dI' % len(EVENT_STREAM), *EVENT_STREAM)
    blob = zlib.compress(ev, 9)
    out = base
    out += struct.pack('<Q', len(EVENT_STREAM))     # event_count
    out += struct.pack('<III', 0, 256, 64)          # flags, throttle full/every
    out += struct.pack('<Q', len(ev))               # raw_bytes
    out += struct.pack('<Q', len(blob))             # comp_bytes
    out += blob
    return bytes(out)
```

and extend `__main__` to honor `--timed`:

```python
if __name__ == '__main__':
    import sys
    timed = '--timed' in sys.argv
    args = [a for a in sys.argv[1:] if a != '--timed']
    path = args[0] if args else ('test-fixture-timed.xct' if timed
                                 else 'test-fixture.xct')
    data = build_timed() if timed else build()
    open(path, 'wb').write(data)
    print(f'wrote {path}')
```

- [ ] **Step 3: Generate and validate both fixtures**

Run:
```bash
python3 tools/calltrace/make_test_xct.py tools/calltrace/test-fixture.xct
python3 tools/calltrace/make_test_xct.py --timed tools/calltrace/test-fixture-timed.xct
python3 tools/calltrace/xct_dump.py tools/calltrace/test-fixture-timed.xct
```
Expected: v1 dump unchanged (`OK`); timed dump additionally prints `events: 13 (flags=0 throttle=256/64, ... compressed bytes)` and `first 20 event edge-indices: [0, 1, 2, 3, 3, 3, 4, 4, 6, 7, 8, 0, 2]`, then `OK`.

- [ ] **Step 4: Validate a real v2 capture**

Run `python3 tools/calltrace/xct_dump.py <the .xct from Task 5>`.
Expected: prints an `events:` line with a large count and a much smaller compressed size; no assertion errors (all event indices in range).

- [ ] **Step 5: Commit**

```bash
git add tools/calltrace/make_test_xct.py tools/calltrace/xct_dump.py tools/calltrace/test-fixture-timed.xct
git commit -m "calltrace: Add v2 event dump and timed fixture"
```

---

### Task 7: Viewer — parse v2 + inflate + timeline derivation

**Files:**
- Modify: `tools/calltrace/viewer.html`

**Interfaces:**
- Consumes: v2 format; the async loading path; existing `parseXCT`, `deriveModel`, `loadRecording`, `model.entries`, `runSelfTests`, `T`/`assertEq`, `buildFixture`.
- Produces:
  - `parseXCT` (still sync) additionally returns, for v2: `rec.timed=true`, `rec.eventCount`, `rec.eventFlags`, `rec.throttle={full,every}`, `rec.eventComp` (Uint8Array of the compressed blob), `rec.eventRawBytes`. v1 → `rec.timed=false`.
  - `async inflateEvents(rec) -> Uint32Array|null` — inflates `rec.eventComp` via `DecompressionStream('deflate')`; returns null (and leaves `rec.timed=false`) on failure.
  - `fnOfSite(model, site) -> id` — module-level nearest-preceding-entry lookup (extracted from `deriveModel` and reused there).
  - After load: `model.events` (Uint32Array), `model.edgeFn` (array of `{callerId, calleeId}` per raw edge in index order), and `edge.firstFire` set on each function-edge object (min over its raw edges; `Infinity` if none). `model.eventCount`.
  - `loadRecording` becomes `async`.
  - Selftest harness `runSelfTests` awaits each test fn (async-capable).

- [ ] **Step 1: Add failing async selftests**

In `tools/calltrace/viewer.html`, after the existing `T('DOT export', ...)` block, add:

```js
async function buildTimedFixture() {
  const base = new Uint8Array(buildFixture());
  new DataView(base.buffer).setUint32(4, 2, true);         /* version 2 */
  const stream = [0, 1, 2, 3, 3, 3, 4, 4, 6, 7, 8, 0, 2];
  const ev = new Uint32Array(stream);
  const cs = new CompressionStream('deflate');
  const comp = new Uint8Array(await new Response(
      new Blob([ev.buffer]).stream().pipeThrough(cs)).arrayBuffer());
  const tail = new Uint8Array(8 + 12 + 8 + 8 + comp.length);
  const dv = new DataView(tail.buffer);
  dv.setBigUint64(0, BigInt(stream.length), true);         /* event_count */
  dv.setUint32(8, 0, true);                                /* flags */
  dv.setUint32(12, 256, true); dv.setUint32(16, 64, true); /* throttle */
  dv.setBigUint64(20, BigInt(ev.byteLength), true);        /* raw_bytes */
  dv.setBigUint64(28, BigInt(comp.length), true);          /* comp_bytes */
  tail.set(comp, 36);
  const out = new Uint8Array(base.length + tail.length);
  out.set(base, 0); out.set(tail, base.length);
  return out.buffer;
}

T('parse v2 header + inflate events', async () => {
  const rec = parseXCT(await buildTimedFixture());
  assertEq(rec.timed, true, 'timed flag');
  assertEq(rec.eventCount, 13, 'event count');
  assertEq(rec.throttle.full, 256, 'throttle full');
  const ev = await inflateEvents(rec);
  assertEq(ev.length, 13, 'inflated length');
  assertEq(ev[0], 0, 'ev0'); assertEq(ev[4], 3, 'ev4'); assertEq(ev[11], 0,
                                                                  'ev11');
});
T('firstFire + edgeFn derivation', async () => {
  const rec = parseXCT(await buildTimedFixture());
  const m = deriveModel(rec);
  m.events = await inflateEvents(rec);
  buildTimeline(m);
  /* edge 0 is A->B, first fires at event 0; edge 4 (B->D2) at event 6 */
  assertEq(m.fns.get(0x11000).out.get(0x12000).firstFire, 0, 'A->B ff');
  assertEq(m.fns.get(0x12000).out.get(0x15000).firstFire, 6, 'B->D2 ff');
  /* discoveredAt(P) grows monotonically */
  assertEq(discoveredEdgeCount(m, 0), 1, 'discovered at 0');
  assertEq(discoveredEdgeCount(m, 6) >= 4, true, 'discovered at 6');
});
```

- [ ] **Step 2: Run selftests to verify failure**

Serve and open `http://127.0.0.1:PORT/viewer.html?selftest=1`.
Expected: the two new tests FAIL (`inflateEvents`/`buildTimeline`/`discoveredEdgeCount` not defined); existing 8 still pass.

- [ ] **Step 3: Make the harness async and add the parse/derive code**

3a. Make `runSelfTests` await test fns. Replace its loop body:

```js
  for (const [name, fn] of selfTests) {
    try { fn(); out.push('PASS  ' + name); }
    catch (e) { fails++; out.push('FAIL  ' + name + ' — ' + e.message); }
  }
```

with:

```js
  for (const [name, fn] of selfTests) {
    try { await fn(); out.push('PASS  ' + name); }
    catch (e) { fails++; out.push('FAIL  ' + name + ' — ' + e.message); }
  }
```

and make `runSelfTests` `async function`, and its caller `await runSelfTests();` (in the `load` handler, change `runSelfTests();` to `runSelfTests(); return;` stays — just mark the handler `async` and `await`).

3b. Extract `fnOfSite` to module scope and reuse it in `deriveModel`. Add near `sectionIndexOf`:

```js
function fnOfSite(m, site) {
  const entries = m.entries;
  let lo = 0, hi = entries.length - 1, best = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (entries[mid] <= site) { best = entries[mid]; lo = mid + 1; }
    else hi = mid - 1;
  }
  return best;
}
```

In `deriveModel`, delete the local `const fnOf = site => {...}` and replace its use `callerId = fnOf(e.site);` with `callerId = fnOfSite({entries}, e.site);` — but since `entries` is in scope there, simplest: keep a local `const fnOf = site => fnOfSite({entries}, site);`.

3c. In `parseXCT`, after the edges loop and before `return {...}`, add v2 parsing:

```js
  let timed = false, eventCount = 0, eventFlags = 0, eventComp = null,
      eventRawBytes = 0, throttle = { full: 0, every: 0 };
  if (version >= 2 && off + 36 <= buf.byteLength) {
    timed = true;
    eventCount = Number(dv.getBigUint64(off, true)); off += 8;
    eventFlags = dv.getUint32(off, true); off += 4;
    throttle.full = dv.getUint32(off, true); off += 4;
    throttle.every = dv.getUint32(off, true); off += 4;
    eventRawBytes = Number(dv.getBigUint64(off, true)); off += 8;
    const compBytes = Number(dv.getBigUint64(off, true)); off += 8;
    eventComp = new Uint8Array(buf, off, compBytes);
  }
```

and add these to the returned object:

```js
  return { title, titleId, base, entry, sections, kimports, edges,
           truncated: !!(flags & 1),
           timed, eventCount, eventFlags, throttle, eventComp,
           eventRawBytes };
```

3d. Add the async inflate + timeline builders (place after `deriveModel`):

```js
async function inflateEvents(rec) {
  if (!rec.timed || !rec.eventComp) return null;
  try {
    const ds = new DecompressionStream('deflate');
    const raw = await new Response(
        new Blob([rec.eventComp]).stream().pipeThrough(ds)).arrayBuffer();
    return new Uint32Array(raw);
  } catch (e) {
    rec.timed = false;
    return null;
  }
}

/* Compute per-raw-edge caller/callee fns, per-function-edge first-fire. */
function buildTimeline(m) {
  const rec = m.rec;
  m.edgeFn = rec.edges.map(e => ({
    callerId: e.site >= KERNEL_BOUND ? -2
              : (fnOfSite(m, e.site) < 0 ? -3 : fnOfSite(m, e.site)),
    calleeId: e.callee,
  }));
  for (const f of m.fns.values()) {
    for (const edge of f.out.values()) edge.firstFire = Infinity;
  }
  const ev = m.events, ff = new Int32Array(rec.edges.length).fill(-1);
  if (ev) {
    for (let k = 0; k < ev.length; k++) {
      if (ff[ev[k]] < 0) ff[ev[k]] = k;
    }
  }
  m.firstFireRaw = ff;
  for (let i = 0; i < rec.edges.length; i++) {
    if (ff[i] < 0) continue;
    const { callerId, calleeId } = m.edgeFn[i];
    const f = m.fns.get(callerId);
    const edge = f && f.out.get(calleeId);
    if (edge) edge.firstFire = Math.min(edge.firstFire, ff[i]);
  }
  m.eventCount = ev ? ev.length : 0;
}

function discoveredEdgeCount(m, p) {
  let n = 0;
  for (const f of m.fns.values()) {
    for (const edge of f.out.values()) {
      if (edge.firstFire <= p) n++;
    }
  }
  return n;
}
```

3e. Make `loadRecording` async and call the timeline builders. Change its signature to `async function loadRecording(buf, name)` and, right after `model = deriveModel(rec);`, add:

```js
  if (rec.timed) {
    model.events = await inflateEvents(rec);
    buildTimeline(model);
  }
```

Update the two call sites to not depend on the return (they already ignore it): `f.arrayBuffer().then(buf => loadRecording(buf, f.name));` still works (async fn returns a promise).

- [ ] **Step 4: Run selftests to verify pass**

Reload `viewer.html?selftest=1`.
Expected: all 10 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/calltrace/viewer.html
git commit -m "calltrace: Parse v2 timeline events and derive first-fire data"
```

---

### Task 8: Viewer — timeline visibility + scrubber (no animation yet)

**Files:**
- Modify: `tools/calltrace/viewer.html`

**Interfaces:**
- Consumes: `buildTimeline` outputs (T7), `computeLayout`, `draw`, `el`.
- Produces: a `timeline` state `{playhead, playing, speed}`; `computeLayout` refactored to take an `edgeVisible(fromId, calleeId, edgeObj)` predicate; timeline mode sets visibility by `edgeObj.firstFire <= playhead`; a timeline bar (`#timeline`) with a scrub `<input type=range>` and a position readout, shown only when `model.timed`.

- [ ] **Step 1: Add the timeline bar markup + CSS**

After the `<div id="stats">` line, add:

```html
<div id="timeline" hidden>
  <button id="tl-play">▶</button>
  <input type="range" id="tl-scrub" min="0" max="0" value="0" step="1">
  <select id="tl-speed">
    <option value="1">1×</option>
    <option value="10">10×</option>
    <option value="100" selected>100×</option>
    <option value="1000">1000×</option>
    <option value="10000">10000×</option>
  </select>
  <span id="tl-readout">0 / 0</span>
</div>
```

In the `<style>` block, before `</style>`, add:

```css
  #timeline { display: none; align-items: center; gap: 8px; padding: 6px 12px;
              background: #1d2026; border-top: 1px solid #333; }
  #timeline.on { display: flex; }
  #timeline input[type=range] { flex: 1; }
  #timeline button, #timeline select { background: #2a2e36; color: #ddd;
              border: 1px solid #444; border-radius: 4px; padding: 2px 10px; }
  #tl-readout { font-family: monospace; color: #9ab; min-width: 160px;
              text-align: right; }
```

- [ ] **Step 2: Refactor computeLayout to a visibility predicate**

Replace the inner descent test in `computeLayout`. Change the function signature to `function computeLayout(m, edgeVisible)` and replace:

```js
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
```

with:

```js
    const f = m.fns.get(id);
    if (f) {
      for (const calleeId of [...f.out.keys()].sort((a, b) => a - b)) {
        if (!edgeVisible(id, calleeId, f.out.get(calleeId))) continue;
        if (visit(calleeId, depth + 1, n)) {
          n.treeKids.push(nodes.get(calleeId));
        }
      }
    }
```

In `draw`, where it calls `layoutNodes = computeLayout(model);`, replace with:

```js
  const edgeVisible = model.timed && timeline.active
      ? (from, cid, edge) => edge.firstFire <= timeline.playhead
      : (from) => expanded.has(from);
  layoutNodes = computeLayout(model, edgeVisible);
```

- [ ] **Step 3: Add timeline state and wiring**

Near the other globals (`let view = ...`), add:

```js
let timeline = { active: false, playhead: 0, playing: false, speed: 100 };
```

In `loadRecording`, after `buildTimeline(model)` (inside the `if (rec.timed)`), add:

```js
    timeline = { active: true, playhead: model.eventCount, playing: false,
                 speed: 100 };
    el('tl-scrub').max = String(model.eventCount);
    el('tl-scrub').value = String(model.eventCount);
    el('timeline').classList.add('on');
    updateReadout();
```

and in the non-timed path (e.g. right before the final `draw()` in `loadRecording`), ensure the bar is hidden:

```js
  if (!rec.timed) {
    timeline.active = false;
    el('timeline').classList.remove('on');
  }
```

Add these handlers near the other `el(...).addEventListener` calls:

```js
function updateReadout() {
  const p = timeline.playhead;
  let last = '';
  if (model && model.events && p > 0) {
    const e = model.edgeFn[model.events[p - 1]];
    last = ' · last: ' + (e ? fnName(model, e.calleeId) : '');
  }
  el('tl-readout').textContent = `${p} / ${model ? model.eventCount : 0}${last}`;
}
el('tl-scrub').addEventListener('input', ev => {
  timeline.playhead = parseInt(ev.target.value, 10);
  updateReadout();
  draw();
});
el('tl-speed').addEventListener('change', ev => {
  timeline.speed = parseInt(ev.target.value, 10);
});
```

- [ ] **Step 4: Verify scrubbing with the timed fixture**

Serve the folder; open `viewer.html`; drop `tools/calltrace/test-fixture-timed.xct`.
Expected: the timeline bar appears; dragging the scrubber to 0 shows only the Entry/roots; moving right progressively reveals `sub_00012000` (B), then `sub_00013000` (C), then D1/D2, etc., matching first-fire order; readout shows `P / 13 · last: <fn>`. Selftests (`?selftest=1`) still all pass.

- [ ] **Step 5: Commit**

```bash
git add tools/calltrace/viewer.html
git commit -m "calltrace: Add timeline scrubber with first-fire visibility"
```

---

### Task 9: Viewer — playback + synapse pulses

**Files:**
- Modify: `tools/calltrace/viewer.html`

**Interfaces:**
- Consumes: `timeline`, `layoutNodes`, `model.events`, `model.edgeFn`, `draw`, `MAX_PULSES`, `PULSE_LIFE_MS`.
- Produces: a RAF playback loop advancing `playhead` by `speed`/frame; a `pulses` array of `{fromId, toId, born}`; pulses drawn along their edge; play/pause button behavior; layout caching so it recomputes only when the discovered set changes.

- [ ] **Step 1: Add pulses + playback loop**

Near globals add:

```js
const MAX_PULSES = 200, PULSE_LIFE_MS = 400;
let pulses = [];
let lastLayoutKey = '';   /* invalidates cached layout when discovery grows */
let rafId = 0, lastTs = 0;
```

Add the loop and play toggle (near the timeline handlers):

```js
function spawnPulsesForRange(p0, p1) {
  const ev = model.events;
  for (let k = p0; k < p1; k++) {
    if (pulses.length >= MAX_PULSES) break;
    const ef = model.edgeFn[ev[k]];
    if (!ef) continue;
    if (mutedFns.has(ef.calleeId) || mutedFns.has(ef.callerId)) continue;
    pulses.push({ fromId: ef.callerId, toId: ef.calleeId, born: lastTs });
  }
}
function tick(ts) {
  if (!timeline.playing) { rafId = 0; return; }
  const dtStep = timeline.speed;
  const p0 = timeline.playhead;
  const p1 = Math.min(model.eventCount, p0 + dtStep);
  lastTs = ts;
  spawnPulsesForRange(p0, p1);
  timeline.playhead = p1;
  el('tl-scrub').value = String(p1);
  updateReadout();
  if (p1 >= model.eventCount) { timeline.playing = false; el('tl-play').textContent = '▶'; }
  draw();
  rafId = timeline.playing ? requestAnimationFrame(tick) : 0;
}
el('tl-play').addEventListener('click', () => {
  if (!model || !model.timed) return;
  if (timeline.playhead >= model.eventCount) timeline.playhead = 0;
  timeline.playing = !timeline.playing;
  el('tl-play').textContent = timeline.playing ? '❚❚' : '▶';
  if (timeline.playing && !rafId) rafId = requestAnimationFrame(tick);
});
```

Add `let mutedFns = new Set();` near globals (used here and by Task 10).

- [ ] **Step 2: Draw pulses and keep animating while alive**

In `draw`, after the loop that draws nodes (`for (const n of layoutNodes.values()) drawNode(ctx, n);`), add:

```js
  if (timeline.active && pulses.length) {
    const now = performance.now();
    pulses = pulses.filter(p => now - p.born < PULSE_LIFE_MS);
    for (const p of pulses) {
      const a = layoutNodes.get(p.fromId), b = layoutNodes.get(p.toId);
      if (!a || !b) continue;
      const t = (now - p.born) / PULSE_LIFE_MS;
      const x1 = a.x + a.w, y1 = a.y + a.h / 2, x2 = b.x, y2 = b.y + b.h / 2;
      const mx = (x1 + x2) / 2, u = 1 - t;
      /* cubic Bezier matching drawEdge: P0=(x1,y1) P1=(mx,y1) P2=(mx,y2) P3=(x2,y2) */
      const px = u*u*u*x1 + 3*u*u*t*mx + 3*u*t*t*mx + t*t*t*x2;
      const py = u*u*u*y1 + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y2;
      ctx.beginPath();
      ctx.arc(px, py, 3.5, 0, Math.PI * 2);
      ctx.fillStyle = `rgba(140,200,255,${1 - t})`;
      ctx.shadowColor = 'rgba(140,200,255,0.9)';
      ctx.shadowBlur = 8;
      ctx.fill();
      ctx.shadowBlur = 0;
    }
    if (timeline.active && (pulses.length || timeline.playing) && !rafId
        && !timeline.playing) {
      /* keep fading after the last step */
      requestAnimationFrame(() => draw());
    }
  }
```

- [ ] **Step 3: Cache layout across frames**

To avoid recomputing layout every frame during playback, wrap the layout call in `draw`:

```js
  const visKey = model.timed && timeline.active
      ? 'tl:' + discoveredEdgeCount(model, timeline.playhead)
      : 'manual:' + expanded.size + ':' + [...expanded].join(',');
  if (visKey !== lastLayoutKey || !layoutNodes.size) {
    const edgeVisible = model.timed && timeline.active
        ? (from, cid, edge) => edge.firstFire <= timeline.playhead
        : (from) => expanded.has(from);
    layoutNodes = computeLayout(model, edgeVisible);
    for (const n of layoutNodes.values()) {
      n.label = fnName(model, n.id);
      n.w = Math.min(220, Math.max(120, ctx.measureText(n.label).width + 40));
    }
    lastLayoutKey = visKey;
  }
```

Replace the existing `layoutNodes = computeLayout(...)` + label/width loop in `draw` with the cached block above. (Edges are still drawn every frame from `layoutNodes`.)

- [ ] **Step 4: Verify playback**

Serve; open `viewer.html`; drop `test-fixture-timed.xct`; set playhead to 0; press ▶.
Expected: the graph grows from Entry as pulses of light travel along each edge as it fires; at speed 1× you can watch 13 discrete pulses; the graph ends fully revealed. Then drop a REAL timed `.xct` from Task 5, set speed 1000×–10000×, press play: it grows out from Entry over a few seconds with electricity along active edges; scrubbing jumps instantly. Selftests still pass.

- [ ] **Step 5: Commit**

```bash
git add tools/calltrace/viewer.html
git commit -m "calltrace: Add timeline playback with synapse pulse animation"
```

---

### Task 10: Viewer — Noisiest Functions panel + ignore export

**Files:**
- Modify: `tools/calltrace/viewer.html`

**Interfaces:**
- Consumes: `model.fns` (inbound counts via `inSet`/edges), `showLegend`/details-pane pattern, `mutedFns` (T9), `fnName`, `hex`.
- Produces: a toolbar **Noise** button; `showNoise()` rendering a ranked list into the details pane with checkboxes; checking a row adds to `mutedFns` (hidden from graph + pulses) and to an export set; **Export ignore list** downloads `<title>.ignore.txt` (hex callee addresses, one per line); muted functions are hidden by `computeLayout` and skipped by pulses.

- [ ] **Step 1: Add the Noise button + toolbar wiring**

In the toolbar, after the `Legend` button, add:

```html
  <button id="btn-noise">Noise</button>
```

Add a selftest after the timeline tests:

```js
T('noise ranking by inbound calls', async () => {
  const rec = parseXCT(await buildTimedFixture());
  const m = deriveModel(rec);
  m.events = await inflateEvents(rec); buildTimeline(m);
  const ranked = noiseRanking(m);
  /* C (0x13000) has inbound 1+9+2 = 12, the most of any game fn */
  assertEq(ranked[0].id, 0x13000, 'top spammer is C');
  assertEq(ranked[0].count, 12, 'C inbound count');
});
```

- [ ] **Step 2: Run selftest to verify failure**

Reload `?selftest=1`. Expected: `noise ranking` FAILS (`noiseRanking` undefined); others pass.

- [ ] **Step 3: Implement ranking, panel, muting, export**

Add near `showLegend`:

```js
function noiseRanking(m) {
  const inbound = new Map();
  for (const f of m.fns.values()) {
    for (const [cid, edge] of f.out) {
      if (cid < 0) continue;
      inbound.set(cid, (inbound.get(cid) || 0) + edge.count);
    }
  }
  return [...inbound.entries()]
      .map(([id, count]) => ({ id, count }))
      .sort((a, b) => b.count - a.count);
}

let noiseExport = new Set();

function showNoise() {
  selected = null;
  if (!model) { el('details').innerHTML = '<i>No recording</i>'; return; }
  const ranked = noiseRanking(model).slice(0, 200);
  const rows = ['<h3>Noisiest Functions</h3>',
    '<div class="hint">Check to mute (hidden from the graph and playback) ' +
    'and include in the exported ignore list for your next run.</div>',
    '<button id="noise-export">Export ignore list</button>'];
  for (const r of ranked) {
    const on = noiseExport.has(r.id) ? ' checked' : '';
    rows.push(`<div class="leg"><input type="checkbox" data-id="${r.id}"` +
              `${on} style="margin-right:8px"><span>` +
              `${esc(fnName(model, r.id))} <small>${hex(r.id)} · ` +
              `${r.count.toLocaleString()} calls</small></span></div>`);
  }
  el('details').innerHTML = rows.join('');
  for (const cb of el('details').querySelectorAll('input[type=checkbox]')) {
    cb.onchange = () => {
      const id = parseInt(cb.dataset.id, 10);
      if (cb.checked) { noiseExport.add(id); mutedFns.add(id); }
      else { noiseExport.delete(id); mutedFns.delete(id); }
      lastLayoutKey = '';   /* force relayout to drop/readd the node */
      draw();
    };
  }
  el('noise-export').onclick = exportIgnore;
}

function exportIgnore() {
  if (!noiseExport.size) { showBanner('Check some functions first', 'warn');
                           return; }
  const lines = ['# xemu call-trace ignore list (callee addresses)'];
  for (const id of [...noiseExport].sort((a, b) => a - b)) {
    lines.push(hex(id));
  }
  const blob = new Blob([lines.join('\n') + '\n'], { type: 'text/plain' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = (model.rec.title ? model.rec.title.replace(/\W+/g, '_')
                                : 'calltrace') + '.ignore.txt';
  a.click();
  URL.revokeObjectURL(a.href);
}
el('btn-noise').addEventListener('click', () => { showNoise(); });
```

- [ ] **Step 4: Make muted functions vanish from the graph**

In `computeLayout`, at the top of `visit`, add a guard so muted nodes never render:

```js
  const visit = (id, depth, parent) => {
    if (id >= 0 && mutedFns.has(id)) return false;
    if (nodes.has(id)) return false;
```

(Pulses already skip muted fns from Task 9. `lastLayoutKey` is reset on mute so the cache rebuilds.)

- [ ] **Step 5: Run selftests + manual check, then commit**

Reload `?selftest=1` → all 11 tests PASS. Then load a real timed `.xct`, click **Noise**: the top row is the spammy poller (e.g. `mm3_input_action_pressed_on_any_device`) with a huge count; checking it removes it from the graph and pulses; **Export ignore list** downloads `<title>.ignore.txt`. Copy that file to your recordings dir as `ignore.txt` and, in xemu, `Load ignore list…` before the next Timed run to confirm those callees vanish.

```bash
git add tools/calltrace/viewer.html
git commit -m "calltrace: Add Noisiest Functions panel and ignore-list export"
```

---

### Task 11: End-to-end validation

**Files:** none (verification only)

- [ ] **Step 1: Full timed loop**

Build, boot a game, `Start - Timed`, play ~30 s across a state change, `Stop & Save`. Run `python3 tools/calltrace/xct_dump.py <file>` — confirm `events:` count is large, compressed size ≪ `events×4`, no index-range assertion errors.

- [ ] **Step 2: Viewer replay**

Open the `.xct` in the viewer: timeline bar present; scrub 0→end reveals the graph growing from Entry; play at a high speed shows synapse pulses; the Noise panel ranks real spammers; muting + export writes an ignore file.

- [ ] **Step 3: Ignore round-trip**

Put the exported file as `<recordings>/ignore.txt`, `Load ignore list…`, record Timed again, save. Confirm in `xct_dump.py` the muted callees are absent (no edges into them) and the event count/size dropped.

- [ ] **Step 4: Regression**

Confirm an **Edges** recording still writes `version=1` and opens in the viewer with no timeline bar (byte-compatible with the pre-existing flow). All viewer selftests green.

---

## Self-Review Notes (already applied)

- Spec coverage: modes/depth (T3/T4), edge index + event log (T1/T2/T3), adaptive throttle (T3), ignore-list load + hot-path drop (T3/T4), v2 format with compressed events in index order (T5), dump/fixtures (T6), viewer v2 parse + inflate + first-fire (T7), timeline visibility + scrubber (T8), auto-reveal growth + synapse pulses + speed (T9), noise ranking + mute + ignore export (T10), truncation flags surfaced (T3 engine, T4 menu, viewer via `eventFlags`), end-to-end incl. ignore round-trip (T11).
- `loadRecording`/`parseXCT` async split keeps existing sync selftests intact; only event inflation is async, and the harness now awaits.
- Names kept consistent across tasks: `ct_map_add_indexed`, `CTEvents`/`ct_events_*`, `inflateEvents`, `buildTimeline`, `discoveredEdgeCount`, `noiseRanking`, `mutedFns`, `timeline.{active,playhead,playing,speed}`.
- Non-goals (real-time axis, RET/durations, per-event args, on-disk streaming, in-xemu ignore editing) are excluded from all tasks.

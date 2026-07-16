# Frame Inspector Core (Plan 1 of 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the guest-side instrumentation core of the frame inspector — shadow call trees with per-thread stacks, the RAM-wide writer tag map, TCG CALL/RET/store hooks, and the watched-call engine — end-to-end testable via a Ctrl+Alt+I arm/disarm hotkey with an on-screen stats toast.

**Architecture:** Header-only, QEMU-independent data structures (call tree + argset pool, tag map, shadow stacks, watch engine) unit-tested standalone, glued together by `xemu-frameinspect.c` which owns arm/disarm and the record entry points. TCG hooks follow the existing calltrace pattern exactly: translation-time `if (armed)` guards + `queue_tb_flush()` on toggle, helpers in `misc_helper.c`. Plan 2 (NV2A capture engine) and Plan 3 (ImGui inspector UI) build on the interfaces produced here.

**Tech Stack:** C11 (GCC builtins for atomics so headers stay standalone), QEMU TCG x86 frontend, SDL hotkey + xemu notification toast.

**Spec:** `docs/superpowers/specs/2026-07-16-frame-inspector-design.md`

## Global Constraints

- Header-only `xemu-frameinspect-*.h` files stay **QEMU-independent** (standalone-testable with plain gcc; no glib, no QEMU headers).
- Standalone tests build with `/c/msys64/ucrt64/bin/gcc.exe` in Git Bash (plain `gcc` is FPC's gcc 2.95 — never use it). Emulator builds: `ninja -C build` from an MSYS2 UCRT64 shell (see memory `xemu-windows-build-env`).
- Attribution must **degrade to missing, never wrong**: tags publish only after a store succeeds; desync/discontinuity resets to the unknown root; no synthesized data.
- Caps (from spec, verbatim): path nodes 512 K, argsets 256 K, shadow stacks 64 threads × 512 frames, bounded RET pop 64, watches 16, invocations/watch 1024, write log 1 M records / 24 MB byte pool.
- Tag word semantics: `0` = unattributed; else `(node_id + 1)`, high bit `0x80000000` = partial-dword flag.
- Xbox is single-vCPU; per-vCPU state may be a single global. Guest thread key = current KTHREAD pointer read from `fs:[0x28]` (KPCR.PrcbData.CurrentThread, fixed kernel ABI).
- License headers: GPL-2.0-or-later, "Copyright (C) 2026 xemu contributors" (match `xemu-calltrace.h`).
- Naming: `fi_`/`FI` prefixes in standalone headers; `xemu_frameinspect_` for the QEMU-facing API; TCG helpers `xemu_fi_*`.
- Commit prefix: `frameinspect:`.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `xemu-frameinspect-calltree.h` | create | Path-node interning (parent, call_site, callee) + global argset pool |
| `xemu-frameinspect-tagmap.h` | create | RAM-wide dword writer tags, release/acquire, partial flag |
| `xemu-frameinspect-shadowstack.h` | create | Per-thread shadow stacks, bounded pop, ESP-discontinuity, watch depth |
| `xemu-frameinspect-watch.h` | create | Watch set, invocation records, coalescing write log |
| `xemu-frameinspect.h` / `.c` | create | Arm/disarm state machine, record entry points, status line |
| `target/i386/helper.h` | modify (after line 238) | `xemu_fi_call/ret/store_pre/store_post` declarations |
| `target/i386/tcg/misc_helper.c` | modify (after line 180) | Helper implementations, thread-key cache, phys-translate cache |
| `target/i386/tcg/emit.c.inc` | modify (`gen_CALL*` ~1605-1642, `gen_RET` ~3637) | CALL/RET instrumentation |
| `target/i386/tcg/translate.c` | modify (`gen_op_st_v` line 628) | Post-store (and watch-mode pre-store) hook |
| `meson.build` | modify (line 4054) | Add `xemu-frameinspect.c` to `specific_ss` |
| `ui/xemu.c` | modify (scancode switch ~line 408) | Ctrl+Alt+I arm/disarm + stats toast |
| `tests/frameinspect/test-fi-calltree.c` | create | Unit tests |
| `tests/frameinspect/test-fi-tagmap.c` | create | Unit tests |
| `tests/frameinspect/test-fi-shadowstack.c` | create | Unit tests |
| `tests/frameinspect/test-fi-watch.c` | create | Unit tests |

---

### Task 1: Call tree + argset pool header

**Files:**
- Create: `xemu-frameinspect-calltree.h`
- Test: `tests/frameinspect/test-fi-calltree.c`

**Interfaces:**
- Consumes: nothing (standalone).
- Produces: `FICallTree`; `bool fi_calltree_init(FICallTree *t)`; `void fi_calltree_free(FICallTree *t)`; `uint32_t fi_calltree_intern(FICallTree *t, uint32_t parent, uint32_t call_site, uint32_t callee)` returning a node id or `FI_NODE_INVALID` when capped; `void fi_calltree_add_args(FICallTree *t, uint32_t node, const uint32_t args[6])`; constants `FI_NODE_ROOT` (=0, the unknown root, pre-created by init), `FI_NODE_INVALID` (=0xFFFFFFFF), `FI_CT_MAX_NODES`, `FI_CT_MAX_ARGSETS`. Node fields: `parent`, `call_site`, `callee`, `n_argsets`, `argset_ids[FI_NODE_ARGSETS]`. Argsets readable via `t->argsets[id][0..5]`.

- [ ] **Step 1: Write the failing test**

Create `tests/frameinspect/test-fi-calltree.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xemu-frameinspect-calltree.h"

int main(void)
{
    FICallTree t;
    assert(fi_calltree_init(&t));

    /* init pre-creates the unknown root as node 0 */
    assert(t.num_nodes == 1);
    assert(t.nodes[FI_NODE_ROOT].parent == FI_NODE_INVALID);

    /* interning the same triple twice returns the same node */
    uint32_t a = fi_calltree_intern(&t, FI_NODE_ROOT, 0x11000, 0x22000);
    uint32_t b = fi_calltree_intern(&t, FI_NODE_ROOT, 0x11000, 0x22000);
    assert(a != FI_NODE_INVALID && a == b);
    assert(t.num_nodes == 2);

    /* different parent => different path node, same (site, callee) */
    uint32_t c = fi_calltree_intern(&t, a, 0x11000, 0x22000);
    assert(c != a && c != FI_NODE_INVALID);
    assert(t.nodes[c].parent == a);
    assert(t.nodes[c].call_site == 0x11000 && t.nodes[c].callee == 0x22000);

    /* argsets: dedup within and across nodes; per-node id-slot cap */
    uint32_t s1[6] = {1, 2, 3, 4, 5, 6};
    uint32_t s2[6] = {9, 2, 3, 4, 5, 6};
    fi_calltree_add_args(&t, a, s1);
    fi_calltree_add_args(&t, a, s1);          /* dup: no new set/slot */
    fi_calltree_add_args(&t, a, s2);
    assert(t.nodes[a].n_argsets == 2);
    assert(t.num_argsets == 2);
    fi_calltree_add_args(&t, c, s1);          /* cross-node dedup: pool stays 2 */
    assert(t.num_argsets == 2 && t.nodes[c].n_argsets == 1);
    assert(memcmp(t.argsets[t.nodes[a].argset_ids[0]], s1, 24) == 0);
    for (int i = 0; i < FI_NODE_ARGSETS + 3; i++) {
        uint32_t sx[6] = {100u + (uint32_t)i, 0, 0, 0, 0, 0};
        fi_calltree_add_args(&t, c, sx);
    }
    assert(t.nodes[c].n_argsets == FI_NODE_ARGSETS); /* slots capped, no overflow */

    /* node cap: refuse with FI_NODE_INVALID and set the truncated flag */
    while (t.num_nodes < FI_CT_MAX_NODES) {
        fi_calltree_intern(&t, FI_NODE_ROOT, 0x100000u + t.num_nodes, 0x5);
    }
    assert(fi_calltree_intern(&t, FI_NODE_ROOT, 0xF000F000u, 0xE000E000u)
           == FI_NODE_INVALID);
    assert(t.nodes_truncated);
    assert(fi_calltree_intern(&t, FI_NODE_ROOT, 0x11000, 0x22000) == a);

    fi_calltree_free(&t);
    assert(t.nodes == NULL);
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run (Git Bash): `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-calltree.exe tests/frameinspect/test-fi-calltree.c`
Expected: FAIL — `xemu-frameinspect-calltree.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `xemu-frameinspect-calltree.h`:

```c
/*
 * xemu frame inspector: interned call-path tree + argument-set pool
 *
 * Path nodes are interned by (parent, call_site, callee) so one node
 * represents one distinct call path. Argument sets (6 dwords captured at
 * call time) are interned in a global pool and referenced by id, so
 * varying arguments never duplicate path structure. Header-only and
 * QEMU-independent for standalone unit testing.
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

#ifndef XEMU_FRAMEINSPECT_CALLTREE_H
#define XEMU_FRAMEINSPECT_CALLTREE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_CT_MAX_NODES   (1u << 19)  /* 512 K path nodes */
#define FI_CT_HASH_CAP    (1u << 20)  /* open-addressed slots (2x nodes) */
#define FI_CT_MAX_ARGSETS (1u << 18)  /* 256 K distinct argument sets */
#define FI_CT_ARGHASH_CAP (1u << 19)
#define FI_NODE_ARGSETS   4           /* argset-id slots per node */

#define FI_NODE_INVALID 0xFFFFFFFFu
#define FI_NODE_ROOT    0u            /* the unknown root */

typedef struct FINode {
    uint32_t parent;                  /* node id or FI_NODE_INVALID */
    uint32_t call_site;
    uint32_t callee;
    uint16_t n_argsets;
    uint16_t argsets_truncated;
    uint32_t argset_ids[FI_NODE_ARGSETS];
} FINode;

typedef struct FICallTree {
    FINode *nodes;                    /* [FI_CT_MAX_NODES] */
    uint32_t num_nodes;
    uint64_t *hash_keys;              /* [FI_CT_HASH_CAP], 0 = empty */
    uint32_t *hash_vals;
    uint32_t (*argsets)[6];           /* [FI_CT_MAX_ARGSETS] */
    uint32_t num_argsets;
    uint64_t *arg_keys;               /* [FI_CT_ARGHASH_CAP], 0 = empty */
    uint32_t *arg_vals;
    bool nodes_truncated;
    bool pool_truncated;
} FICallTree;

/* 64-bit avalanche mix (splitmix64 finalizer); | 1 reserves 0 = empty. */
static inline uint64_t fi_ct_mix(uint64_t x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x | 1;
}

static inline bool fi_calltree_init(FICallTree *t)
{
    memset(t, 0, sizeof(*t));
    t->nodes = calloc(FI_CT_MAX_NODES, sizeof(FINode));
    t->hash_keys = calloc(FI_CT_HASH_CAP, sizeof(uint64_t));
    t->hash_vals = calloc(FI_CT_HASH_CAP, sizeof(uint32_t));
    t->argsets = calloc(FI_CT_MAX_ARGSETS, sizeof(*t->argsets));
    t->arg_keys = calloc(FI_CT_ARGHASH_CAP, sizeof(uint64_t));
    t->arg_vals = calloc(FI_CT_ARGHASH_CAP, sizeof(uint32_t));
    if (!t->nodes || !t->hash_keys || !t->hash_vals || !t->argsets ||
        !t->arg_keys || !t->arg_vals) {
        free(t->nodes); free(t->hash_keys); free(t->hash_vals);
        free(t->argsets); free(t->arg_keys); free(t->arg_vals);
        memset(t, 0, sizeof(*t));
        return false;
    }
    t->nodes[FI_NODE_ROOT].parent = FI_NODE_INVALID;
    t->num_nodes = 1;
    return true;
}

static inline void fi_calltree_free(FICallTree *t)
{
    free(t->nodes); free(t->hash_keys); free(t->hash_vals);
    free(t->argsets); free(t->arg_keys); free(t->arg_vals);
    memset(t, 0, sizeof(*t));
    t->nodes = NULL;
}

static inline uint32_t fi_calltree_intern(FICallTree *t, uint32_t parent,
                                          uint32_t call_site, uint32_t callee)
{
    uint64_t key = fi_ct_mix(((uint64_t)parent << 32) ^
                             ((uint64_t)call_site << 16) ^ callee);
    uint32_t i = (uint32_t)key & (FI_CT_HASH_CAP - 1);
    for (;;) {
        if (t->hash_keys[i] == key) {
            FINode *n = &t->nodes[t->hash_vals[i]];
            if (n->parent == parent && n->call_site == call_site &&
                n->callee == callee) {
                return t->hash_vals[i];
            }
        } else if (t->hash_keys[i] == 0) {
            break;
        }
        i = (i + 1) & (FI_CT_HASH_CAP - 1);
    }
    if (t->num_nodes >= FI_CT_MAX_NODES) {
        t->nodes_truncated = true;
        return FI_NODE_INVALID;
    }
    uint32_t id = t->num_nodes++;
    FINode *n = &t->nodes[id];
    n->parent = parent;
    n->call_site = call_site;
    n->callee = callee;
    t->hash_keys[i] = key;
    t->hash_vals[i] = id;
    return id;
}

static inline void fi_calltree_add_args(FICallTree *t, uint32_t node,
                                        const uint32_t args[6])
{
    if (node >= t->num_nodes) {
        return;
    }
    uint64_t key = fi_ct_mix(((uint64_t)args[0] << 32) ^ args[1]) ^
                   fi_ct_mix(((uint64_t)args[2] << 32) ^ args[3]) ^
                   fi_ct_mix(((uint64_t)args[4] << 32) ^ args[5]);
    key |= 1;
    uint32_t i = (uint32_t)key & (FI_CT_ARGHASH_CAP - 1);
    uint32_t id = FI_NODE_INVALID;
    for (;;) {
        if (t->arg_keys[i] == key &&
            memcmp(t->argsets[t->arg_vals[i]], args, 24) == 0) {
            id = t->arg_vals[i];
            break;
        }
        if (t->arg_keys[i] == 0) {
            if (t->num_argsets >= FI_CT_MAX_ARGSETS) {
                t->pool_truncated = true;
                return;
            }
            id = t->num_argsets++;
            memcpy(t->argsets[id], args, 24);
            t->arg_keys[i] = key;
            t->arg_vals[i] = id;
            break;
        }
        i = (i + 1) & (FI_CT_ARGHASH_CAP - 1);
    }
    FINode *n = &t->nodes[node];
    for (uint32_t k = 0; k < n->n_argsets; k++) {
        if (n->argset_ids[k] == id) {
            return;                   /* already attached */
        }
    }
    if (n->n_argsets < FI_NODE_ARGSETS) {
        n->argset_ids[n->n_argsets++] = id;
    } else {
        n->argsets_truncated = 1;
    }
}

#endif
```

- [ ] **Step 4: Run test to verify it passes**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-calltree.exe tests/frameinspect/test-fi-calltree.c && ./tests/frameinspect/test-fi-calltree.exe`
Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add xemu-frameinspect-calltree.h tests/frameinspect/test-fi-calltree.c
git commit -m "frameinspect: Add interned call-path tree + argset pool"
```

---

### Task 2: Tag map header

**Files:**
- Create: `xemu-frameinspect-tagmap.h`
- Test: `tests/frameinspect/test-fi-tagmap.c`

**Interfaces:**
- Consumes: nothing (standalone).
- Produces: `FITagMap`; `bool fi_tagmap_init(FITagMap *m, uint64_t ram_size)`; `void fi_tagmap_free(FITagMap *m)`; `void fi_tagmap_tag(FITagMap *m, uint64_t paddr, uint32_t len, uint32_t node)` (release-store per overlapped dword; publishes `node + 1`, ORs `FI_TAG_PARTIAL` on partially covered dwords; silently ignores out-of-range bytes); `uint32_t fi_tagmap_lookup(const FITagMap *m, uint64_t paddr)` (acquire-load of the raw tag word; 0 = unattributed); macros `FI_TAG_PARTIAL` (0x80000000), `FI_TAG_NODE(tag)` (`((tag) & 0x7FFFFFFF) - 1`, only valid when tag ≠ 0).

- [ ] **Step 1: Write the failing test**

Create `tests/frameinspect/test-fi-tagmap.c`:

```c
#include <assert.h>
#include <stdio.h>
#include "../../xemu-frameinspect-tagmap.h"

int main(void)
{
    FITagMap m;
    assert(fi_tagmap_init(&m, 1024));

    /* untagged reads 0 */
    assert(fi_tagmap_lookup(&m, 0x10) == 0);

    /* aligned full-dword store: clean tag, node recoverable */
    fi_tagmap_tag(&m, 0x10, 4, 7);
    uint32_t tag = fi_tagmap_lookup(&m, 0x10);
    assert(tag != 0 && !(tag & FI_TAG_PARTIAL) && FI_TAG_NODE(tag) == 7);

    /* node 0 (unknown root) is distinguishable from unattributed */
    fi_tagmap_tag(&m, 0x20, 4, 0);
    tag = fi_tagmap_lookup(&m, 0x20);
    assert(tag != 0 && FI_TAG_NODE(tag) == 0);

    /* byte store: partial flag on its dword */
    fi_tagmap_tag(&m, 0x31, 1, 9);
    tag = fi_tagmap_lookup(&m, 0x30);
    assert((tag & FI_TAG_PARTIAL) && FI_TAG_NODE(tag) == 9);

    /* unaligned 4-byte store crossing a dword boundary: both partial */
    fi_tagmap_tag(&m, 0x42, 4, 3);
    assert(fi_tagmap_lookup(&m, 0x40) & FI_TAG_PARTIAL);
    assert(fi_tagmap_lookup(&m, 0x44) & FI_TAG_PARTIAL);
    assert(FI_TAG_NODE(fi_tagmap_lookup(&m, 0x44)) == 3);

    /* aligned 8-byte store: both dwords clean */
    fi_tagmap_tag(&m, 0x50, 8, 4);
    assert(!(fi_tagmap_lookup(&m, 0x50) & FI_TAG_PARTIAL));
    assert(!(fi_tagmap_lookup(&m, 0x54) & FI_TAG_PARTIAL));

    /* out-of-range bytes ignored; in-range prefix still tagged */
    fi_tagmap_tag(&m, 1020, 8, 5);
    assert(FI_TAG_NODE(fi_tagmap_lookup(&m, 1020)) == 5);
    assert(fi_tagmap_lookup(&m, 1024) == 0);   /* also out-of-range read */

    fi_tagmap_free(&m);
    assert(m.tags == NULL);
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-tagmap.exe tests/frameinspect/test-fi-tagmap.c`
Expected: FAIL — `xemu-frameinspect-tagmap.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `xemu-frameinspect-tagmap.h`:

```c
/*
 * xemu frame inspector: RAM-wide writer tag map
 *
 * One uint32_t per dword of guest RAM recording the call-path node that
 * last stored there. Tag 0 = unattributed; otherwise (node_id + 1), with
 * the high bit flagging a partially covered dword. Written on the vCPU
 * thread (release), read on the PFIFO thread (acquire). Header-only and
 * QEMU-independent for standalone unit testing.
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

#ifndef XEMU_FRAMEINSPECT_TAGMAP_H
#define XEMU_FRAMEINSPECT_TAGMAP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_TAG_PARTIAL 0x80000000u
#define FI_TAG_NODE(tag) ((((tag) & 0x7FFFFFFFu)) - 1u)

typedef struct FITagMap {
    uint32_t *tags;                   /* one per RAM dword */
    uint64_t ram_size;
} FITagMap;

static inline bool fi_tagmap_init(FITagMap *m, uint64_t ram_size)
{
    m->ram_size = ram_size;
    m->tags = calloc(ram_size / 4, sizeof(uint32_t));
    return m->tags != NULL;
}

static inline void fi_tagmap_free(FITagMap *m)
{
    free(m->tags);
    m->tags = NULL;
    m->ram_size = 0;
}

static inline void fi_tagmap_tag(FITagMap *m, uint64_t paddr, uint32_t len,
                                 uint32_t node)
{
    if (!m->tags || paddr >= m->ram_size) {
        return;
    }
    if (len > m->ram_size - paddr) {
        len = (uint32_t)(m->ram_size - paddr);
    }
    uint64_t end = paddr + len;
    for (uint64_t d = paddr & ~3ull; d < end; d += 4) {
        uint32_t tag = node + 1;
        if (d < paddr || d + 4 > end) {
            tag |= FI_TAG_PARTIAL;    /* store covers only part of dword */
        }
        __atomic_store_n(&m->tags[d >> 2], tag, __ATOMIC_RELEASE);
    }
}

static inline uint32_t fi_tagmap_lookup(const FITagMap *m, uint64_t paddr)
{
    if (!m->tags || paddr >= m->ram_size) {
        return 0;
    }
    return __atomic_load_n(&m->tags[paddr >> 2], __ATOMIC_ACQUIRE);
}

#endif
```

- [ ] **Step 4: Run test to verify it passes**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-tagmap.exe tests/frameinspect/test-fi-tagmap.c && ./tests/frameinspect/test-fi-tagmap.exe`
Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add xemu-frameinspect-tagmap.h tests/frameinspect/test-fi-tagmap.c
git commit -m "frameinspect: Add RAM-wide writer tag map"
```

---

### Task 3: Shadow stack header

**Files:**
- Create: `xemu-frameinspect-shadowstack.h`
- Test: `tests/frameinspect/test-fi-shadowstack.c`

**Interfaces:**
- Consumes: `FICallTree`, `fi_calltree_intern`, `FI_NODE_ROOT`, `FI_NODE_INVALID` (Task 1).
- Produces: `FIShadow`; `void fi_shadow_init(FIShadow *s, FICallTree *tree)`; `uint32_t fi_shadow_call(FIShadow *s, uint32_t thread_key, uint32_t call_site, uint32_t callee, uint32_t ret_addr, uint32_t esp)` returning the callee's node id (never `FI_NODE_INVALID`; falls back to `FI_NODE_ROOT`); `uint32_t fi_shadow_ret(FIShadow *s, uint32_t thread_key, uint32_t ret_target, FIPopped popped[], uint32_t *n_popped)` returning the node now current; `uint32_t fi_shadow_current(FIShadow *s, uint32_t thread_key)`; `void fi_shadow_set_watch(FIShadow *s, uint32_t thread_key, uint32_t invoc)` marking the just-pushed frame watched; `bool fi_shadow_in_watch(FIShadow *s, uint32_t thread_key)`. `FIPopped` = `{ uint32_t watch_invoc; bool clean; }` (only watched frames are reported; `clean` true only for a frame popped by its matching RET). Constants `FI_SS_MAX_THREADS` 64, `FI_SS_MAX_DEPTH` 512, `FI_SS_MAX_POPS` 64, `FI_INVOC_INVALID` 0xFFFFFFFF.

- [ ] **Step 1: Write the failing test**

Create `tests/frameinspect/test-fi-shadowstack.c`:

```c
#include <assert.h>
#include <stdio.h>
#include "../../xemu-frameinspect-calltree.h"
#include "../../xemu-frameinspect-shadowstack.h"

int main(void)
{
    FICallTree t;
    FIShadow s;
    FIPopped pops[FI_SS_MAX_POPS];
    uint32_t np;
    assert(fi_calltree_init(&t));
    fi_shadow_init(&s, &t);

    /* empty stack: current is the unknown root */
    assert(fi_shadow_current(&s, 0xD0000100) == FI_NODE_ROOT);

    /* call chain builds path nodes: A -> B */
    uint32_t a = fi_shadow_call(&s, 0xD0000100, 0x1000, 0x2000, 0x1005, 0x7000);
    uint32_t b = fi_shadow_call(&s, 0xD0000100, 0x2010, 0x3000, 0x2015, 0x6FF0);
    assert(a != FI_NODE_ROOT && t.nodes[b].parent == a);
    assert(fi_shadow_current(&s, 0xD0000100) == b);

    /* matched RET pops exactly one frame */
    assert(fi_shadow_ret(&s, 0xD0000100, 0x2015, pops, &np) == a);
    assert(np == 0);   /* nothing watched */

    /* RET skipping frames (e.g. after longjmp-like unwind) pops through */
    uint32_t b2 = fi_shadow_call(&s, 0xD0000100, 0x2010, 0x3000, 0x2015, 0x6FF0);
    fi_shadow_call(&s, 0xD0000100, 0x3010, 0x4000, 0x3015, 0x6FE0);
    (void)b2;
    assert(fi_shadow_ret(&s, 0xD0000100, 0x1005, pops, &np) == FI_NODE_ROOT);

    /* unmatched RET = desync: reset to unknown root, no fabricated parent */
    fi_shadow_call(&s, 0xD0000100, 0x1000, 0x2000, 0x1005, 0x7000);
    assert(fi_shadow_ret(&s, 0xD0000100, 0xDEAD, pops, &np) == FI_NODE_ROOT);
    assert(fi_shadow_current(&s, 0xD0000100) == FI_NODE_ROOT);

    /* separate thread keys get separate stacks */
    uint32_t t1 = fi_shadow_call(&s, 0xD0000100, 0x1000, 0x2000, 0x1005, 0x7000);
    uint32_t t2 = fi_shadow_call(&s, 0xD0000200, 0x5000, 0x6000, 0x5005, 0x9000);
    assert(fi_shadow_current(&s, 0xD0000100) == t1);
    assert(fi_shadow_current(&s, 0xD0000200) == t2);
    assert(t.nodes[t2].parent == FI_NODE_ROOT);

    /* ESP discontinuity: a CALL above dead frames pops them first */
    fi_shadow_ret(&s, 0xD0000100, 0x1005, pops, &np);       /* empty stack */
    fi_shadow_call(&s, 0xD0000100, 0x1000, 0x2000, 0x1005, 0x7000);
    fi_shadow_call(&s, 0xD0000100, 0x2010, 0x3000, 0x2015, 0x6FF0);
    uint32_t d = fi_shadow_call(&s, 0xD0000100, 0x1100, 0x8000, 0x1105, 0x7100);
    assert(t.nodes[d].parent == FI_NODE_ROOT); /* not under stale frames */

    /* watch marking + pop reporting: clean on matched RET */
    fi_shadow_ret(&s, 0xD0000100, 0x1105, pops, &np);
    uint32_t w = fi_shadow_call(&s, 0xD0000100, 0x1000, 0x2000, 0x1005, 0x7000);
    (void)w;
    fi_shadow_set_watch(&s, 0xD0000100, 42);
    assert(fi_shadow_in_watch(&s, 0xD0000100));
    fi_shadow_call(&s, 0xD0000100, 0x2010, 0x3000, 0x2015, 0x6FF0);
    assert(fi_shadow_in_watch(&s, 0xD0000100));  /* subtree inherits */
    fi_shadow_ret(&s, 0xD0000100, 0x2015, pops, &np);
    assert(np == 0);                              /* watched frame still open */
    fi_shadow_ret(&s, 0xD0000100, 0x1005, pops, &np);
    assert(np == 1 && pops[0].watch_invoc == 42 && pops[0].clean);
    assert(!fi_shadow_in_watch(&s, 0xD0000100));

    /* watched frame killed by desync reports unclean */
    fi_shadow_call(&s, 0xD0000100, 0x1000, 0x2000, 0x1005, 0x7000);
    fi_shadow_set_watch(&s, 0xD0000100, 43);
    fi_shadow_ret(&s, 0xD0000100, 0xDEAD, pops, &np);
    assert(np == 1 && pops[0].watch_invoc == 43 && !pops[0].clean);

    /* depth overflow: calls beyond FI_SS_MAX_DEPTH keep returning a node */
    for (int i = 0; i < FI_SS_MAX_DEPTH + 8; i++) {
        uint32_t n = fi_shadow_call(&s, 0xD0000300, 0x1000u + (uint32_t)i,
                                    0x2000u + (uint32_t)i, 0x1005u + (uint32_t)i,
                                    0x9000u - 16u * (uint32_t)i);
        assert(n != FI_NODE_INVALID);
    }

    fi_calltree_free(&t);
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-shadowstack.exe tests/frameinspect/test-fi-shadowstack.c`
Expected: FAIL — `xemu-frameinspect-shadowstack.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `xemu-frameinspect-shadowstack.h`:

```c
/*
 * xemu frame inspector: per-guest-thread shadow call stacks
 *
 * Tracks the live call path per guest thread (keyed by KTHREAD pointer)
 * from CALL/RET instrumentation. RETs pop by matching the recorded
 * return address (bounded search); mismatches and ESP discontinuities
 * reset toward the unknown root so attribution degrades to missing,
 * never fabricated. Watched frames propagate a watch depth so store
 * instrumentation knows when it is inside a watched subtree.
 * Header-only and QEMU-independent for standalone unit testing.
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

#ifndef XEMU_FRAMEINSPECT_SHADOWSTACK_H
#define XEMU_FRAMEINSPECT_SHADOWSTACK_H

#include "xemu-frameinspect-calltree.h"

#define FI_SS_MAX_THREADS 64
#define FI_SS_MAX_DEPTH   512
#define FI_SS_MAX_POPS    64
#define FI_INVOC_INVALID  0xFFFFFFFFu

typedef struct FIFrame {
    uint32_t node;
    uint32_t expected_ret;
    uint32_t esp_at_call;             /* ESP before the CALL pushed */
    uint32_t watch_invoc;             /* FI_INVOC_INVALID if unwatched */
} FIFrame;

typedef struct FIStack {
    uint32_t thread_key;              /* 0 = slot unused */
    uint32_t depth;
    uint32_t watch_depth;             /* frames at/below a watched frame */
    FIFrame frames[FI_SS_MAX_DEPTH];
} FIStack;

typedef struct FIPopped {
    uint32_t watch_invoc;
    bool clean;                       /* popped by its matching RET */
} FIPopped;

typedef struct FIShadow {
    FICallTree *tree;
    FIStack stacks[FI_SS_MAX_THREADS];
    uint32_t n_stacks;
    uint32_t last_idx;                /* single-hot-thread cache */
    bool threads_truncated;
} FIShadow;

static inline void fi_shadow_init(FIShadow *s, FICallTree *tree)
{
    memset(s, 0, sizeof(*s));
    s->tree = tree;
}

static inline FIStack *fi_shadow_stack(FIShadow *s, uint32_t thread_key)
{
    if (s->n_stacks && s->stacks[s->last_idx].thread_key == thread_key) {
        return &s->stacks[s->last_idx];
    }
    for (uint32_t i = 0; i < s->n_stacks; i++) {
        if (s->stacks[i].thread_key == thread_key) {
            s->last_idx = i;
            return &s->stacks[i];
        }
    }
    if (s->n_stacks >= FI_SS_MAX_THREADS) {
        s->threads_truncated = true;
        return NULL;
    }
    FIStack *st = &s->stacks[s->n_stacks];
    st->thread_key = thread_key;
    st->depth = 0;
    st->watch_depth = 0;
    s->last_idx = s->n_stacks++;
    return st;
}

static inline void fi_ss_pop_one(FIStack *st, bool clean,
                                 FIPopped *popped, uint32_t *n_popped)
{
    FIFrame *f = &st->frames[--st->depth];
    if (st->watch_depth) {
        st->watch_depth--;
    }
    if (f->watch_invoc != FI_INVOC_INVALID && popped &&
        *n_popped < FI_SS_MAX_POPS) {
        popped[*n_popped].watch_invoc = f->watch_invoc;
        popped[*n_popped].clean = clean;
        (*n_popped)++;
    }
}

static inline uint32_t fi_shadow_current(FIShadow *s, uint32_t thread_key)
{
    FIStack *st = fi_shadow_stack(s, thread_key);
    if (!st || st->depth == 0) {
        return FI_NODE_ROOT;
    }
    return st->frames[st->depth - 1].node;
}

static inline uint32_t fi_shadow_call(FIShadow *s, uint32_t thread_key,
                                      uint32_t call_site, uint32_t callee,
                                      uint32_t ret_addr, uint32_t esp)
{
    FIStack *st = fi_shadow_stack(s, thread_key);
    if (!st) {
        return FI_NODE_ROOT;
    }
    /* ESP discontinuity: frames whose ESP is below the caller's current
     * ESP were unwound without RETs (longjmp/exception). Pop them dead. */
    while (st->depth > 0 && esp > st->frames[st->depth - 1].esp_at_call) {
        fi_ss_pop_one(st, false, NULL, NULL);
    }
    uint32_t parent = st->depth ? st->frames[st->depth - 1].node
                                : FI_NODE_ROOT;
    uint32_t node = fi_calltree_intern(s->tree, parent, call_site, callee);
    if (node == FI_NODE_INVALID) {
        node = FI_NODE_ROOT;          /* tree capped: attribute to root */
    }
    if (st->depth < FI_SS_MAX_DEPTH) {
        FIFrame *f = &st->frames[st->depth++];
        f->node = node;
        f->expected_ret = ret_addr;
        f->esp_at_call = esp;
        f->watch_invoc = FI_INVOC_INVALID;
        if (st->watch_depth) {
            st->watch_depth++;
        }
    }
    return node;
}

static inline uint32_t fi_shadow_ret(FIShadow *s, uint32_t thread_key,
                                     uint32_t ret_target,
                                     FIPopped popped[], uint32_t *n_popped)
{
    *n_popped = 0;
    FIStack *st = fi_shadow_stack(s, thread_key);
    if (!st || st->depth == 0) {
        return FI_NODE_ROOT;
    }
    uint32_t limit = st->depth < FI_SS_MAX_POPS ? st->depth : FI_SS_MAX_POPS;
    for (uint32_t k = 0; k < limit; k++) {
        if (st->frames[st->depth - 1 - k].expected_ret == ret_target) {
            for (uint32_t j = 0; j < k; j++) {       /* skipped frames */
                fi_ss_pop_one(st, false, popped, n_popped);
            }
            fi_ss_pop_one(st, true, popped, n_popped);
            return st->depth ? st->frames[st->depth - 1].node : FI_NODE_ROOT;
        }
    }
    /* No match within bound: desync. Reset; report all watched frames. */
    while (st->depth > 0) {
        fi_ss_pop_one(st, false, popped, n_popped);
    }
    st->watch_depth = 0;
    return FI_NODE_ROOT;
}

static inline void fi_shadow_set_watch(FIShadow *s, uint32_t thread_key,
                                       uint32_t invoc)
{
    FIStack *st = fi_shadow_stack(s, thread_key);
    if (st && st->depth > 0) {
        st->frames[st->depth - 1].watch_invoc = invoc;
        if (!st->watch_depth) {
            st->watch_depth = 1;
        }
    }
}

static inline bool fi_shadow_in_watch(FIShadow *s, uint32_t thread_key)
{
    FIStack *st = fi_shadow_stack(s, thread_key);
    return st && st->watch_depth > 0;
}

#endif
```

- [ ] **Step 4: Run test to verify it passes**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-shadowstack.exe tests/frameinspect/test-fi-shadowstack.c && ./tests/frameinspect/test-fi-shadowstack.exe`
Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add xemu-frameinspect-shadowstack.h tests/frameinspect/test-fi-shadowstack.c
git commit -m "frameinspect: Add per-thread shadow call stacks"
```

---

### Task 4: Watch engine header

**Files:**
- Create: `xemu-frameinspect-watch.h`
- Test: `tests/frameinspect/test-fi-watch.c`

**Interfaces:**
- Consumes: `FI_INVOC_INVALID` (Task 3; redefined-guarded here so the header stands alone).
- Produces: `FIWatchEngine`; `bool fi_watch_init(FIWatchEngine *w)`; `void fi_watch_deinit(FIWatchEngine *w)`; `bool fi_watch_add(FIWatchEngine *w, uint32_t callee)`; `void fi_watch_remove(FIWatchEngine *w, uint32_t callee)`; `int fi_watch_find(const FIWatchEngine *w, uint32_t callee)` (-1 if unwatched); `uint32_t fi_watch_enter(FIWatchEngine *w, int watch_idx, uint32_t node, const uint32_t regs[8], uint32_t esp, const uint32_t stack16[16])` returning an invocation id or `FI_INVOC_INVALID` when capped; `void fi_watch_exit(FIWatchEngine *w, uint32_t invoc, bool clean, uint32_t eax)` (incomplete invocations keep `state == FI_INVOC_INCOMPLETE`, no return value); `void fi_watch_log_write(FIWatchEngine *w, uint32_t invoc, uint32_t addr, const uint8_t *old_bytes, const uint8_t *new_bytes, uint32_t len)` (coalesces contiguous same-invocation writes); `void fi_watch_count_mmio(FIWatchEngine *w, uint32_t invoc)`. States `FI_INVOC_OPEN/COMPLETE/INCOMPLETE`. Records: `w->recs[i]` = `{addr, len, old_off, new_off, invoc}` with bytes in `w->pool`; `w->invocs[id]` = `{watch_idx, order, node, regs[8], esp, stack16[16], eax_ret, state, mmio_writes}`.

- [ ] **Step 1: Write the failing test**

Create `tests/frameinspect/test-fi-watch.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xemu-frameinspect-watch.h"

int main(void)
{
    FIWatchEngine w;
    assert(fi_watch_init(&w));

    /* watch set add/find/remove */
    assert(fi_watch_add(&w, 0x8C440));
    assert(fi_watch_find(&w, 0x8C440) == 0);
    assert(fi_watch_find(&w, 0x11111) == -1);
    fi_watch_remove(&w, 0x8C440);
    assert(fi_watch_find(&w, 0x8C440) == -1);
    assert(fi_watch_add(&w, 0x8C440));

    /* enter captures regs/esp/stack and orders invocations */
    uint32_t regs[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t st16[16] = {0};
    st16[0] = 0xAABB;
    uint32_t i0 = fi_watch_enter(&w, 0, 77, regs, 0x7000, st16);
    uint32_t i1 = fi_watch_enter(&w, 0, 77, regs, 0x7000, st16);
    assert(i0 != FI_INVOC_INVALID && i1 == i0 + 1);
    assert(w.invocs[i0].order == 0 && w.invocs[i1].order == 1);
    assert(w.invocs[i0].node == 77 && w.invocs[i0].stack16[0] == 0xAABB);
    assert(w.invocs[i0].state == FI_INVOC_OPEN);

    /* clean exit records EAX; unclean marks incomplete without a value */
    fi_watch_exit(&w, i0, true, 0x1234);
    assert(w.invocs[i0].state == FI_INVOC_COMPLETE);
    assert(w.invocs[i0].eax_ret == 0x1234);
    fi_watch_exit(&w, i1, false, 0xFFFF);
    assert(w.invocs[i1].state == FI_INVOC_INCOMPLETE);

    /* write log stores old+new bytes */
    uint32_t i2 = fi_watch_enter(&w, 0, 77, regs, 0x7000, st16);
    uint8_t oldb[4] = {0, 0, 0, 0}, newb[4] = {0xEF, 0xBE, 0xAD, 0xDE};
    fi_watch_log_write(&w, i2, 0x1000, oldb, newb, 4);
    assert(w.num_recs == 1);
    assert(w.recs[0].addr == 0x1000 && w.recs[0].len == 4);
    assert(memcmp(&w.pool[w.recs[0].new_off], newb, 4) == 0);

    /* contiguous same-invocation writes coalesce (REP-style), and the
     * coalesced runs hold the full byte sequences in order */
    fi_watch_log_write(&w, i2, 0x1004, oldb, newb, 4);
    fi_watch_log_write(&w, i2, 0x1008, oldb, newb, 4);
    assert(w.num_recs == 1 && w.recs[0].len == 12);
    uint8_t exp_new[12], exp_old[12] = {0};
    for (int k = 0; k < 3; k++) {
        memcpy(&exp_new[4 * k], newb, 4);
    }
    assert(memcmp(&w.pool[w.recs[0].old_off], exp_old, 12) == 0);
    assert(memcmp(&w.pool[w.recs[0].new_off], exp_new, 12) == 0);

    /* non-contiguous or different invocation starts a new record */
    fi_watch_log_write(&w, i2, 0x2000, oldb, newb, 4);
    assert(w.num_recs == 2);
    uint32_t i3 = fi_watch_enter(&w, 0, 77, regs, 0x7000, st16);
    fi_watch_log_write(&w, i3, 0x2004, oldb, newb, 4);
    assert(w.num_recs == 3);

    /* MMIO writes are counted, not logged */
    fi_watch_count_mmio(&w, i3);
    assert(w.invocs[i3].mmio_writes == 1 && w.num_recs == 3);

    /* per-watch invocation cap */
    uint32_t got = 0;
    for (uint32_t k = 0; k < FI_WATCH_MAX_INVOC + 8; k++) {
        if (fi_watch_enter(&w, 0, 77, regs, 0x7000, st16)
            != FI_INVOC_INVALID) {
            got++;
        }
    }
    assert(got == FI_WATCH_MAX_INVOC - 4);   /* 4 used above */
    assert(w.invocs_truncated);

    fi_watch_deinit(&w);
    assert(w.recs == NULL);
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-watch.exe tests/frameinspect/test-fi-watch.c`
Expected: FAIL — `xemu-frameinspect-watch.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `xemu-frameinspect-watch.h`:

```c
/*
 * xemu frame inspector: watched-call engine
 *
 * A small set of watched callee addresses; each invocation records entry
 * registers, 16 stack dwords, and completion state, plus a write log of
 * (addr, old bytes, new bytes) for stores made inside the watched
 * subtree. Contiguous writes from one invocation coalesce into range
 * records (REP-friendly). Header-only and QEMU-independent for
 * standalone unit testing.
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

#ifndef XEMU_FRAMEINSPECT_WATCH_H
#define XEMU_FRAMEINSPECT_WATCH_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef FI_INVOC_INVALID
#define FI_INVOC_INVALID 0xFFFFFFFFu
#endif

#define FI_WATCH_MAX       16
#define FI_WATCH_MAX_INVOC 1024                /* per watch */
#define FI_WLOG_MAX_RECS   (1u << 20)
#define FI_WLOG_POOL_BYTES (24u << 20)

enum { FI_INVOC_OPEN = 0, FI_INVOC_COMPLETE = 1, FI_INVOC_INCOMPLETE = 2 };

typedef struct FIInvocation {
    uint32_t watch_idx;
    uint32_t order;
    uint32_t node;
    uint32_t regs[8];                          /* EAX..EDI, R_EAX order */
    uint32_t esp;
    uint32_t stack16[16];
    uint32_t eax_ret;
    uint32_t mmio_writes;
    uint8_t state;
} FIInvocation;

typedef struct FIWriteRec {
    uint32_t addr;
    uint32_t len;
    uint32_t old_off;                          /* offset into pool */
    uint32_t new_off;
    uint32_t invoc;
} FIWriteRec;

typedef struct FIWatchEngine {
    uint32_t watches[FI_WATCH_MAX];            /* 0 = slot unused */
    uint32_t invoc_count[FI_WATCH_MAX];
    FIInvocation *invocs;                      /* [FI_WATCH_MAX*FI_WATCH_MAX_INVOC] */
    uint32_t num_invocs;
    FIWriteRec *recs;                          /* [FI_WLOG_MAX_RECS] */
    uint32_t num_recs;
    uint8_t *pool;                             /* [FI_WLOG_POOL_BYTES] */
    uint32_t pool_used;
    bool invocs_truncated;
    bool log_truncated;
} FIWatchEngine;

static inline bool fi_watch_init(FIWatchEngine *w)
{
    memset(w, 0, sizeof(*w));
    w->invocs = calloc(FI_WATCH_MAX * FI_WATCH_MAX_INVOC,
                       sizeof(FIInvocation));
    w->recs = calloc(FI_WLOG_MAX_RECS, sizeof(FIWriteRec));
    w->pool = malloc(FI_WLOG_POOL_BYTES);
    if (!w->invocs || !w->recs || !w->pool) {
        free(w->invocs); free(w->recs); free(w->pool);
        memset(w, 0, sizeof(*w));
        return false;
    }
    return true;
}

static inline void fi_watch_deinit(FIWatchEngine *w)
{
    free(w->invocs); free(w->recs); free(w->pool);
    memset(w, 0, sizeof(*w));
    w->recs = NULL;
}

static inline int fi_watch_find(const FIWatchEngine *w, uint32_t callee)
{
    for (int i = 0; i < FI_WATCH_MAX; i++) {
        if (w->watches[i] == callee && callee != 0) {
            return i;
        }
    }
    return -1;
}

static inline bool fi_watch_add(FIWatchEngine *w, uint32_t callee)
{
    if (callee == 0 || fi_watch_find(w, callee) >= 0) {
        return callee != 0;
    }
    for (int i = 0; i < FI_WATCH_MAX; i++) {
        if (w->watches[i] == 0) {
            w->watches[i] = callee;
            w->invoc_count[i] = 0;
            return true;
        }
    }
    return false;
}

static inline void fi_watch_remove(FIWatchEngine *w, uint32_t callee)
{
    int i = fi_watch_find(w, callee);
    if (i >= 0) {
        w->watches[i] = 0;
    }
}

static inline uint32_t fi_watch_enter(FIWatchEngine *w, int watch_idx,
                                      uint32_t node, const uint32_t regs[8],
                                      uint32_t esp, const uint32_t stack16[16])
{
    if (!w->invocs || watch_idx < 0 ||
        w->invoc_count[watch_idx] >= FI_WATCH_MAX_INVOC ||
        w->num_invocs >= FI_WATCH_MAX * FI_WATCH_MAX_INVOC) {
        w->invocs_truncated = true;
        return FI_INVOC_INVALID;
    }
    uint32_t id = w->num_invocs++;
    FIInvocation *iv = &w->invocs[id];
    iv->watch_idx = (uint32_t)watch_idx;
    iv->order = w->invoc_count[watch_idx]++;
    iv->node = node;
    memcpy(iv->regs, regs, sizeof(iv->regs));
    iv->esp = esp;
    memcpy(iv->stack16, stack16, sizeof(iv->stack16));
    iv->state = FI_INVOC_OPEN;
    return id;
}

static inline void fi_watch_exit(FIWatchEngine *w, uint32_t invoc,
                                 bool clean, uint32_t eax)
{
    if (invoc >= w->num_invocs || w->invocs[invoc].state != FI_INVOC_OPEN) {
        return;
    }
    if (clean) {
        w->invocs[invoc].state = FI_INVOC_COMPLETE;
        w->invocs[invoc].eax_ret = eax;
    } else {
        w->invocs[invoc].state = FI_INVOC_INCOMPLETE;
    }
}

static inline void fi_watch_log_write(FIWatchEngine *w, uint32_t invoc,
                                      uint32_t addr, const uint8_t *old_bytes,
                                      const uint8_t *new_bytes, uint32_t len)
{
    if (!w->recs || invoc == FI_INVOC_INVALID) {
        return;
    }
    /* Coalesce with the previous record when this write extends it.
     * Invariant: the most recent record's runs sit at the pool tail as
     * [old run L][new run L] with new_off == old_off + L and
     * pool_used == new_off + L. */
    if (w->num_recs > 0) {
        FIWriteRec *p = &w->recs[w->num_recs - 1];
        if (p->invoc == invoc && p->addr + p->len == addr &&
            p->old_off + p->len == p->new_off &&
            w->pool_used == p->new_off + p->len &&
            w->pool_used + 2 * len <= FI_WLOG_POOL_BYTES) {
            uint32_t L = p->len;
            /* shift the new run up by len to open room for old bytes */
            memmove(&w->pool[p->new_off + len], &w->pool[p->new_off], L);
            memcpy(&w->pool[p->old_off + L], old_bytes, len);
            p->new_off += len;
            memcpy(&w->pool[p->new_off + L], new_bytes, len);
            p->len = L + len;
            w->pool_used = p->new_off + p->len;
            return;
        }
    }
    if (w->num_recs >= FI_WLOG_MAX_RECS ||
        w->pool_used + 2 * len > FI_WLOG_POOL_BYTES) {
        w->log_truncated = true;
        return;
    }
    FIWriteRec *r = &w->recs[w->num_recs++];
    r->addr = addr;
    r->len = len;
    r->invoc = invoc;
    r->old_off = w->pool_used;
    memcpy(&w->pool[r->old_off], old_bytes, len);
    r->new_off = w->pool_used + len;
    memcpy(&w->pool[r->new_off], new_bytes, len);
    w->pool_used += 2 * len;
}

static inline void fi_watch_count_mmio(FIWatchEngine *w, uint32_t invoc)
{
    if (invoc < w->num_invocs) {
        w->invocs[invoc].mmio_writes++;
    }
}

#endif
```

- [ ] **Step 4: Run test to verify it passes**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-watch.exe tests/frameinspect/test-fi-watch.c && ./tests/frameinspect/test-fi-watch.exe`
Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add xemu-frameinspect-watch.h tests/frameinspect/test-fi-watch.c
git commit -m "frameinspect: Add watched-call engine with coalescing write log"
```

---

### Task 5: Core module + meson wiring

**Files:**
- Create: `xemu-frameinspect.h`, `xemu-frameinspect.c`
- Modify: `meson.build:4054`

**Interfaces:**
- Consumes: all four headers (Tasks 1–4).
- Produces (the API Plans 2/3 and the TCG helpers use):

```c
extern bool xemu_frameinspect_armed;        /* translation-time guard */
extern bool xemu_frameinspect_watch_mode;   /* pre-store hook guard */
void xemu_frameinspect_arm(uint64_t ram_size);   /* idempotent */
void xemu_frameinspect_disarm(void);             /* idempotent */
bool xemu_frameinspect_is_armed(void);
/* Hot paths (vCPU thread): */
bool xemu_frameinspect_callee_watched(uint32_t callee);
uint32_t xemu_frameinspect_record_call(uint32_t thread_key, uint32_t call_site,
                                       uint32_t callee, uint32_t ret_addr,
                                       uint32_t esp, const uint32_t args[6]);
void xemu_frameinspect_record_call_watched(uint32_t thread_key,
                                           uint32_t callee,
                                           const uint32_t regs[8],
                                           uint32_t esp,
                                           const uint32_t stack16[16]);
void xemu_frameinspect_record_ret(uint32_t thread_key, uint32_t ret_target,
                                  uint32_t eax);
void xemu_frameinspect_record_store(uint32_t thread_key, uint64_t paddr,
                                    uint32_t len);
void xemu_frameinspect_record_store_watched(uint32_t thread_key,
                                            uint64_t paddr, uint32_t len,
                                            const uint8_t *old_bytes,
                                            const uint8_t *new_bytes,
                                            bool is_ram);
/* Lookup (PFIFO thread, Plan 2) and UI (Plan 3): */
uint32_t xemu_frameinspect_lookup_tag(uint64_t paddr);
bool xemu_frameinspect_watch_add(uint32_t callee);
void xemu_frameinspect_watch_remove(uint32_t callee);
char *xemu_frameinspect_status_line(void);  /* g_strdup'd, caller frees */
```

`record_call` interns the path node, attaches args, and returns the node. `record_call_watched` is called *after* `record_call` for watched callees only. Arm/disarm follow the calltrace pattern: set flag, then `queue_tb_flush(qemu_get_cpu(0))`. Store hooks are gated at translation time on `xemu_frameinspect_armed`; the pre-store hook additionally on `xemu_frameinspect_watch_mode` (set by `arm()` when any watch exists).

- [ ] **Step 1: Write the header**

Create `xemu-frameinspect.h` with the license header (GPL, xemu contributors, as in Task 1), `#ifndef XEMU_FRAMEINSPECT_H` guard, `extern "C"` wrapper (match `xemu-calltrace.h:29-31`), and exactly the declarations in the Interfaces block above plus `#include <stdbool.h>` / `#include <stdint.h>`.

- [ ] **Step 2: Write the implementation**

Create `xemu-frameinspect.c`:

```c
/*
 * xemu frame inspector: capture core (Plan 1: guest-side instrumentation)
 *
 * Owns the shadow call tree, per-thread shadow stacks, RAM-wide writer
 * tag map, and watched-call engine, and exposes the record entry points
 * called by the TCG helpers. See
 * docs/superpowers/specs/2026-07-16-frame-inspector-design.md.
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
#include "xemu-frameinspect.h"
#include "xemu-frameinspect-calltree.h"
#include "xemu-frameinspect-tagmap.h"
#include "xemu-frameinspect-shadowstack.h"
#include "xemu-frameinspect-watch.h"

bool xemu_frameinspect_armed;
bool xemu_frameinspect_watch_mode;

static FICallTree fi_tree;
static FITagMap fi_tags;
static FIShadow fi_shadow;
static FIWatchEngine fi_watch;
static bool fi_alive;                 /* structures allocated */
static uint64_t fi_stores_tagged;

/* Watch addresses persist across captures; the engine is rebuilt on
 * every arm and seeded from this list. */
static uint32_t fi_watch_addrs[FI_WATCH_MAX];

void xemu_frameinspect_arm(uint64_t ram_size)
{
    if (xemu_frameinspect_armed) {
        return;
    }
    if (fi_alive) {                   /* previous capture: drop it */
        fi_calltree_free(&fi_tree);
        fi_tagmap_free(&fi_tags);
        fi_watch_deinit(&fi_watch);
        fi_alive = false;
    }
    if (!fi_calltree_init(&fi_tree) || !fi_tagmap_init(&fi_tags, ram_size) ||
        !fi_watch_init(&fi_watch)) {
        fi_calltree_free(&fi_tree);
        fi_tagmap_free(&fi_tags);
        fi_watch_deinit(&fi_watch);
        return;                       /* allocation failed; stay disarmed */
    }
    memcpy(fi_watch.watches, fi_watch_addrs, sizeof(fi_watch_addrs));
    fi_shadow_init(&fi_shadow, &fi_tree);
    fi_stores_tagged = 0;
    fi_alive = true;
    bool any_watch = false;
    for (int i = 0; i < FI_WATCH_MAX; i++) {
        any_watch |= fi_watch_addrs[i] != 0;
    }
    xemu_frameinspect_watch_mode = any_watch;
    /* Arm before flushing so retranslated code is instrumented
     * (same pattern as xemu-calltrace.c:174-176). */
    xemu_frameinspect_armed = true;
    queue_tb_flush(qemu_get_cpu(0));
}

void xemu_frameinspect_disarm(void)
{
    if (!xemu_frameinspect_armed) {
        return;
    }
    xemu_frameinspect_armed = false;
    xemu_frameinspect_watch_mode = false;
    /* Captured data intentionally retained until next arm. */
    queue_tb_flush(qemu_get_cpu(0));
}

bool xemu_frameinspect_is_armed(void)
{
    return xemu_frameinspect_armed;
}

bool xemu_frameinspect_callee_watched(uint32_t callee)
{
    return fi_alive && fi_watch_find(&fi_watch, callee) >= 0;
}

uint32_t xemu_frameinspect_record_call(uint32_t thread_key, uint32_t call_site,
                                       uint32_t callee, uint32_t ret_addr,
                                       uint32_t esp, const uint32_t args[6])
{
    if (!fi_alive) {
        return FI_NODE_ROOT;
    }
    uint32_t node = fi_shadow_call(&fi_shadow, thread_key, call_site, callee,
                                   ret_addr, esp);
    fi_calltree_add_args(&fi_tree, node, args);
    return node;
}

void xemu_frameinspect_record_call_watched(uint32_t thread_key,
                                           uint32_t callee,
                                           const uint32_t regs[8],
                                           uint32_t esp,
                                           const uint32_t stack16[16])
{
    if (!fi_alive) {
        return;
    }
    int idx = fi_watch_find(&fi_watch, callee);
    if (idx < 0) {
        return;
    }
    uint32_t node = fi_shadow_current(&fi_shadow, thread_key);
    uint32_t invoc = fi_watch_enter(&fi_watch, idx, node, regs, esp, stack16);
    if (invoc != FI_INVOC_INVALID) {
        fi_shadow_set_watch(&fi_shadow, thread_key, invoc);
    }
}

void xemu_frameinspect_record_ret(uint32_t thread_key, uint32_t ret_target,
                                  uint32_t eax)
{
    if (!fi_alive) {
        return;
    }
    FIPopped popped[FI_SS_MAX_POPS];
    uint32_t n = 0;
    fi_shadow_ret(&fi_shadow, thread_key, ret_target, popped, &n);
    for (uint32_t i = 0; i < n; i++) {
        fi_watch_exit(&fi_watch, popped[i].watch_invoc, popped[i].clean, eax);
    }
}

void xemu_frameinspect_record_store(uint32_t thread_key, uint64_t paddr,
                                    uint32_t len)
{
    if (!fi_alive) {
        return;
    }
    uint32_t node = fi_shadow_current(&fi_shadow, thread_key);
    fi_tagmap_tag(&fi_tags, paddr, len, node);
    fi_stores_tagged++;
}

void xemu_frameinspect_record_store_watched(uint32_t thread_key,
                                            uint64_t paddr, uint32_t len,
                                            const uint8_t *old_bytes,
                                            const uint8_t *new_bytes,
                                            bool is_ram)
{
    if (!fi_alive) {
        return;
    }
    if (is_ram) {
        xemu_frameinspect_record_store(thread_key, paddr, len);
    }
    if (!fi_shadow_in_watch(&fi_shadow, thread_key)) {
        return;
    }
    FIStack *st = fi_shadow_stack(&fi_shadow, thread_key);
    uint32_t invoc = FI_INVOC_INVALID;
    for (uint32_t d = st ? st->depth : 0; d > 0; d--) {
        if (st->frames[d - 1].watch_invoc != FI_INVOC_INVALID) {
            invoc = st->frames[d - 1].watch_invoc;
            break;                    /* innermost watched frame */
        }
    }
    if (invoc == FI_INVOC_INVALID) {
        return;
    }
    if (is_ram) {
        fi_watch_log_write(&fi_watch, invoc, (uint32_t)paddr, old_bytes,
                           new_bytes, len);
    } else {
        fi_watch_count_mmio(&fi_watch, invoc);
    }
}

uint32_t xemu_frameinspect_lookup_tag(uint64_t paddr)
{
    return fi_alive ? fi_tagmap_lookup(&fi_tags, paddr) : 0;
}

bool xemu_frameinspect_watch_add(uint32_t callee)
{
    if (callee == 0) {
        return false;
    }
    for (int i = 0; i < FI_WATCH_MAX; i++) {
        if (fi_watch_addrs[i] == callee) {
            return true;
        }
    }
    for (int i = 0; i < FI_WATCH_MAX; i++) {
        if (fi_watch_addrs[i] == 0) {
            fi_watch_addrs[i] = callee;
            if (fi_alive) {
                fi_watch_add(&fi_watch, callee);
            }
            return true;
        }
    }
    return false;                     /* all 16 slots in use */
}

void xemu_frameinspect_watch_remove(uint32_t callee)
{
    for (int i = 0; i < FI_WATCH_MAX; i++) {
        if (fi_watch_addrs[i] == callee) {
            fi_watch_addrs[i] = 0;
        }
    }
    if (fi_alive) {
        fi_watch_remove(&fi_watch, callee);
    }
}

char *xemu_frameinspect_status_line(void)
{
    if (!fi_alive) {
        return g_strdup("Frame inspector: no capture");
    }
    return g_strdup_printf(
        "FI: %u nodes, %u argsets, %u threads, %" PRIu64 " stores, "
        "%u invocs, %u writes%s",
        fi_tree.num_nodes, fi_tree.num_argsets, fi_shadow.n_stacks,
        fi_stores_tagged, fi_watch.num_invocs, fi_watch.num_recs,
        (fi_tree.nodes_truncated || fi_watch.invocs_truncated ||
         fi_watch.log_truncated) ? " [TRUNCATED]" : "");
}
```

Note for implementer: `fi_shadow_stack` is used directly here; it is `static inline` in the header, which is fine since this file includes it. `queue_tb_flush` comes from `exec/tb-flush.h` — the include list above matches `xemu-calltrace.c:20-25` exactly for this reason.

- [ ] **Step 3: Wire into meson**

Modify `meson.build:4054`:

```meson
specific_ss.add(files('xemu-xbe.c', 'xemu-calltrace.c', 'xemu-frameinspect.c', 'xemu-version.c'))
```

- [ ] **Step 4: Build**

Run (MSYS2 UCRT64 shell): `ninja -C build`
Expected: builds clean (no warnings introduced in the new file).

- [ ] **Step 5: Commit**

```bash
git add xemu-frameinspect.h xemu-frameinspect.c meson.build
git commit -m "frameinspect: Add capture core module (arm/disarm, record paths)"
```

---

### Task 6: TCG CALL/RET hooks

**Files:**
- Modify: `target/i386/helper.h` (after line 238)
- Modify: `target/i386/tcg/misc_helper.c` (after line 180)
- Modify: `target/i386/tcg/emit.c.inc` (`gen_CALL` ~1605, `gen_CALL_m` ~1614, `gen_CALLF` ~1623, `gen_CALLF_m` ~1631, `gen_RET` ~3637)

**Interfaces:**
- Consumes: `xemu_frameinspect_armed`, `xemu_frameinspect_record_call`, `xemu_frameinspect_record_call_watched`, `xemu_frameinspect_callee_watched`, `xemu_frameinspect_record_ret` (Task 5).
- Produces: helpers `xemu_fi_call(env, call_site, callee)` and `xemu_fi_ret(env, ret_target)`; translation-time emission in all four CALL decoders and `gen_RET`; per-vCPU cached thread key `fi_thread_key(env)` reused by Task 7.

- [ ] **Step 1: Declare the helpers**

In `target/i386/helper.h`, after line 238 (the `xemu_calltrace_data` line):

```c
DEF_HELPER_FLAGS_3(xemu_fi_call, TCG_CALL_NO_WG, void, env, tl, tl)
DEF_HELPER_FLAGS_2(xemu_fi_ret, TCG_CALL_NO_WG, void, env, tl)
```

- [ ] **Step 2: Implement the helpers**

In `target/i386/tcg/misc_helper.c`, after line 180 (after `HELPER(xemu_calltrace_data)`), reusing the existing `ct_safe_ldl` (line 157):

```c
/* xemu frame inspector (see xemu-frameinspect.c) */
#include "../../../xemu-frameinspect.h"

/*
 * Guest thread key: current KTHREAD pointer at KPCR.PrcbData.CurrentThread
 * = fs:[0x28] (fixed Xbox kernel ABI). Read non-faulting and cached;
 * refreshed at every CALL/RET, which brackets thread switches closely
 * enough for best-effort attribution (stores between a switch and the
 * next CALL/RET may attribute to the previous thread's path).
 */
static uint32_t fi_cached_thread_key;

static uint32_t fi_thread_key(CPUX86State *env)
{
    uint32_t key = ct_safe_ldl(env, (uint32_t)env->segs[R_FS].base + 0x28);
    if (key != 0) {
        fi_cached_thread_key = key;
    }
    return fi_cached_thread_key;
}

/* Used by the store helpers (Tasks 7/8), which live in this same file —
 * keep it static so no prototype is needed. */
static uint32_t fi_thread_key_cached(void)
{
    return fi_cached_thread_key;
}

void HELPER(xemu_fi_call)(CPUX86State *env, target_ulong call_site,
                          target_ulong callee)
{
    uint32_t key = fi_thread_key(env);
    uint32_t esp = (uint32_t)env->regs[R_ESP];
    uint32_t args[6];
    args[0] = (uint32_t)env->regs[R_ECX];
    args[1] = (uint32_t)env->regs[R_EDX];
    for (int k = 0; k < 4; k++) {
        args[2 + k] = ct_safe_ldl(env, esp + 4u * (uint32_t)k);
    }
    /* call_site holds next-EIP: the address the CALL pushes. It both
     * identifies the call site and is the RET-match key (see the design
     * note under Step 3). */
    uint32_t ret_addr = (uint32_t)call_site;
    xemu_frameinspect_record_call(key, ret_addr, (uint32_t)callee,
                                  ret_addr, esp, args);
    if (xemu_frameinspect_callee_watched((uint32_t)callee)) {
        uint32_t regs[8];
        for (int r = 0; r < 8; r++) {
            regs[r] = (uint32_t)env->regs[r];
        }
        uint32_t stack16[16];
        for (int k = 0; k < 16; k++) {
            stack16[k] = ct_safe_ldl(env, esp + 4u * (uint32_t)k);
        }
        xemu_frameinspect_record_call_watched(key, (uint32_t)callee, regs,
                                              esp, stack16);
    }
}

void HELPER(xemu_fi_ret)(CPUX86State *env, target_ulong ret_target)
{
    uint32_t key = fi_thread_key(env);
    xemu_frameinspect_record_ret(key, (uint32_t)ret_target,
                                 (uint32_t)env->regs[R_EAX]);
}
```

Design note: we pass **next-EIP** (the pushed return address) as the `call_site` argument — it both identifies the call site (uniquely: site = next_eip − insn length) and serves directly as the RET-match key, avoiding a third scalar argument. The calltrace helpers pass `eip_cur_tl`; the frame inspector's gen hooks pass `eip_next_tl` instead (Step 3).

- [ ] **Step 3: Emit hooks in the CALL decoders**

In `target/i386/tcg/emit.c.inc`, immediately after the existing calltrace block (line 1591), add:

```c
extern bool xemu_frameinspect_armed;

static void gen_xemu_fi_call(DisasContext *s, TCGv callee)
{
    gen_helper_xemu_fi_call(tcg_env, eip_next_tl(s), callee);
}

static void gen_xemu_fi_call_direct(DisasContext *s, X86DecodedInsn *decode)
{
    /* Same destination arithmetic as gen_xemu_calltrace_direct(). */
    target_ulong dest = s->pc + decode->immediate - s->cs_base;

    if (s->dflag == MO_16) {
        dest &= 0xffff;
    }
    gen_xemu_fi_call(s, tcg_constant_tl(dest));
}
```

Then extend each CALL decoder (pattern identical to the calltrace guards at lines 1607, 1616, 1625, 1638):

```c
static void gen_CALL(DisasContext *s, X86DecodedInsn *decode)
{
    if (xemu_calltrace_armed) {
        gen_xemu_calltrace_direct(s, decode);
    }
    if (xemu_frameinspect_armed) {
        gen_xemu_fi_call_direct(s, decode);
    }
    gen_push_v(s, eip_next_tl(s));
    gen_JMP(s, decode);
}
```

and equivalently in `gen_CALL_m`, `gen_CALLF`, `gen_CALLF_m` (each adds `if (xemu_frameinspect_armed) { gen_xemu_fi_call(s, s->T0); }` beside the existing calltrace call, before the push/far-call).

- [ ] **Step 4: Emit the RET hook**

In `gen_RET` (emit.c.inc:3637), after `MemOp ot = gen_pop_T0(s);` (s->T0 now holds the return target) and before `gen_op_jmp_v`:

```c
static void gen_RET(DisasContext *s, X86DecodedInsn *decode)
{
    int16_t adjust = decode->e.op1 == X86_TYPE_I ? decode->immediate : 0;

    MemOp ot = gen_pop_T0(s);
    if (xemu_frameinspect_armed) {
        gen_helper_xemu_fi_ret(tcg_env, s->T0);
    }
    gen_stack_update(s, adjust + (1 << ot));
    gen_op_jmp_v(s, s->T0);
    gen_bnd_jmp(s);
    s->base.is_jmp = DISAS_JUMP;
}
```

`gen_RETF` is left unhooked: the Xbox flat model does not use far returns; a far return while armed degrades to a future RET mismatch → unknown root (allowed by the spec's "missing, never wrong").

- [ ] **Step 5: Build**

Run (MSYS2 UCRT64 shell): `ninja -C build`
Expected: clean build. If `eip_next_tl` is not visible at the insertion point, the calltrace block at line 1575 already uses `eip_cur_tl` — both are defined earlier in translate.c; no include changes needed.

- [ ] **Step 6: Commit**

```bash
git add target/i386/helper.h target/i386/tcg/misc_helper.c target/i386/tcg/emit.c.inc
git commit -m "frameinspect: Instrument CALL/RET with shadow-stack helpers"
```

---

### Task 7: Store hook (tag path)

**Files:**
- Modify: `target/i386/helper.h` (with Task 6's entries)
- Modify: `target/i386/tcg/misc_helper.c` (after Task 6's helpers)
- Modify: `target/i386/tcg/translate.c:628` (`gen_op_st_v`)

**Interfaces:**
- Consumes: `xemu_frameinspect_record_store`, `fi_thread_key_cached()` (Task 6), `xemu_frameinspect_armed`.
- Produces: helper `xemu_fi_store_post(env, addr, size)`; every integer store path through `gen_op_st_v` (mov-to-memory, push, movs/stos iterations, pop-to-memory writeback) tags the tag map post-store.

- [ ] **Step 1: Declare the helper**

In `target/i386/helper.h`, after the Task 6 entries:

```c
DEF_HELPER_FLAGS_3(xemu_fi_store_post, TCG_CALL_NO_WG, void, env, tl, i32)
```

- [ ] **Step 2: Implement the helper**

In `target/i386/tcg/misc_helper.c`, after the Task 6 helpers:

```c
/*
 * Post-store tagging: runs only after the store retired (the helper call
 * is emitted after the qemu_st op, so a faulting store unwinds first and
 * never reaches it) — tags are published only for successful stores.
 * Linear→physical goes through a 1-entry page cache; misses walk the
 * page tables non-faulting.
 */
static struct { uint32_t page; uint64_t phys_page; bool valid; } fi_tlb1;

static bool fi_lin_to_phys(CPUX86State *env, uint32_t lin, uint64_t *phys)
{
    uint32_t page = lin & TARGET_PAGE_MASK;
    if (!fi_tlb1.valid || fi_tlb1.page != page) {
        hwaddr p = cpu_get_phys_page_debug(env_cpu(env), page);
        if (p == -1) {
            return false;
        }
        fi_tlb1.page = page;
        fi_tlb1.phys_page = p;
        fi_tlb1.valid = true;
    }
    *phys = fi_tlb1.phys_page | (lin & ~TARGET_PAGE_MASK);
    return true;
}

void HELPER(xemu_fi_store_post)(CPUX86State *env, target_ulong addr,
                                uint32_t size)
{
    uint32_t lin = (uint32_t)addr;
    uint32_t done = 0;
    while (done < size) {             /* split page-crossing stores */
        uint32_t chunk = TARGET_PAGE_SIZE - ((lin + done) & ~TARGET_PAGE_MASK);
        if (chunk > size - done) {
            chunk = size - done;
        }
        uint64_t phys;
        if (fi_lin_to_phys(env, lin + done, &phys)) {
            xemu_frameinspect_record_store(fi_thread_key_cached(), phys,
                                           chunk);
        }
        done += chunk;
    }
}
```

Note: `chunk` computation — `(lin + done) & ~TARGET_PAGE_MASK` is the offset within the page; `TARGET_PAGE_SIZE - offset` is the room left. Uses the *cached* thread key (no fs read per store; refreshed at CALL/RET per Task 6).

- [ ] **Step 3: Emit in gen_op_st_v**

Modify `target/i386/tcg/translate.c:628`:

```c
extern bool xemu_frameinspect_armed;

static inline void gen_op_st_v(DisasContext *s, int idx, TCGv t0, TCGv a0)
{
    tcg_gen_qemu_st_tl(t0, a0, s->mem_index, idx | MO_LE);
    /* xemu frame inspector: tag the store's destination post-retire.
     * Translation-time guard; arming/disarming flushes the TB cache. */
    if (xemu_frameinspect_armed) {
        gen_helper_xemu_fi_store_post(tcg_env, a0,
                                      tcg_constant_i32(1 << (idx & MO_SIZE)));
    }
}
```

`a0` is a TCG temp still live after the store; `idx` is the MemOp, so `1 << (idx & MO_SIZE)` is the store width in bytes.

- [ ] **Step 4: Build and smoke-test**

Run (MSYS2 UCRT64 shell): `ninja -C build`
Expected: clean build. (Runtime smoke happens in Task 9 with the hotkey.)

- [ ] **Step 5: Commit**

```bash
git add target/i386/helper.h target/i386/tcg/misc_helper.c target/i386/tcg/translate.c
git commit -m "frameinspect: Tag guest stores post-retire via gen_op_st_v hook"
```

---

### Task 8: Watch-mode pre/post store pair

**Files:**
- Modify: `target/i386/helper.h`
- Modify: `target/i386/tcg/misc_helper.c`
- Modify: `target/i386/tcg/translate.c:628` (extend Task 7's hook)

**Interfaces:**
- Consumes: `xemu_frameinspect_watch_mode`, `xemu_frameinspect_record_store_watched` (Task 5), `fi_lin_to_phys` (Task 7).
- Produces: helpers `xemu_fi_store_pre(env, addr, size)` (stashes old bytes) and the watch-mode branch of `xemu_fi_store_post` (reads new bytes, logs old→new). Only emitted when `xemu_frameinspect_watch_mode` was true at translation time.

- [ ] **Step 1: Declare the helper**

In `target/i386/helper.h`:

```c
DEF_HELPER_FLAGS_3(xemu_fi_store_pre, TCG_CALL_NO_WG, void, env, tl, i32)
```

- [ ] **Step 2: Implement pre-stash and post-log**

In `target/i386/tcg/misc_helper.c`, extend the frame-inspector section:

```c
/* Watch mode: the pre-helper stashes the old bytes (single vCPU, so one
 * scratch slot suffices); the post-helper reads the new bytes back from
 * memory and logs old->new. Nothing is published if the store faults
 * between the two (the post-helper never runs; the stash is simply
 * overwritten by the next store). */
static struct {
    uint64_t phys;
    uint32_t size;
    bool is_ram;
    uint8_t old_bytes[8];
} fi_store_stash;

void HELPER(xemu_fi_store_pre)(CPUX86State *env, target_ulong addr,
                               uint32_t size)
{
    uint64_t phys;
    fi_store_stash.size = size;
    fi_store_stash.is_ram = fi_lin_to_phys(env, (uint32_t)addr, &phys);
    if (fi_store_stash.is_ram) {
        fi_store_stash.phys = phys;
        cpu_physical_memory_read(phys, fi_store_stash.old_bytes, size);
    }
}

void HELPER(xemu_fi_store_post_watch)(CPUX86State *env, target_ulong addr,
                                      uint32_t size)
{
    uint8_t new_bytes[8];
    if (fi_store_stash.is_ram && fi_store_stash.size == size) {
        cpu_physical_memory_read(fi_store_stash.phys, new_bytes, size);
        xemu_frameinspect_record_store_watched(fi_thread_key_cached(),
                                               fi_store_stash.phys, size,
                                               fi_store_stash.old_bytes,
                                               new_bytes, true);
    } else {
        xemu_frameinspect_record_store_watched(fi_thread_key_cached(),
                                               (uint32_t)addr, size,
                                               NULL, NULL, false);
    }
}
```

And declare it in `target/i386/helper.h`:

```c
DEF_HELPER_FLAGS_3(xemu_fi_store_post_watch, TCG_CALL_NO_WG, void, env, tl, i32)
```

Simplification note: page-crossing watched stores log via the RAM path of the first page only (the stash holds one phys run); the tag map still covers both pages through `xemu_fi_store_post`'s split. This matches "best-effort, never wrong" — a cross-page watched write logs its first-page bytes and the second page is tagged but not value-logged.

- [ ] **Step 3: Extend the gen hook**

In `translate.c`, final form of `gen_op_st_v`:

```c
extern bool xemu_frameinspect_armed;
extern bool xemu_frameinspect_watch_mode;

static inline void gen_op_st_v(DisasContext *s, int idx, TCGv t0, TCGv a0)
{
    /* xemu frame inspector: watch mode brackets the store to capture
     * old->new bytes; plain armed mode tags post-retire only. */
    if (xemu_frameinspect_armed && xemu_frameinspect_watch_mode) {
        gen_helper_xemu_fi_store_pre(tcg_env, a0,
                                     tcg_constant_i32(1 << (idx & MO_SIZE)));
    }
    tcg_gen_qemu_st_tl(t0, a0, s->mem_index, idx | MO_LE);
    if (xemu_frameinspect_armed) {
        if (xemu_frameinspect_watch_mode) {
            gen_helper_xemu_fi_store_post_watch(
                tcg_env, a0, tcg_constant_i32(1 << (idx & MO_SIZE)));
        } else {
            gen_helper_xemu_fi_store_post(
                tcg_env, a0, tcg_constant_i32(1 << (idx & MO_SIZE)));
        }
    }
}
```

Note: `xemu_fi_store_post_watch` must also tag the map — it calls `record_store_watched(..., is_ram=true)` which tags via `record_store` (see Task 5 implementation), so the plain post helper is correctly skipped in watch mode.

- [ ] **Step 4: Build**

Run (MSYS2 UCRT64 shell): `ninja -C build`
Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add target/i386/helper.h target/i386/tcg/misc_helper.c target/i386/tcg/translate.c
git commit -m "frameinspect: Watch-mode store bracketing captures old/new bytes"
```

---

### Task 9: Hotkey, toast, and end-to-end smoke test

**Files:**
- Modify: `ui/xemu.c` (include block ~line 57; scancode switch ~line 408)

**Interfaces:**
- Consumes: `xemu_frameinspect_arm/disarm/is_armed/status_line` (Task 5), `xemu_queue_notification` (existing), `xemu_get_xbe_info` (existing).
- Produces: Ctrl+Alt+I toggle — the user-facing arm entry point Plan 2 replaces with flip-boundary sequencing (the hotkey stays; only what "disarm" triggers changes).

- [ ] **Step 1: Add the include**

In `ui/xemu.c` after line 57 (`#include "../xemu-calltrace.h"`):

```c
#include "../xemu-frameinspect.h"
```

- [ ] **Step 2: Add the hotkey case**

In the Ctrl+Alt scancode switch (same block as `SDL_SCANCODE_T` at line 412), add:

```c
        case SDL_SCANCODE_I: {
            gui_keysym = 1;
            if (xemu_frameinspect_is_armed()) {
                xemu_frameinspect_disarm();
                char *msg = xemu_frameinspect_status_line();
                xemu_queue_notification(msg);
                g_free(msg);
            } else if (xemu_get_xbe_info() != NULL) {
                MachineState *ms = MACHINE(qdev_get_machine());
                xemu_frameinspect_arm(ms->ram_size);
                xemu_queue_notification(
                    xemu_frameinspect_is_armed()
                        ? "Frame inspector: armed (recording)"
                        : "Frame inspector: arm failed (allocation)");
            } else {
                xemu_queue_notification("Load a game before inspecting");
            }
            break;
        }
```

Add `#include "hw/boards.h"` beside the other includes if `MachineState`/`MACHINE` are unresolved.

- [ ] **Step 3: Build**

Run (MSYS2 UCRT64 shell): `ninja -C build`
Expected: clean build.

- [ ] **Step 4: End-to-end smoke test (manual)**

1. Launch xemu with any game.
2. Press Ctrl+Alt+I → toast "Frame inspector: armed (recording)". The game keeps running (expect a visible slowdown — that's the store instrumentation working).
3. Wait ~2 seconds, press Ctrl+Alt+I again → toast like `FI: 1234 nodes, 567 argsets, 3 threads, 8912345 stores, 0 invocs, 0 writes`.
4. Verify: nodes > 100 (call paths interned), threads ≥ 1 (KTHREAD keys seen), stores > 100000 (tag path live), no crash, game returns to full speed after disarm.
5. Arm/disarm five times in a row → stable, no leaks visible in Task Manager beyond the expected ~40 MB tree + 64–128 MB tag map while a capture is held.

Record the observed numbers in the commit message.

- [ ] **Step 5: Commit**

```bash
git add ui/xemu.c
git commit -m "frameinspect: Ctrl+Alt+I toggles instrumentation with stats toast"
```

---

## Post-plan verification checklist

- All four standalone tests print `PASS`.
- `ninja -C build` clean from scratch.
- Smoke test numbers recorded (Task 9 step 4).
- Interfaces produced here and consumed by Plan 2 (NV2A capture engine): `xemu_frameinspect_lookup_tag`, `xemu_frameinspect_arm/disarm`, `xemu_frameinspect_armed`.
- Interfaces consumed by Plan 3 (inspector UI): `xemu_frameinspect_watch_add/remove`, `xemu_frameinspect_status_line`, the calltree/watch structures.

## Follow-on plans (not in this document)

- **Plan 2 — NV2A capture engine:** flip-boundary state machine, PFIFO method logging with `CommandOrigin`, surface generations + color history + resource snapshots, scanout event, immutable capture publish, async VM pause. Also: **SSE store coverage** — the spec's "common SSE stores" go through emission paths other than `gen_op_st_v` (e.g. the SSE store gen functions in emit.c.inc); Plan 2 verifies those sites and adds the same post-store hook. Until then SSE-written data reads `unattributed`, which the spec permits (missing, never wrong).
- **Plan 3 — Inspector UI:** ImGui overlay (hover/highlight, Origin/Methods/State/Resources/Pixels tabs, timelines, address lookup, watch panel), re-capture flow.

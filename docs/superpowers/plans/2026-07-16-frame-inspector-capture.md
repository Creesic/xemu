# Frame Inspector Capture Engine — Plan 2A (data model + lifecycle) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the in-RAM capture data model (surface generations, per-generation colour-history codec, content-hashed resource pool, event log + budget) as standalone unit-tested headers, plus the capture lifecycle skeleton that arms a one-shot frame capture, records a structurally complete event stream at the flip boundary, publishes an immutable capture object, and pauses the VM — with pixel/resource/origin *content* deferred to Plan 2B.

**Architecture:** Four header-only, QEMU-independent data structures (unit-tested with gcc, exactly like Plan 1's headers) own all capture storage. A capture module (`xemu-frameinspect-capture.c/.h`) bundles them into a live `FICapture`, drives the `IDLE→ARMED→CAPTURING→DONE` state machine off the NV097_FLIP_STALL boundary on the pfifo thread, publishes an immutable pointer for the UI, and schedules a bottom half to `vm_stop()` under the BQL. Renderer-ops hooks in the GL backend record the event skeleton (batch/clear/blit boundaries) and intern the current render-surface generation, but read no pixels yet.

**Tech Stack:** C11 (GCC builtins for atomics), QEMU NV2A pgraph GL renderer, QEMU aio bottom halves + `vm_stop`.

**Spec:** `docs/superpowers/specs/2026-07-16-frame-inspector-design.md` (the "NV2A capture engine" scope; this plan is the data-model + lifecycle half, Plan 2B fills in live pixels/origins/resources/scanout).

**Builds on:** Plan 1 (`docs/superpowers/plans/2026-07-16-frame-inspector-core.md`) — the guest instrumentation, `xemu-frameinspect.{c,h}` arm/disarm, tag map, shadow stack. This plan reuses `xemu_frameinspect_arm/disarm` for the capture's lead-in window.

## Global Constraints

- Header-only `xemu-frameinspect-*.h` files stay **QEMU-independent** (standalone-testable with `/c/msys64/ucrt64/bin/gcc.exe`; no glib/QEMU headers). Plain `gcc` is FPC's 2.95 — never use it. Emulator build: `ninja -C build` via MSYS2 UCRT64 (memory `xemu-windows-build-env`); the wrapper script is `.../scratchpad/build-xemu.sh` — run `/c/msys64/usr/bin/bash.exe -l "<script>"`.
- The capture object handed to the UI is **immutable and self-contained**: it depends on nothing live (no GL objects, no guest RAM). Publication is an acquire/release pointer handoff; the UI only ever reads a published capture.
- The VM pause is a **UX effect scheduled after** the immutable publish — never the synchronization mechanism. `vm_stop` requires the BQL; the flip hook runs on the pfifo thread **without** the BQL, so the pause is deferred via `aio_bh_schedule_oneshot(qemu_get_aio_context(), cb, opaque)` (pattern: `ui/console.c:160`; `vm_stop(RUN_STATE_PAUSED)` pattern: `ui/xui/actions.cc:73`).
- "Missing, never wrong" holds here too: an unreadable/uncapturable element is recorded as absent, never fabricated.
- All growable stores have hard caps and a shared global byte budget (default 1 GiB); hitting any cap sets a per-store truncation flag surfaced in the capture — never a silent drop, never a wholesale abort.
- License headers: GPL-2.0-or-later, "Copyright (C) 2026 xemu contributors" (match `xemu-frameinspect.h`).
- Naming: `fi_`/`FI` in standalone headers; `xemu_frameinspect_capture_` for the QEMU-facing capture API. Commit prefix: `frameinspect:`.
- GL renderer only (v1). Non-GL backends are out of scope.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `xemu-frameinspect-surfaces.h` | create | Surface-generation identity interning, rebind/alias tracking |
| `xemu-frameinspect-colorhist.h` | create | Per-generation colour-change history: keyframes + RLE deltas, reconstruct, per-pixel history |
| `xemu-frameinspect-resources.h` | create | Content-hash-deduplicated resource blob pool |
| `xemu-frameinspect-capture.h` / `.c` | create | Live `FICapture`, event log + budget, arm/flip state machine, immutable publish, pause BH |
| `xemu-frameinspect-eventlog.h` | create | Event log + shared byte budget (standalone) |
| `hw/xbox/nv2a/pgraph/pgraph.c` | modify (`NV097_FLIP_STALL` ~905) | Call the capture flip hook after `ops.flip_stall` |
| `hw/xbox/nv2a/pgraph/gl/renderer.c` | modify (ops table ~185, flip_stall ~102) | (context for hook wiring) |
| `hw/xbox/nv2a/pgraph/gl/draw.c` | modify (`draw_begin` 134, `draw_end` 333, `clear_surface` 27) | Batch/clear capture-event hooks |
| `hw/xbox/nv2a/pgraph/gl/blit.c` | modify (`image_blit` 72) | Blit capture-event hook |
| `hw/xbox/nv2a/pgraph/gl/surface.c` | modify (`update_surface_part` 1102) | Intern current surface generation during capture |
| `ui/xemu.c` | modify (`SDL_SCANCODE_I` case, Plan 1) | Repurpose hotkey to arm a one-shot capture |
| `meson.build` | modify (line ~4054) | Add `xemu-frameinspect-capture.c` |
| `tests/frameinspect/test-fi-surfaces.c` | create | Unit test |
| `tests/frameinspect/test-fi-colorhist.c` | create | Unit test |
| `tests/frameinspect/test-fi-resources.c` | create | Unit test |
| `tests/frameinspect/test-fi-eventlog.c` | create | Unit test |

---

### Task 1: Surface-generation store header

**Files:**
- Create: `xemu-frameinspect-surfaces.h`
- Test: `tests/frameinspect/test-fi-surfaces.c`

**Interfaces:**
- Consumes: nothing (standalone).
- Produces: `FISurfaceKey { uint32_t addr, format, pitch, width, height; uint8_t swizzle, color, aa, scale; }`; `FISurfaceStore`; `bool fi_surfaces_init(FISurfaceStore *s)`; `void fi_surfaces_free(FISurfaceStore *s)`; `uint32_t fi_surfaces_intern(FISurfaceStore *s, const FISurfaceKey *k)` returning a capture-local generation id or `FI_SURFGEN_INVALID` when capped. Record fields (`s->gens[id]`): `key`, `extent` (addr + pitch*height, byte end), `generation` (per-addr rebind counter, 0-based), `prev_at_addr` (id of the previous generation at the same addr, or `FI_SURFGEN_INVALID`), `alias` (true if its VRAM byte range overlaps a different-addr generation). Constants `FI_SURFGEN_INVALID` (0xFFFFFFFF), `FI_SURF_MAX_GENS` (4096).

- [ ] **Step 1: Write the failing test**

Create `tests/frameinspect/test-fi-surfaces.c`:

```c
#include <assert.h>
#include <stdio.h>
#include "../../xemu-frameinspect-surfaces.h"

static FISurfaceKey mk(uint32_t addr, uint32_t fmt, uint32_t pitch,
                       uint32_t w, uint32_t h)
{
    FISurfaceKey k = {0};
    k.addr = addr; k.format = fmt; k.pitch = pitch;
    k.width = w; k.height = h;
    k.swizzle = 0; k.color = 1; k.aa = 0; k.scale = 1;
    return k;
}

int main(void)
{
    FISurfaceStore s;
    assert(fi_surfaces_init(&s));

    /* first intern creates generation 0 */
    FISurfaceKey a = mk(0x1000, 4, 2560, 640, 480);
    uint32_t g0 = fi_surfaces_intern(&s, &a);
    assert(g0 != FI_SURFGEN_INVALID);
    assert(s.gens[g0].generation == 0);
    assert(s.gens[g0].prev_at_addr == FI_SURFGEN_INVALID);
    assert(s.gens[g0].extent == 0x1000u + 2560u * 480u);

    /* identical key at same addr -> same generation, no growth */
    uint32_t g0b = fi_surfaces_intern(&s, &a);
    assert(g0b == g0 && s.num_gens == 1);

    /* changed format at same addr -> new generation, gen counter bumps,
     * prev link points back */
    FISurfaceKey a2 = mk(0x1000, 5, 2560, 640, 480);
    uint32_t g1 = fi_surfaces_intern(&s, &a2);
    assert(g1 != g0 && s.gens[g1].generation == 1);
    assert(s.gens[g1].prev_at_addr == g0);
    assert(s.num_gens == 2);

    /* a different addr whose byte range overlaps g1 -> flagged alias
     * (both flagged) */
    FISurfaceKey b = mk(0x1000 + 2560 * 100, 4, 2560, 640, 380);
    uint32_t gb = fi_surfaces_intern(&s, &b);
    assert(s.gens[gb].alias && s.gens[g1].alias);

    /* a disjoint addr -> not aliased */
    FISurfaceKey c = mk(0x800000, 4, 2560, 640, 480);
    uint32_t gc = fi_surfaces_intern(&s, &c);
    assert(!s.gens[gc].alias);

    /* cap: refuse with INVALID + truncated flag */
    while (s.num_gens < FI_SURF_MAX_GENS) {
        FISurfaceKey k = mk(0x2000000u + s.num_gens * 0x10000u, 4, 256, 64, 64);
        fi_surfaces_intern(&s, &k);
    }
    FISurfaceKey over = mk(0xF0000000u, 4, 256, 64, 64);
    assert(fi_surfaces_intern(&s, &over) == FI_SURFGEN_INVALID);
    assert(s.truncated);

    fi_surfaces_free(&s);
    assert(s.gens == NULL);
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-surfaces.exe tests/frameinspect/test-fi-surfaces.c`
Expected: FAIL — `xemu-frameinspect-surfaces.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `xemu-frameinspect-surfaces.h` (license header as in `xemu-frameinspect.h`, guard `XEMU_FRAMEINSPECT_SURFACES_H`), with:

```c
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_SURFGEN_INVALID 0xFFFFFFFFu
#define FI_SURF_MAX_GENS   4096u

typedef struct FISurfaceKey {
    uint32_t addr;
    uint32_t format;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t swizzle;
    uint8_t color;   /* 1 = colour surface, 0 = zeta */
    uint8_t aa;
    uint8_t scale;   /* internal scale factor at capture time */
} FISurfaceKey;

typedef struct FISurfaceGen {
    FISurfaceKey key;
    uint64_t extent;        /* addr + pitch*height (byte end, exclusive) */
    uint32_t generation;    /* per-addr rebind counter */
    uint32_t prev_at_addr;  /* previous gen id at same addr, or INVALID */
    bool alias;             /* overlaps a different-addr generation */
} FISurfaceGen;

typedef struct FISurfaceStore {
    FISurfaceGen *gens;     /* [FI_SURF_MAX_GENS] */
    uint32_t num_gens;
    bool truncated;
} FISurfaceStore;

static inline bool fi_surfaces_init(FISurfaceStore *s)
{
    memset(s, 0, sizeof(*s));
    s->gens = (FISurfaceGen *)calloc(FI_SURF_MAX_GENS, sizeof(FISurfaceGen));
    return s->gens != NULL;
}

static inline void fi_surfaces_free(FISurfaceStore *s)
{
    free(s->gens);
    memset(s, 0, sizeof(*s));
    s->gens = NULL;
}

static inline bool fi_surf_key_eq(const FISurfaceKey *a, const FISurfaceKey *b)
{
    return memcmp(a, b, sizeof(FISurfaceKey)) == 0;
}

static inline uint32_t fi_surfaces_intern(FISurfaceStore *s,
                                          const FISurfaceKey *k)
{
    /* Reuse the most recent generation at this addr if its key is identical. */
    for (uint32_t i = s->num_gens; i-- > 0; ) {
        if (s->gens[i].key.addr == k->addr) {
            if (fi_surf_key_eq(&s->gens[i].key, k)) {
                return i;
            }
            break; /* newest gen at addr differs -> this is a rebind */
        }
    }
    if (s->num_gens >= FI_SURF_MAX_GENS) {
        s->truncated = true;
        return FI_SURFGEN_INVALID;
    }
    uint32_t id = s->num_gens++;
    FISurfaceGen *g = &s->gens[id];
    memset(g, 0, sizeof(*g));
    g->key = *k;
    g->extent = (uint64_t)k->addr + (uint64_t)k->pitch * k->height;
    g->prev_at_addr = FI_SURFGEN_INVALID;
    g->generation = 0;
    for (uint32_t i = id; i-- > 0; ) {
        if (s->gens[i].key.addr == k->addr) {
            g->prev_at_addr = i;
            g->generation = s->gens[i].generation + 1;
            break;
        }
    }
    /* Alias detection: overlap in VRAM byte range with a different addr. */
    for (uint32_t i = 0; i < id; i++) {
        if (s->gens[i].key.addr == k->addr) {
            continue;
        }
        bool overlap = (uint64_t)k->addr < s->gens[i].extent &&
                       s->gens[i].key.addr < g->extent;
        if (overlap) {
            g->alias = true;
            s->gens[i].alias = true;
        }
    }
    return id;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-surfaces.exe tests/frameinspect/test-fi-surfaces.c && ./tests/frameinspect/test-fi-surfaces.exe`
Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add xemu-frameinspect-surfaces.h tests/frameinspect/test-fi-surfaces.c
git commit -m "frameinspect: Add surface-generation identity store"
```

---

### Task 2: Colour-history codec header

**Files:**
- Create: `xemu-frameinspect-colorhist.h`
- Test: `tests/frameinspect/test-fi-colorhist.c`

**Interfaces:**
- Consumes: nothing (standalone).
- Produces: `FIColorHist`; `bool fi_colorhist_init(FIColorHist *ch, uint32_t width, uint32_t height, uint32_t keyframe_interval)`; `void fi_colorhist_free(FIColorHist *ch)`; `void fi_colorhist_set_baseline(FIColorHist *ch, const uint32_t *rgba)` (w*h pixels, becomes the current image + keyframe at event index 0); `bool fi_colorhist_add_event(FIColorHist *ch, uint32_t event_id, const uint32_t *post)` (diffs the internal current image vs `post`, stores changed-pixel runs with before+after colours, updates current, snapshots a keyframe every `keyframe_interval` events; returns false when a cap is hit); `uint32_t fi_colorhist_num_events(const FIColorHist *ch)`; `void fi_colorhist_reconstruct(const FIColorHist *ch, uint32_t event_index, uint32_t *out_rgba)` (exact image after applying events 0..event_index); `int fi_colorhist_pixel_history(const FIColorHist *ch, uint32_t pixel_index, FIColorTouch *out, int max)` returning the count written. `FIColorTouch { uint32_t event_id; uint32_t before; uint32_t after; }`. Caps: `FI_CH_MAX_EVENTS` (4096), `FI_CH_MAX_KEYFRAMES` (64), `FI_CH_RUN_BYTES_CAP` (default via init arg or fixed 256 MiB — here fixed constant); truncation flag `ch->truncated`.

- [ ] **Step 1: Write the failing test**

Create `tests/frameinspect/test-fi-colorhist.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xemu-frameinspect-colorhist.h"

int main(void)
{
    /* 4x4 image, keyframe every 8 events */
    FIColorHist ch;
    assert(fi_colorhist_init(&ch, 4, 4, 8));

    uint32_t base[16];
    for (int i = 0; i < 16; i++) base[i] = 0x00000000u;
    fi_colorhist_set_baseline(&ch, base);

    /* event A (id 10): set pixels 5,6,7 to 0xAA */
    uint32_t imgA[16];
    memcpy(imgA, base, sizeof(base));
    imgA[5] = imgA[6] = imgA[7] = 0xAAAAAAAAu;
    assert(fi_colorhist_add_event(&ch, 10, imgA));

    /* event B (id 11): change pixel 6 to 0xBB */
    uint32_t imgB[16];
    memcpy(imgB, imgA, sizeof(imgA));
    imgB[6] = 0xBBBBBBBBu;
    assert(fi_colorhist_add_event(&ch, 11, imgB));

    assert(fi_colorhist_num_events(&ch) == 2);

    /* reconstruct after event 0 (A) equals imgA exactly */
    uint32_t out[16];
    fi_colorhist_reconstruct(&ch, 0, out);
    assert(memcmp(out, imgA, sizeof(out)) == 0);

    /* reconstruct after event 1 (B) equals imgB exactly */
    fi_colorhist_reconstruct(&ch, 1, out);
    assert(memcmp(out, imgB, sizeof(out)) == 0);

    /* pixel 6 history: (A: 0x00->0xAA), (B: 0xAA->0xBB) */
    FIColorTouch t[8];
    int n = fi_colorhist_pixel_history(&ch, 6, t, 8);
    assert(n == 2);
    assert(t[0].event_id == 10 && t[0].before == 0 && t[0].after == 0xAAAAAAAAu);
    assert(t[1].event_id == 11 && t[1].before == 0xAAAAAAAAu &&
           t[1].after == 0xBBBBBBBBu);

    /* pixel 5 changed only in A */
    n = fi_colorhist_pixel_history(&ch, 5, t, 8);
    assert(n == 1 && t[0].event_id == 10);

    /* pixel 0 never changed */
    n = fi_colorhist_pixel_history(&ch, 0, t, 8);
    assert(n == 0);

    /* keyframe reconstruction: push >8 events so a keyframe is taken,
     * then reconstruct an event after it and verify exactness */
    uint32_t img[16];
    memcpy(img, imgB, sizeof(imgB));
    for (int e = 0; e < 12; e++) {
        img[e % 16] = 0xC0000000u + (uint32_t)e;
        assert(fi_colorhist_add_event(&ch, 100u + (uint32_t)e, img));
    }
    uint32_t ref[16];
    memcpy(ref, img, sizeof(img)); /* img now equals state after last event */
    fi_colorhist_reconstruct(&ch, fi_colorhist_num_events(&ch) - 1, out);
    assert(memcmp(out, ref, sizeof(out)) == 0);

    fi_colorhist_free(&ch);
    assert(ch.events == NULL);
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-colorhist.exe tests/frameinspect/test-fi-colorhist.c`
Expected: FAIL — header not found.

- [ ] **Step 3: Write the header**

Create `xemu-frameinspect-colorhist.h` (license + guard `XEMU_FRAMEINSPECT_COLORHIST_H`):

```c
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_CH_MAX_EVENTS    4096u
#define FI_CH_MAX_KEYFRAMES 64u
#define FI_CH_RUN_CAP       (1u << 24)   /* max run entries total */
#define FI_CH_COLOR_CAP     (1u << 26)   /* max colour entries (before+after) */

typedef struct FIColorTouch {
    uint32_t event_id;
    uint32_t before;
    uint32_t after;
} FIColorTouch;

typedef struct FIColorRun {
    uint32_t start;      /* first pixel index in the run */
    uint32_t len;        /* contiguous changed pixels */
    uint32_t color_off;  /* offset into colours[]; before at off, after at off+len */
} FIColorRun;

typedef struct FIColorEvent {
    uint32_t event_id;
    uint32_t run_first;  /* index into runs[] */
    uint32_t run_count;
} FIColorEvent;

typedef struct FIKeyframe {
    uint32_t after_event; /* image state after this event index (or 0 = baseline) */
    uint32_t *image;      /* w*h snapshot */
} FIKeyframe;

typedef struct FIColorHist {
    uint32_t width, height, npix;
    uint32_t keyframe_interval;
    uint32_t *current;          /* running image */
    FIColorEvent *events;       /* [FI_CH_MAX_EVENTS] */
    uint32_t num_events;
    FIColorRun *runs;           /* growable up to FI_CH_RUN_CAP */
    uint32_t num_runs, cap_runs;
    uint32_t *colors;           /* growable up to FI_CH_COLOR_CAP */
    uint32_t num_colors, cap_colors;
    FIKeyframe keyframes[FI_CH_MAX_KEYFRAMES];
    uint32_t num_keyframes;
    bool has_baseline;
    bool truncated;
} FIColorHist;

static inline bool fi_colorhist_init(FIColorHist *ch, uint32_t width,
                                     uint32_t height, uint32_t keyframe_interval)
{
    memset(ch, 0, sizeof(*ch));
    ch->width = width; ch->height = height; ch->npix = width * height;
    ch->keyframe_interval = keyframe_interval ? keyframe_interval : 16;
    ch->current = (uint32_t *)calloc(ch->npix, sizeof(uint32_t));
    ch->events = (FIColorEvent *)calloc(FI_CH_MAX_EVENTS, sizeof(FIColorEvent));
    ch->cap_runs = 4096; ch->cap_colors = 4096;
    ch->runs = (FIColorRun *)malloc(ch->cap_runs * sizeof(FIColorRun));
    ch->colors = (uint32_t *)malloc(ch->cap_colors * sizeof(uint32_t));
    if (!ch->current || !ch->events || !ch->runs || !ch->colors) {
        free(ch->current); free(ch->events); free(ch->runs); free(ch->colors);
        memset(ch, 0, sizeof(*ch));
        return false;
    }
    return true;
}

static inline void fi_colorhist_free(FIColorHist *ch)
{
    for (uint32_t i = 0; i < ch->num_keyframes; i++) {
        free(ch->keyframes[i].image);
    }
    free(ch->current); free(ch->events); free(ch->runs); free(ch->colors);
    memset(ch, 0, sizeof(*ch));
    ch->events = NULL;
}

static inline void fi_ch_snapshot_keyframe(FIColorHist *ch, uint32_t after_event)
{
    if (ch->num_keyframes >= FI_CH_MAX_KEYFRAMES) {
        return; /* keyframes are an optimization; reconstruction still works */
    }
    FIKeyframe *kf = &ch->keyframes[ch->num_keyframes++];
    kf->after_event = after_event;
    kf->image = (uint32_t *)malloc(ch->npix * sizeof(uint32_t));
    if (kf->image) {
        memcpy(kf->image, ch->current, ch->npix * sizeof(uint32_t));
    } else {
        ch->num_keyframes--;
    }
}

static inline void fi_colorhist_set_baseline(FIColorHist *ch,
                                             const uint32_t *rgba)
{
    memcpy(ch->current, rgba, ch->npix * sizeof(uint32_t));
    ch->has_baseline = true;
    fi_ch_snapshot_keyframe(ch, 0); /* keyframe tagged after_event 0 = baseline */
}

static inline bool fi_ch_grow_runs(FIColorHist *ch, uint32_t need)
{
    if (ch->num_runs + need <= ch->cap_runs) return true;
    uint32_t nc = ch->cap_runs * 2;
    while (nc < ch->num_runs + need) nc *= 2;
    if (nc > FI_CH_RUN_CAP) { ch->truncated = true; return false; }
    FIColorRun *nr = (FIColorRun *)realloc(ch->runs, nc * sizeof(FIColorRun));
    if (!nr) { ch->truncated = true; return false; }
    ch->runs = nr; ch->cap_runs = nc; return true;
}

static inline bool fi_ch_grow_colors(FIColorHist *ch, uint32_t need)
{
    if (ch->num_colors + need <= ch->cap_colors) return true;
    uint32_t nc = ch->cap_colors * 2;
    while (nc < ch->num_colors + need) nc *= 2;
    if (nc > FI_CH_COLOR_CAP) { ch->truncated = true; return false; }
    uint32_t *nn = (uint32_t *)realloc(ch->colors, nc * sizeof(uint32_t));
    if (!nn) { ch->truncated = true; return false; }
    ch->colors = nn; ch->cap_colors = nc; return true;
}

static inline bool fi_colorhist_add_event(FIColorHist *ch, uint32_t event_id,
                                          const uint32_t *post)
{
    if (ch->num_events >= FI_CH_MAX_EVENTS) { ch->truncated = true; return false; }
    FIColorEvent *ev = &ch->events[ch->num_events];
    ev->event_id = event_id;
    ev->run_first = ch->num_runs;
    ev->run_count = 0;
    uint32_t i = 0;
    while (i < ch->npix) {
        if (ch->current[i] == post[i]) { i++; continue; }
        uint32_t start = i;
        while (i < ch->npix && ch->current[i] != post[i]) i++;
        uint32_t len = i - start;
        if (!fi_ch_grow_runs(ch, 1) || !fi_ch_grow_colors(ch, 2 * len)) {
            return false;
        }
        FIColorRun *r = &ch->runs[ch->num_runs++];
        r->start = start; r->len = len; r->color_off = ch->num_colors;
        for (uint32_t k = 0; k < len; k++) {
            ch->colors[ch->num_colors + k] = ch->current[start + k];      /* before */
            ch->colors[ch->num_colors + len + k] = post[start + k];       /* after  */
        }
        ch->num_colors += 2 * len;
        ev->run_count++;
    }
    memcpy(ch->current, post, ch->npix * sizeof(uint32_t));
    ch->num_events++;
    if ((ch->num_events % ch->keyframe_interval) == 0) {
        fi_ch_snapshot_keyframe(ch, ch->num_events - 1);
    }
    return true;
}

static inline uint32_t fi_colorhist_num_events(const FIColorHist *ch)
{
    return ch->num_events;
}

static inline void fi_colorhist_reconstruct(const FIColorHist *ch,
                                            uint32_t event_index, uint32_t *out)
{
    /* nearest keyframe with after_event <= event_index (baseline kf = 0) */
    const FIKeyframe *best = NULL;
    for (uint32_t i = 0; i < ch->num_keyframes; i++) {
        uint32_t ae = ch->keyframes[i].after_event;
        if (ae <= event_index && (!best || ae >= best->after_event)) {
            best = &ch->keyframes[i];
        }
    }
    uint32_t start_ev;
    if (best) {
        memcpy(out, best->image, ch->npix * sizeof(uint32_t));
        start_ev = best->after_event ? best->after_event + 1 : 0;
        /* baseline keyframe (after_event 0) already equals pre-event-0 state
         * only if no event 0 applied; distinguish via has_baseline: the
         * baseline kf image is the state BEFORE event 0, so replay from 0. */
        if (best->after_event == 0) start_ev = 0;
    } else {
        memset(out, 0, ch->npix * sizeof(uint32_t));
        start_ev = 0;
    }
    for (uint32_t e = start_ev; e <= event_index && e < ch->num_events; e++) {
        const FIColorEvent *ev = &ch->events[e];
        for (uint32_t r = 0; r < ev->run_count; r++) {
            const FIColorRun *run = &ch->runs[ev->run_first + r];
            const uint32_t *after = &ch->colors[run->color_off + run->len];
            for (uint32_t k = 0; k < run->len; k++) {
                out[run->start + k] = after[k];
            }
        }
    }
}

static inline int fi_colorhist_pixel_history(const FIColorHist *ch,
                                             uint32_t pixel_index,
                                             FIColorTouch *out, int max)
{
    int n = 0;
    for (uint32_t e = 0; e < ch->num_events && n < max; e++) {
        const FIColorEvent *ev = &ch->events[e];
        for (uint32_t r = 0; r < ev->run_count; r++) {
            const FIColorRun *run = &ch->runs[ev->run_first + r];
            if (pixel_index >= run->start && pixel_index < run->start + run->len) {
                uint32_t off = pixel_index - run->start;
                out[n].event_id = ev->event_id;
                out[n].before = ch->colors[run->color_off + off];
                out[n].after = ch->colors[run->color_off + run->len + off];
                n++;
                break;
            }
        }
    }
    return n;
}
```

Note for implementer: the baseline keyframe stores the image state *before* event 0, tagged `after_event = 0`; the reconstruct loop treats a matched baseline keyframe by replaying from event 0. A keyframe taken *after* event N (interval snapshot) is tagged `after_event = N` and replays from N+1. If the reconstruct test for keyframe exactness fails, verify this baseline-vs-interval distinction is handled as written (the `if (best->after_event == 0) start_ev = 0;` line).

- [ ] **Step 4: Run test to verify it passes**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-colorhist.exe tests/frameinspect/test-fi-colorhist.c && ./tests/frameinspect/test-fi-colorhist.exe`
Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add xemu-frameinspect-colorhist.h tests/frameinspect/test-fi-colorhist.c
git commit -m "frameinspect: Add colour-history codec (keyframes + RLE deltas)"
```

---

### Task 3: Resource snapshot pool header

**Files:**
- Create: `xemu-frameinspect-resources.h`
- Test: `tests/frameinspect/test-fi-resources.c`

**Interfaces:**
- Consumes: nothing (standalone).
- Produces: `FIResourcePool`; `bool fi_resources_init(FIResourcePool *p)`; `void fi_resources_free(FIResourcePool *p)`; `uint32_t fi_resources_intern(FIResourcePool *p, uint32_t kind, const void *data, uint32_t len, uint64_t meta)` returning a resource id (deduplicated by content hash + `len` + `kind` + `meta`) or `FI_RES_INVALID` when capped; record fields `p->res[id]` = `{ uint32_t kind; uint32_t len; uint64_t off; uint64_t meta; uint64_t hash; }`, bytes at `p->blob + off`. Constants `FI_RES_INVALID` (0xFFFFFFFF), `FI_RES_MAX` (65536), `FI_RES_BLOB_CAP` (default 256 MiB). Resource kinds are opaque enum values chosen by the caller (Plan 2B): texture, palette, vertex, index, shader.

- [ ] **Step 1: Write the failing test**

Create `tests/frameinspect/test-fi-resources.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xemu-frameinspect-resources.h"

int main(void)
{
    FIResourcePool p;
    assert(fi_resources_init(&p));

    uint8_t a[64], b[64];
    memset(a, 0xAB, sizeof(a));
    memset(b, 0xCD, sizeof(b));

    uint32_t r0 = fi_resources_intern(&p, 1, a, sizeof(a), 0);
    assert(r0 != FI_RES_INVALID);
    assert(p.res[r0].len == 64 && p.res[r0].kind == 1);

    /* identical bytes+kind+meta -> same id, blob doesn't grow */
    uint64_t used = p.blob_used;
    uint32_t r0b = fi_resources_intern(&p, 1, a, sizeof(a), 0);
    assert(r0b == r0 && p.blob_used == used);

    /* different bytes -> new id */
    uint32_t r1 = fi_resources_intern(&p, 1, b, sizeof(b), 0);
    assert(r1 != r0);

    /* same bytes, different kind -> distinct */
    uint32_t r2 = fi_resources_intern(&p, 2, a, sizeof(a), 0);
    assert(r2 != r0);

    /* same bytes+kind, different meta -> distinct (e.g. format/dims) */
    uint32_t r3 = fi_resources_intern(&p, 1, a, sizeof(a), 0x1234);
    assert(r3 != r0);

    /* stored bytes are retrievable and correct */
    assert(memcmp(p.blob + p.res[r0].off, a, 64) == 0);

    fi_resources_free(&p);
    assert(p.blob == NULL);
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-resources.exe tests/frameinspect/test-fi-resources.c`
Expected: FAIL — header not found.

- [ ] **Step 3: Write the header**

Create `xemu-frameinspect-resources.h` (license + guard `XEMU_FRAMEINSPECT_RESOURCES_H`):

```c
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_RES_INVALID  0xFFFFFFFFu
#define FI_RES_MAX      65536u
#define FI_RES_HASH_CAP (1u << 17)   /* 2x FI_RES_MAX, power of two */
#define FI_RES_BLOB_CAP (256ull << 20)

typedef struct FIResource {
    uint32_t kind;
    uint32_t len;
    uint64_t off;
    uint64_t meta;
    uint64_t hash;
} FIResource;

typedef struct FIResourcePool {
    FIResource *res;      /* [FI_RES_MAX] */
    uint32_t num_res;
    uint8_t *blob;        /* growable up to FI_RES_BLOB_CAP */
    uint64_t blob_used, blob_cap;
    uint32_t *hash_slots; /* [FI_RES_HASH_CAP], value = id+1, 0 = empty */
    bool truncated;
} FIResourcePool;

static inline uint64_t fi_res_hash(uint32_t kind, const void *data,
                                   uint32_t len, uint64_t meta)
{
    /* FNV-1a over kind, meta, and bytes. */
    uint64_t h = 1469598103934665603ull;
    const uint8_t *pfx = (const uint8_t *)&kind;
    for (int i = 0; i < 4; i++) { h ^= pfx[i]; h *= 1099511628211ull; }
    const uint8_t *mpx = (const uint8_t *)&meta;
    for (int i = 0; i < 8; i++) { h ^= mpx[i]; h *= 1099511628211ull; }
    const uint8_t *d = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) { h ^= d[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

static inline bool fi_resources_init(FIResourcePool *p)
{
    memset(p, 0, sizeof(*p));
    p->res = (FIResource *)calloc(FI_RES_MAX, sizeof(FIResource));
    p->hash_slots = (uint32_t *)calloc(FI_RES_HASH_CAP, sizeof(uint32_t));
    p->blob_cap = 1u << 20;
    p->blob = (uint8_t *)malloc(p->blob_cap);
    if (!p->res || !p->hash_slots || !p->blob) {
        free(p->res); free(p->hash_slots); free(p->blob);
        memset(p, 0, sizeof(*p));
        return false;
    }
    return true;
}

static inline void fi_resources_free(FIResourcePool *p)
{
    free(p->res); free(p->hash_slots); free(p->blob);
    memset(p, 0, sizeof(*p));
    p->blob = NULL;
}

static inline uint32_t fi_resources_intern(FIResourcePool *p, uint32_t kind,
                                           const void *data, uint32_t len,
                                           uint64_t meta)
{
    uint64_t h = fi_res_hash(kind, data, len, meta);
    uint32_t i = (uint32_t)h & (FI_RES_HASH_CAP - 1);
    for (;;) {
        uint32_t slot = p->hash_slots[i];
        if (slot == 0) break;
        FIResource *r = &p->res[slot - 1];
        if (r->hash == h && r->kind == kind && r->len == len && r->meta == meta &&
            memcmp(p->blob + r->off, data, len) == 0) {
            return slot - 1;
        }
        i = (i + 1) & (FI_RES_HASH_CAP - 1);
    }
    if (p->num_res >= FI_RES_MAX) { p->truncated = true; return FI_RES_INVALID; }
    while (p->blob_used + len > p->blob_cap) {
        uint64_t nc = p->blob_cap * 2;
        if (nc > FI_RES_BLOB_CAP) { p->truncated = true; return FI_RES_INVALID; }
        uint8_t *nb = (uint8_t *)realloc(p->blob, nc);
        if (!nb) { p->truncated = true; return FI_RES_INVALID; }
        p->blob = nb; p->blob_cap = nc;
    }
    uint32_t id = p->num_res++;
    FIResource *r = &p->res[id];
    r->kind = kind; r->len = len; r->meta = meta; r->hash = h;
    r->off = p->blob_used;
    memcpy(p->blob + r->off, data, len);
    p->blob_used += len;
    p->hash_slots[i] = id + 1;
    return id;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-resources.exe tests/frameinspect/test-fi-resources.c && ./tests/frameinspect/test-fi-resources.exe`
Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add xemu-frameinspect-resources.h tests/frameinspect/test-fi-resources.c
git commit -m "frameinspect: Add content-hash resource snapshot pool"
```

---

### Task 4: Event log + budget header

**Files:**
- Create: `xemu-frameinspect-eventlog.h`
- Test: `tests/frameinspect/test-fi-eventlog.c`

**Interfaces:**
- Consumes: nothing (standalone).
- Produces: event-kind enum `FI_EV_METHOD, FI_EV_BATCH, FI_EV_CLEAR, FI_EV_BLIT, FI_EV_UPLOAD, FI_EV_SCANOUT`; `FIEvent { uint8_t kind; uint32_t surface_gen; uint32_t seq; uint32_t a0,a1,a2,a3; }`; `FIEventLog`; `bool fi_eventlog_init(FIEventLog *l)`; `void fi_eventlog_free(FIEventLog *l)`; `uint32_t fi_eventlog_append(FIEventLog *l, uint8_t kind, uint32_t surface_gen, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)` returning the event index or `FI_EVENT_INVALID` when capped; `FIBudget { uint64_t limit, used; }`; `bool fi_budget_try(FIBudget *b, uint64_t bytes)` (returns false and leaves `used` unchanged when it would exceed `limit`). Constants `FI_EVENT_INVALID` (0xFFFFFFFF), `FI_EVENTLOG_CAP` (1u<<20).

- [ ] **Step 1: Write the failing test**

Create `tests/frameinspect/test-fi-eventlog.c`:

```c
#include <assert.h>
#include <stdio.h>
#include "../../xemu-frameinspect-eventlog.h"

int main(void)
{
    FIEventLog l;
    assert(fi_eventlog_init(&l));

    uint32_t e0 = fi_eventlog_append(&l, FI_EV_BATCH, 3, 100, 0, 0, 0);
    uint32_t e1 = fi_eventlog_append(&l, FI_EV_CLEAR, 3, 0, 0, 0, 0);
    assert(e0 == 0 && e1 == 1 && l.count == 2);
    assert(l.events[e0].kind == FI_EV_BATCH && l.events[e0].surface_gen == 3);
    assert(l.events[e0].seq == 0 && l.events[e1].seq == 1);
    assert(l.events[e0].a0 == 100);

    /* budget: take within limit succeeds, over-limit refused without charge */
    FIBudget b = { .limit = 1000, .used = 0 };
    assert(fi_budget_try(&b, 600) && b.used == 600);
    assert(!fi_budget_try(&b, 500) && b.used == 600);
    assert(fi_budget_try(&b, 400) && b.used == 1000);

    /* cap: fill to FI_EVENTLOG_CAP, next append refused + truncated flag */
    while (l.count < FI_EVENTLOG_CAP) {
        fi_eventlog_append(&l, FI_EV_METHOD, 0, 0, 0, 0, 0);
    }
    assert(fi_eventlog_append(&l, FI_EV_METHOD, 0, 0, 0, 0, 0) == FI_EVENT_INVALID);
    assert(l.truncated);

    fi_eventlog_free(&l);
    assert(l.events == NULL);
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-eventlog.exe tests/frameinspect/test-fi-eventlog.c`
Expected: FAIL — header not found.

- [ ] **Step 3: Write the header**

Create `xemu-frameinspect-eventlog.h` (license + guard `XEMU_FRAMEINSPECT_EVENTLOG_H`):

```c
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_EVENT_INVALID 0xFFFFFFFFu
#define FI_EVENTLOG_CAP  (1u << 20)

enum {
    FI_EV_METHOD = 0,
    FI_EV_BATCH,
    FI_EV_CLEAR,
    FI_EV_BLIT,
    FI_EV_UPLOAD,
    FI_EV_SCANOUT,
};

typedef struct FIEvent {
    uint8_t kind;
    uint32_t surface_gen;
    uint32_t seq;
    uint32_t a0, a1, a2, a3;
} FIEvent;

typedef struct FIEventLog {
    FIEvent *events;
    uint32_t count, cap;
    bool truncated;
} FIEventLog;

typedef struct FIBudget {
    uint64_t limit;
    uint64_t used;
} FIBudget;

static inline bool fi_eventlog_init(FIEventLog *l)
{
    memset(l, 0, sizeof(*l));
    l->cap = 4096;
    l->events = (FIEvent *)malloc(l->cap * sizeof(FIEvent));
    return l->events != NULL;
}

static inline void fi_eventlog_free(FIEventLog *l)
{
    free(l->events);
    memset(l, 0, sizeof(*l));
    l->events = NULL;
}

static inline uint32_t fi_eventlog_append(FIEventLog *l, uint8_t kind,
                                          uint32_t surface_gen, uint32_t a0,
                                          uint32_t a1, uint32_t a2, uint32_t a3)
{
    if (l->count >= FI_EVENTLOG_CAP) { l->truncated = true; return FI_EVENT_INVALID; }
    if (l->count >= l->cap) {
        uint32_t nc = l->cap * 2;
        if (nc > FI_EVENTLOG_CAP) nc = FI_EVENTLOG_CAP;
        FIEvent *ne = (FIEvent *)realloc(l->events, nc * sizeof(FIEvent));
        if (!ne) { l->truncated = true; return FI_EVENT_INVALID; }
        l->events = ne; l->cap = nc;
    }
    uint32_t idx = l->count++;
    FIEvent *e = &l->events[idx];
    e->kind = kind; e->surface_gen = surface_gen; e->seq = idx;
    e->a0 = a0; e->a1 = a1; e->a2 = a2; e->a3 = a3;
    return idx;
}

static inline bool fi_budget_try(FIBudget *b, uint64_t bytes)
{
    if (b->used + bytes > b->limit) return false;
    b->used += bytes;
    return true;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-eventlog.exe tests/frameinspect/test-fi-eventlog.c && ./tests/frameinspect/test-fi-eventlog.exe`
Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add xemu-frameinspect-eventlog.h tests/frameinspect/test-fi-eventlog.c
git commit -m "frameinspect: Add capture event log + byte budget"
```

---

### Task 5: Capture module (state machine + immutable publish)

**Files:**
- Create: `xemu-frameinspect-capture.h`, `xemu-frameinspect-capture.c`
- Modify: `meson.build` (line ~4054, the `specific_ss.add(files(...))` that already lists `xemu-frameinspect.c`)

**Interfaces:**
- Consumes: the four headers (Tasks 1–4); Plan 1's `xemu_frameinspect_arm(uint64_t ram_size)` / `xemu_frameinspect_disarm()`.
- Produces (used by Tasks 6/7 and Plan 3):

```c
typedef enum { FI_CAP_IDLE, FI_CAP_ARMED, FI_CAP_CAPTURING, FI_CAP_DONE } FICaptureState;

typedef struct FICapture {
    FISurfaceStore surfaces;
    FIResourcePool resources;
    FIEventLog events;
    FIBudget budget;
    FIColorHist *hist;        /* [surfaces.num_gens], allocated lazily in 2B */
    uint32_t hist_count;
    uint32_t open_batch_gen;  /* surface gen of the batch currently open, or INVALID */
    bool truncated;
} FICapture;

/* Arm a one-shot capture: begins the lead-in (Plan-1 instrumentation on). */
void xemu_frameinspect_capture_arm(uint64_t ram_size);
/* Called on the pfifo thread at each NV097_FLIP_STALL. Returns true when the
 * capture just completed and the caller should request the VM pause. */
bool xemu_frameinspect_capture_on_flip(void);
FICaptureState xemu_frameinspect_capture_state(void);
/* Skeleton event recorders (Task 7 calls these; 2B extends them). */
void xemu_frameinspect_capture_begin_batch(uint32_t surface_gen);
void xemu_frameinspect_capture_end_batch(void);
void xemu_frameinspect_capture_event(uint8_t kind, uint32_t surface_gen);
uint32_t xemu_frameinspect_capture_intern_surface(const FISurfaceKey *k);
/* Published immutable capture for the UI (Plan 3); NULL until first publish. */
const FICapture *xemu_frameinspect_capture_get(void);
/* Human-readable one-line summary of the published capture (g_strdup'd). */
char *xemu_frameinspect_capture_summary(void);
```

- [ ] **Step 1: Write the header**

Create `xemu-frameinspect-capture.h` with the license header, guard `XEMU_FRAMEINSPECT_CAPTURE_H`, `extern "C"` wrapper, includes of the four data-model headers, and exactly the type + function declarations in the Interfaces block. `#define FI_CAP_BUDGET_DEFAULT (1ull << 30)`.

- [ ] **Step 2: Write the implementation**

Create `xemu-frameinspect-capture.c`. Structure (full logic — the implementer writes this against the header APIs):

```c
/* license header */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "xemu-frameinspect.h"
#include "xemu-frameinspect-capture.h"

static FICapture fi_cap;                 /* capture in progress */
static FICaptureState fi_state = FI_CAP_IDLE;
static const FICapture *fi_published;    /* immutable, read by UI thread */
static uint64_t fi_cap_ram_size;

static void fi_capture_reset(FICapture *c)
{
    fi_surfaces_free(&c->surfaces);
    fi_resources_free(&c->resources);
    fi_eventlog_free(&c->events);
    for (uint32_t i = 0; i < c->hist_count; i++) {
        fi_colorhist_free(&c->hist[i]);
    }
    free(c->hist);
    memset(c, 0, sizeof(*c));
}

static bool fi_capture_alloc(FICapture *c)
{
    memset(c, 0, sizeof(*c));
    c->open_batch_gen = FI_SURFGEN_INVALID;
    c->budget.limit = FI_CAP_BUDGET_DEFAULT;
    if (!fi_surfaces_init(&c->surfaces) || !fi_resources_init(&c->resources) ||
        !fi_eventlog_init(&c->events)) {
        fi_capture_reset(c);
        return false;
    }
    return true;
}

void xemu_frameinspect_capture_arm(uint64_t ram_size)
{
    if (fi_state != FI_CAP_IDLE && fi_state != FI_CAP_DONE) {
        return; /* a capture is already in flight */
    }
    fi_cap_ram_size = ram_size;
    fi_state = FI_CAP_ARMED;
    /* Lead-in: enable Plan-1 guest instrumentation now so writes during the
     * frame before the captured frame are tagged. */
    xemu_frameinspect_arm(ram_size);
}

bool xemu_frameinspect_capture_on_flip(void)
{
    switch (fi_state) {
    case FI_CAP_ARMED:
        /* First flip after arming: the captured frame starts now. */
        if (!fi_capture_alloc(&fi_cap)) {
            fi_state = FI_CAP_IDLE;
            xemu_frameinspect_disarm();
            return false;
        }
        fi_state = FI_CAP_CAPTURING;
        return false;
    case FI_CAP_CAPTURING: {
        /* Second flip: the captured frame ended. Finalize + publish. */
        FICapture *done = (FICapture *)malloc(sizeof(FICapture));
        if (done) {
            *done = fi_cap;             /* transfer ownership of buffers */
            memset(&fi_cap, 0, sizeof(fi_cap));
            const FICapture *old = fi_published;
            qatomic_store_release(&fi_published, done);
            if (old) {
                fi_capture_reset((FICapture *)old);
                free((void *)old);
            }
        } else {
            fi_capture_reset(&fi_cap);
        }
        fi_state = FI_CAP_DONE;
        xemu_frameinspect_disarm();     /* end lead-in instrumentation */
        return true;                    /* request the pause */
    }
    default:
        return false;
    }
}

FICaptureState xemu_frameinspect_capture_state(void) { return fi_state; }

uint32_t xemu_frameinspect_capture_intern_surface(const FISurfaceKey *k)
{
    if (fi_state != FI_CAP_CAPTURING) return FI_SURFGEN_INVALID;
    return fi_surfaces_intern(&fi_cap.surfaces, k);
}

void xemu_frameinspect_capture_begin_batch(uint32_t surface_gen)
{
    if (fi_state != FI_CAP_CAPTURING) return;
    fi_cap.open_batch_gen = surface_gen;
    fi_eventlog_append(&fi_cap.events, FI_EV_BATCH, surface_gen, 0, 0, 0, 0);
}

void xemu_frameinspect_capture_end_batch(void)
{
    if (fi_state != FI_CAP_CAPTURING) return;
    fi_cap.open_batch_gen = FI_SURFGEN_INVALID;
}

void xemu_frameinspect_capture_event(uint8_t kind, uint32_t surface_gen)
{
    if (fi_state != FI_CAP_CAPTURING) return;
    fi_eventlog_append(&fi_cap.events, kind, surface_gen, 0, 0, 0, 0);
}

const FICapture *xemu_frameinspect_capture_get(void)
{
    return qatomic_load_acquire(&fi_published);
}

char *xemu_frameinspect_capture_summary(void)
{
    const FICapture *c = xemu_frameinspect_capture_get();
    if (!c) return g_strdup("Frame inspector: no capture");
    return g_strdup_printf("Captured frame: %u events, %u surfaces%s",
                           c->events.count, c->surfaces.num_gens,
                           (c->events.truncated || c->surfaces.truncated)
                               ? " [TRUNCATED]" : "");
}
```

Note for implementer (latent, no reader until Plan 3): `on_flip` frees the *previous* published capture right after the atomic swap. That is safe only because the UI reads a published capture strictly while the VM is paused, and a new capture (which triggers the free) requires un-pausing and re-arming first. Plan 3's UI must therefore not retain a `xemu_frameinspect_capture_get()` pointer across a potential re-arm. Do not add refcounting/hazard-pointers here — record the invariant and leave it; the Plan-3 review verifies the UI honours it.

- [ ] **Step 3: Wire into meson**

Modify `meson.build` line ~4054 so the `specific_ss.add(files(...))` list also contains `'xemu-frameinspect-capture.c'` (add it right after `'xemu-frameinspect.c'`).

- [ ] **Step 4: Build**

Run: `/c/msys64/usr/bin/bash.exe -l "C:/Users/Tera/AppData/Local/Temp/claude/C--Users-Tera-Documents-GitHub-xemu/64ca3ae4-7ebb-4ef2-bef8-a381f134df21/scratchpad/build-xemu.sh"`
Expected: `xemu-frameinspect-capture.c` compiles; both `qemu-system-i386.exe` and `qemu-system-i386w.exe` link. (Pre-existing `qemu_ftruncate64` qtest link failures are unrelated.)

- [ ] **Step 5: Commit**

```bash
git add xemu-frameinspect-capture.h xemu-frameinspect-capture.c meson.build
git commit -m "frameinspect: Add capture state machine + immutable publish"
```

---

### Task 6: Flip-stall hook + async VM pause

**Files:**
- Modify: `hw/xbox/nv2a/pgraph/pgraph.c` (`NV097_FLIP_STALL` handler ~901-908)
- Modify: `ui/xemu.c` (the `SDL_SCANCODE_I` case added in Plan 1)

**Interfaces:**
- Consumes: `xemu_frameinspect_capture_on_flip()`, `xemu_frameinspect_capture_arm()`, `xemu_frameinspect_capture_summary()`; QEMU `aio_bh_schedule_oneshot`, `qemu_get_aio_context`, `vm_stop`.
- Produces: the runtime capture trigger (Ctrl+Alt+I arms a one-shot capture; the frame is captured and the VM pauses).

- [ ] **Step 1: Add the pause bottom half + flip hook in pgraph.c**

At the top of `hw/xbox/nv2a/pgraph/pgraph.c` includes, add `#include "system/runstate.h"` (for `vm_stop`) and `#include "block/aio.h"` / `#include "qemu/main-loop.h"` if not already present (check existing includes first — `qemu/main-loop.h` is usually already included). Add near the other file-scope helpers:

```c
#include "xemu-frameinspect-capture.h"

static void fi_capture_pause_bh(void *opaque)
{
    /* Runs in the main loop under the BQL; safe to stop the VM here. */
    vm_stop(RUN_STATE_PAUSED);
}
```

In the `NV097_FLIP_STALL` handler (currently lines 901-908), after `d->pgraph.renderer->ops.flip_stall(d);` and the existing `nv2a_profile_flip_stall();`, add:

```c
    if (xemu_frameinspect_capture_on_flip()) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                fi_capture_pause_bh, NULL);
    }
```

(This runs on the pfifo thread without the BQL; `aio_bh_schedule_oneshot` is thread-safe and defers `vm_stop` to the BQL-holding main loop — pattern from `ui/console.c:160`.)

- [ ] **Step 2: Repurpose the hotkey in ui/xemu.c**

Replace the body of the `SDL_SCANCODE_I` case (added in Plan 1) so it arms a one-shot capture instead of toggling instrumentation:

```c
        case SDL_SCANCODE_I: {
            gui_keysym = 1;
            if (xemu_get_xbe_info() != NULL) {
                MachineState *ms = MACHINE(qdev_get_machine());
                xemu_frameinspect_capture_arm(ms->ram_size);
                xemu_queue_notification(
                    "Frame inspector: capturing next frame...");
            } else {
                xemu_queue_notification("Load a game before inspecting");
            }
            break;
        }
```

Add `#include "../xemu-frameinspect-capture.h"` beside the existing `#include "../xemu-frameinspect.h"`.

- [ ] **Step 3: Toast the summary when the capture completes**

The pause happens asynchronously on the main thread. Post the summary toast from the pause bottom half so the user sees the result. Update `fi_capture_pause_bh` in pgraph.c:

```c
static void fi_capture_pause_bh(void *opaque)
{
    vm_stop(RUN_STATE_PAUSED);
    char *msg = xemu_frameinspect_capture_summary();
    xemu_queue_notification(msg);
    g_free(msg);
}
```

Add `#include "ui/xemu-notifications.h"` to pgraph.c if `xemu_queue_notification` is not already declared there (check first; if it introduces a UI dependency into pgraph.c that breaks the build, instead move the toast into `ui/xemu.c`'s main loop by having the BH only call `vm_stop`, and detect the paused+capture-present state in the render loop to toast once — record whichever approach you took in the report).

- [ ] **Step 4: Build**

Run the build script. Expected: clean compile of pgraph.c and ui/xemu.c; both emulator exes link.

- [ ] **Step 5: Commit**

```bash
git add hw/xbox/nv2a/pgraph/pgraph.c ui/xemu.c
git commit -m "frameinspect: Arm one-shot capture; pause VM at second flip"
```

---

### Task 7: Renderer-ops event hooks (surface generations + batch/clear/blit skeleton)

**Files:**
- Modify: `hw/xbox/nv2a/pgraph/gl/surface.c` (`update_surface_part` ~1102, where `r->color_binding`/`r->zeta_binding` are set)
- Modify: `hw/xbox/nv2a/pgraph/gl/draw.c` (`pgraph_gl_draw_begin` 134, `pgraph_gl_draw_end` 333, `pgraph_gl_clear_surface` 27)
- Modify: `hw/xbox/nv2a/pgraph/gl/blit.c` (`pgraph_gl_image_blit` 72)

**Interfaces:**
- Consumes: `xemu_frameinspect_capture_state()`, `xemu_frameinspect_capture_intern_surface()`, `xemu_frameinspect_capture_begin_batch/end_batch/event()`; the GL `SurfaceBinding` fields (renderer.h:41-67) and `pg->surface_scale_factor`.
- Produces: during a capture, the event stream (batches, clears, blits) with each event tagged by the interned surface generation of the current colour binding. No pixels yet (2B).

- [ ] **Step 1: Add a capture helper to derive a FISurfaceKey from the current colour binding**

In `hw/xbox/nv2a/pgraph/gl/surface.c`, add a small static helper near the top (after includes) that builds an `FISurfaceKey` from a `SurfaceBinding *` and interns it, returning the generation id (or `FI_SURFGEN_INVALID` when not capturing):

```c
#include "xemu-frameinspect-capture.h"

static uint32_t fi_intern_current_color(NV2AState *d)
{
    if (xemu_frameinspect_capture_state() != FI_CAP_CAPTURING) {
        return FI_SURFGEN_INVALID;
    }
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;
    SurfaceBinding *s = r->color_binding;
    if (!s) return FI_SURFGEN_INVALID;
    FISurfaceKey k = {0};
    k.addr = s->vram_addr;
    k.format = s->fmt.gl_internal_format;   /* stable per-format id; confirm field */
    k.pitch = s->pitch;
    k.width = s->width;
    k.height = s->height;
    k.swizzle = s->swizzle ? 1 : 0;
    k.color = 1;
    k.aa = (uint8_t)s->shape.anti_aliasing; /* confirm field name in SurfaceShape */
    k.scale = (uint8_t)pg->surface_scale_factor;
    return xemu_frameinspect_capture_intern_surface(&k);
}
```

Note for implementer: verify the exact field names against `renderer.h` `SurfaceBinding`/`SurfaceShape` and `SurfaceFormatInfo fmt` — the map only needs a *stable* identity per distinct surface configuration, so any deterministic per-format value works for `k.format` (e.g. `s->fmt.gl_internal_format`, or `s->shape.color_format`). Pick a real field and record it.

Declare it in `renderer.h` (or keep static and expose via a tiny accessor) so draw.c/blit.c can call it. Simplest: make it non-static `uint32_t pgraph_gl_fi_intern_current_color(NV2AState *d);` and declare in `renderer.h` beside the other `pgraph_gl_*` surface prototypes.

- [ ] **Step 2: Hook draw_begin / draw_end (batch events)**

In `pgraph_gl_draw_begin` (draw.c:134), after `pgraph_gl_surface_update(...)` and the `assert(r->color_binding || r->zeta_binding)` (so the binding is resolved), add:

```c
    xemu_frameinspect_capture_begin_batch(pgraph_gl_fi_intern_current_color(d));
```

In `pgraph_gl_draw_end` (draw.c:333), after `pgraph_gl_flush_draw(d)` and the draw_time updates, add:

```c
    xemu_frameinspect_capture_end_batch();
```

Add `#include "xemu-frameinspect-capture.h"` to draw.c.

- [ ] **Step 3: Hook clear_surface and image_blit**

In `pgraph_gl_clear_surface` (draw.c:27), after `pgraph_gl_surface_update(...)` resolves the binding and the `glClear`, add:

```c
    xemu_frameinspect_capture_event(FI_EV_CLEAR,
                                    pgraph_gl_fi_intern_current_color(d));
```

In `pgraph_gl_image_blit` (blit.c:72), after the blit completes (near the `memory_region_set_client_dirty` calls at the end), add:

```c
    xemu_frameinspect_capture_event(FI_EV_BLIT,
                                    pgraph_gl_fi_intern_current_color(d));
```

Add `#include "xemu-frameinspect-capture.h"` (and `renderer.h` if needed for the prototype) to blit.c.

- [ ] **Step 4: Build**

Run the build script. Expected: draw.c, blit.c, surface.c compile; both emulator exes link.

- [ ] **Step 5: Manual integration check (deferred to human — list in report)**

The implementer should NOT run this (needs a live game); list these steps in the task report for the human:
1. Launch a game, press Ctrl+Alt+I → toast "capturing next frame...".
2. The VM should pause within a frame or two; toast shows `Captured frame: N events, M surfaces` with N and M both > 0 (batches + clears from the real frame; at least one surface generation).
3. Un-pause via the existing pause toggle; the game resumes normally.
4. Re-arm several times → stable, each capture replaces the previous published one.

- [ ] **Step 6: Commit**

```bash
git add hw/xbox/nv2a/pgraph/gl/surface.c hw/xbox/nv2a/pgraph/gl/draw.c hw/xbox/nv2a/pgraph/gl/blit.c hw/xbox/nv2a/pgraph/gl/renderer.h
git commit -m "frameinspect: Record batch/clear/blit events + surface generations during capture"
```

---

## Self-review checklist (run after writing, before execution)

- **Spec coverage:** surface generations ✓ (T1), colour-history codec ✓ (T2, pixels wired in 2B), resource pool ✓ (T3, populated in 2B), event log + budget ✓ (T4), capture lifecycle + immutable publish + async pause ✓ (T5/T6), surface-tagged event stream ✓ (T7). Deferred to 2B: real pixel readback/diff, PFIFO method-origin logging, register/state snapshots, resource snapshotting, scanout image + PCRTC/PVIDEO. Deferred watch-subsystem findings (M2/M3/M4 from Plan 1) also land in 2B.
- **Placeholder scan:** none — every header task has complete code; integration tasks give exact insertion sites + representative code with explicit "confirm this field" notes where a real symbol must be verified against the tree (these are implementer verification points, not placeholders).
- **Type consistency:** `FI_SURFGEN_INVALID` used consistently across T1/T5/T7; `FICapture` fields match between T5 header and T5 impl; event kinds (T4) used in T7.
- **Ambiguity:** the colour-history baseline-keyframe semantics are the one subtle point — called out explicitly in T2 with the guarding line and a debugging note.

## Out of scope (this plan — deferred to Plan 2B)

- Real surface pixel readback + per-writer-event diff into `FIColorHist` (T2 codec is ready; T7 records events but no pixels).
- Per-draw register/state snapshots and PFIFO method-event logging with `CommandOrigin` (source addr + writer tag from Plan 1's tag map).
- Resource snapshotting (texture/palette/vertex/index) into `FIResourcePool`.
- Scanout event: final displayed image + PCRTC/PVIDEO/line-offset/transform.
- Wiring the `FICapture.hist[]` array (per-generation colour history allocation) and the global budget across pixel/resource stores.

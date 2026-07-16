# Frame Inspector Capture Engine — Plan 2B (live GL capture fill-in) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Populate the Plan-2A capture skeleton with real content — per-writer-event colour-history readback+diff, PFIFO method-event logging with guest-code origins, per-draw register + resource snapshots, and the final scanout event — and harden the lifecycle (budget wiring, truncation reporting, `fi_state` atomicity, capture-cancel), so an armed capture yields a fully inspectable frame.

**Architecture:** One new standalone unit-tested header (method-origin log). Everything else extends existing files: the capture readback + colour-history diff live in `gl/surface.c` (which owns the static surface-download path), method-origin logging hooks the PFIFO pusher, register/resource snapshots hook the GL draw path and reuse the Plan-2A resource pool (register files and texture/vertex bytes are all just content-hashed blobs), and scanout hooks the display path. The capture module gains the per-generation colour-history array, budget enforcement, and full truncation roll-up.

**Tech Stack:** C11, QEMU NV2A pgraph GL renderer + PFIFO pusher, Plan-1 tag map (`xemu_frameinspect_lookup_tag`), Plan-2A capture data model.

**Spec:** `docs/superpowers/specs/2026-07-16-frame-inspector-design.md` (the "NV2A capture engine" scope — this plan is the live-content half).

**Builds on:** Plan 2A (`docs/superpowers/plans/2026-07-16-frame-inspector-capture.md`). It carries forward these Plan-2A review findings (fixed in Task 7 here unless noted): F1 `fi_state` cross-thread atomicity, F2 no capture-cancel, F5 truncation roll-up incomplete, F6 dead fields (`fi_cap_ram_size`, `has_baseline`), budget overflow check, and the deferred `open_batch_gen` pairing caution (F4).

## Global Constraints

- Header-only `xemu-frameinspect-*.h` files stay **QEMU-independent** (standalone-testable with `/c/msys64/ucrt64/bin/gcc.exe`; plain `gcc` is broken 2.95). Emulator build: `/c/msys64/usr/bin/bash.exe -l ".../scratchpad/build-xemu.sh"` (prefix `MSYSTEM=UCRT64` if ninja isn't found). Success = the touched objects compile and both `qemu-system-i386.exe` + `qemu-system-i386w.exe` link (pre-existing qtest `qemu_ftruncate64` link failures are unrelated).
- **Guarantee model (from the spec):** colour history is EXACT colour-change history (no claim about same-colour writes, rejected fragments, or blended contributors); resource snapshots are exact draw-time bytes; guest origins are best-effort with explicit `unattributed`/`partial-dword`/`lost-sync`/`pre-arm` states. Never label inferred data as ground truth.
- **RT-backed textures never read guest RAM** (`gl/texture.c` `surf_to_tex` path): a texture that is itself a rendered surface has no valid guest bytes; record it as a resource-dependency reference to the producing surface generation, not a guest-RAM snapshot.
- Everything runs on the **pfifo thread** (holding `pgraph.lock`) except the UI-thread arm/cancel; the published capture stays immutable + self-contained.
- All stores honour the global `FIBudget` (default 1 GiB) and per-store caps; every truncation sets a flag folded into `FICapture.truncated` and the summary — never a silent drop.
- License headers: GPL-2.0-or-later, "Copyright (C) 2026 xemu contributors". Commit prefix: `frameinspect:`.
- GL renderer only.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `xemu-frameinspect-methodlog.h` | create | Per-batch method-event log with guest-origin fields |
| `xemu-frameinspect-capture.h` / `.c` | modify | Per-gen colour-history array, method log, resource-ref lists, budget, truncation roll-up, `fi_state` atomic, cancel |
| `hw/xbox/nv2a/pgraph/gl/surface.c` | modify | Capture readback (RGBA8888) + colour-history baseline/diff hook |
| `hw/xbox/nv2a/pgraph/gl/draw.c` | modify (draw_begin 134, draw_end 333, clear 27) | Register + resource snapshots; drive colour-history diff per writer event |
| `hw/xbox/nv2a/pgraph/gl/blit.c` | modify (image_blit 72) | Colour-history diff for blit destination |
| `hw/xbox/nv2a/pgraph/pfifo.c` | modify (pusher ~295-346) | Method-event logging with source phys addr + writer tag |
| `hw/xbox/nv2a/pgraph/gl/display.c` | modify (`pgraph_gl_sync` 375) | Scanout event: displayed image + PCRTC/pvideo state |
| `ui/xemu.c` | modify (SDL_SCANCODE_I) | Re-press while capturing = cancel |
| `tests/frameinspect/test-fi-methodlog.c` | create | Unit test |

---

### Task 1: Method-origin log header

**Files:**
- Create: `xemu-frameinspect-methodlog.h`
- Test: `tests/frameinspect/test-fi-methodlog.c`

**Interfaces:**
- Consumes: nothing (standalone).
- Produces: origin-confidence enum `FI_ORIG_ATTRIBUTED, FI_ORIG_PARTIAL, FI_ORIG_UNATTRIBUTED, FI_ORIG_LOSTSYNC`; `FIMethodRec { uint32_t method; uint16_t subchannel; uint16_t confidence; uint32_t param; uint32_t phys_addr; uint32_t writer_node; }`; `FIMethodLog`; `bool fi_methodlog_init(FIMethodLog *m)`; `void fi_methodlog_free(FIMethodLog *m)`; `uint32_t fi_methodlog_append(FIMethodLog *m, uint32_t method, uint16_t subchannel, uint32_t param, uint32_t phys_addr, uint32_t writer_node, uint16_t confidence)` returning the record index or `FI_METHOD_INVALID` on cap; `void fi_methodlog_mark_batch(FIMethodLog *m, uint32_t batch_event, uint32_t first_rec, uint32_t rec_count)` recording that a batch owns records `[first_rec, first_rec+rec_count)`; accessors via `m->recs[i]` and `m->batches[]`. Constants `FI_METHOD_INVALID` (0xFFFFFFFF), `FI_METHODLOG_CAP` (1u<<22), `FI_METHODLOG_BATCH_CAP` (1u<<18). Truncation flag `m->truncated`.

- [ ] **Step 1: Write the failing test**

Create `tests/frameinspect/test-fi-methodlog.c`:

```c
#include <assert.h>
#include <stdio.h>
#include "../../xemu-frameinspect-methodlog.h"

int main(void)
{
    FIMethodLog m;
    assert(fi_methodlog_init(&m));

    uint32_t r0 = fi_methodlog_append(&m, 0x1810, 0, 0x2A, 0x03A4F20C,
                                      41, FI_ORIG_ATTRIBUTED);
    uint32_t r1 = fi_methodlog_append(&m, 0x1814, 0, 0x00, 0x03A4F210,
                                      0, FI_ORIG_UNATTRIBUTED);
    assert(r0 == 0 && r1 == 1 && m.num_recs == 2);
    assert(m.recs[r0].method == 0x1810 && m.recs[r0].param == 0x2A);
    assert(m.recs[r0].phys_addr == 0x03A4F20C && m.recs[r0].writer_node == 41);
    assert(m.recs[r0].confidence == FI_ORIG_ATTRIBUTED);
    assert(m.recs[r1].confidence == FI_ORIG_UNATTRIBUTED);

    /* a batch owns a contiguous record range */
    fi_methodlog_mark_batch(&m, 7, 0, 2);
    assert(m.num_batches == 1);
    assert(m.batches[0].batch_event == 7);
    assert(m.batches[0].first_rec == 0 && m.batches[0].rec_count == 2);

    /* record cap */
    while (m.num_recs < FI_METHODLOG_CAP) {
        fi_methodlog_append(&m, 0, 0, 0, 0, 0, FI_ORIG_UNATTRIBUTED);
    }
    assert(fi_methodlog_append(&m, 1, 0, 0, 0, 0, FI_ORIG_ATTRIBUTED)
           == FI_METHOD_INVALID);
    assert(m.truncated);

    fi_methodlog_free(&m);
    assert(m.recs == NULL);
    printf("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-methodlog.exe tests/frameinspect/test-fi-methodlog.c`
Expected: FAIL — header not found.

- [ ] **Step 3: Write the header**

Create `xemu-frameinspect-methodlog.h` with the GPL-2.0-or-later license block (match `xemu-frameinspect-surfaces.h`), guard `XEMU_FRAMEINSPECT_METHODLOG_H`, and:

```c
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_METHOD_INVALID     0xFFFFFFFFu
#define FI_METHODLOG_CAP       (1u << 22)
#define FI_METHODLOG_BATCH_CAP (1u << 18)

enum {
    FI_ORIG_ATTRIBUTED = 0,
    FI_ORIG_PARTIAL,
    FI_ORIG_UNATTRIBUTED,
    FI_ORIG_LOSTSYNC,
};

typedef struct FIMethodRec {
    uint32_t method;
    uint16_t subchannel;
    uint16_t confidence;
    uint32_t param;
    uint32_t phys_addr;
    uint32_t writer_node;
} FIMethodRec;

typedef struct FIMethodBatch {
    uint32_t batch_event;
    uint32_t first_rec;
    uint32_t rec_count;
} FIMethodBatch;

typedef struct FIMethodLog {
    FIMethodRec *recs;
    uint32_t num_recs, cap_recs;
    FIMethodBatch *batches;
    uint32_t num_batches, cap_batches;
    bool truncated;
} FIMethodLog;

static inline bool fi_methodlog_init(FIMethodLog *m)
{
    memset(m, 0, sizeof(*m));
    m->cap_recs = 65536;
    m->cap_batches = 1024;
    m->recs = (FIMethodRec *)malloc(m->cap_recs * sizeof(FIMethodRec));
    m->batches = (FIMethodBatch *)malloc(m->cap_batches * sizeof(FIMethodBatch));
    if (!m->recs || !m->batches) {
        free(m->recs); free(m->batches);
        memset(m, 0, sizeof(*m));
        return false;
    }
    return true;
}

static inline void fi_methodlog_free(FIMethodLog *m)
{
    free(m->recs); free(m->batches);
    memset(m, 0, sizeof(*m));
    m->recs = NULL;
}

static inline uint32_t fi_methodlog_append(FIMethodLog *m, uint32_t method,
                                           uint16_t subchannel, uint32_t param,
                                           uint32_t phys_addr,
                                           uint32_t writer_node,
                                           uint16_t confidence)
{
    if (m->num_recs >= FI_METHODLOG_CAP) { m->truncated = true; return FI_METHOD_INVALID; }
    if (m->num_recs >= m->cap_recs) {
        uint32_t nc = m->cap_recs * 2;
        if (nc > FI_METHODLOG_CAP) nc = FI_METHODLOG_CAP;
        FIMethodRec *nr = (FIMethodRec *)realloc(m->recs, nc * sizeof(FIMethodRec));
        if (!nr) { m->truncated = true; return FI_METHOD_INVALID; }
        m->recs = nr; m->cap_recs = nc;
    }
    uint32_t idx = m->num_recs++;
    FIMethodRec *r = &m->recs[idx];
    r->method = method; r->subchannel = subchannel; r->param = param;
    r->phys_addr = phys_addr; r->writer_node = writer_node;
    r->confidence = confidence;
    return idx;
}

static inline void fi_methodlog_mark_batch(FIMethodLog *m, uint32_t batch_event,
                                           uint32_t first_rec, uint32_t rec_count)
{
    if (m->num_batches >= FI_METHODLOG_BATCH_CAP) { m->truncated = true; return; }
    if (m->num_batches >= m->cap_batches) {
        uint32_t nc = m->cap_batches * 2;
        if (nc > FI_METHODLOG_BATCH_CAP) nc = FI_METHODLOG_BATCH_CAP;
        FIMethodBatch *nb = (FIMethodBatch *)realloc(m->batches, nc * sizeof(FIMethodBatch));
        if (!nb) { m->truncated = true; return; }
        m->batches = nb; m->cap_batches = nc;
    }
    FIMethodBatch *b = &m->batches[m->num_batches++];
    b->batch_event = batch_event; b->first_rec = first_rec; b->rec_count = rec_count;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-methodlog.exe tests/frameinspect/test-fi-methodlog.c && ./tests/frameinspect/test-fi-methodlog.exe`
Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add xemu-frameinspect-methodlog.h tests/frameinspect/test-fi-methodlog.c
git commit -m "frameinspect: Add method-origin log"
```

---

### Task 2: Capture readback + colour-history wiring

**Files:**
- Modify: `xemu-frameinspect-capture.h` / `.c` (add per-generation colour-history + the diff entry point)
- Modify: `hw/xbox/nv2a/pgraph/gl/surface.c` (capture readback of the colour target as RGBA8888)

**Interfaces:**
- Consumes: `FIColorHist` (Plan 2A), `pgraph_gl_fi_intern_current_color` (Plan 2A Task 7), the static `surface_download_to_buffer` (surface.c:710) via a new capture readback.
- Produces:
  - In the capture module: `void xemu_frameinspect_capture_writer(uint8_t kind, uint32_t surface_gen, const uint32_t *rgba, uint32_t width, uint32_t height)` — the ONE entry point every writer event (batch/clear/blit) calls with the post-writer RGBA8888 image of the affected colour generation. It lazily allocates the per-generation `FIColorHist` (baseline = the first image seen for that generation, then per-event diffs), appends the matching event (`FI_EV_BATCH`/`CLEAR`/`BLIT`) to the event log tagged with `surface_gen`, and feeds the image to that generation's colour history. Budget-checked; on overflow sets truncation.
  - In surface.c: `uint32_t *pgraph_gl_fi_readback_color(NV2AState *d, uint32_t *out_w, uint32_t *out_h)` — returns a freshly `g_malloc`'d RGBA8888 buffer of the current colour binding at surface scale (or NULL if not capturing / no colour binding / zeta-only), reading it via the FBO + `glReadPixels(GL_RGBA, GL_UNSIGNED_BYTE)` path. Caller frees.

Design details for the implementer:
- `FICapture` gains `FIColorHist *hist` (already declared in Plan 2A) sized to `surfaces.num_gens`, grown on demand, indexed by `surface_gen`. Track a parallel `bool *hist_inited` or use `hist[g].width != 0` to detect first-touch (baseline).
- `capture_writer(kind, gen, rgba, w, h)`: if `gen == FI_SURFGEN_INVALID` or not capturing, still append the event (with `surface_gen = gen`) but skip colour history (no pixels — "missing"). Otherwise ensure `hist[gen]` is inited (`fi_colorhist_init` + `fi_colorhist_set_baseline(rgba)` on first touch → this frame's first sight of the generation is its baseline), else `fi_colorhist_add_event(hist[gen], event_index, rgba)`. Use the appended event's index as the colour-history `event_id`.
- The readback function lives in surface.c because `surface_download_to_buffer` is static there. Model it on `render_surface_to_texture_slow` (surface.c:303-325): compute `width/height` scaled by `pgraph_apply_scaling_factor`, but read as canonical RGBA8888. Simplest: bind `surface->gl_buffer` to a temporary FBO (as `surface_download_to_buffer` does at surface.c:708-709) and `glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,buf)`. If reusing `surface_download_to_buffer` is easier, add an RGBA-forcing variant; record which you did.

- [ ] **Step 1: Extend the capture header + module**

Add `xemu_frameinspect_capture_writer(...)` to `xemu-frameinspect-capture.h` and implement it in `.c` (lazy per-gen `FIColorHist`, event append, diff, budget). Grow `fi_cap.hist`/`hist_count` to cover `surfaces.num_gens` as generations appear. Ensure `fi_capture_reset` frees each inited `FIColorHist` (Plan 2A already loops `hist_count`).

- [ ] **Step 2: Add the RGBA readback in surface.c**

Add `pgraph_gl_fi_readback_color` (non-static; prototype in `renderer.h`). Verify the FBO/readback calls against `surface_download_to_buffer` (surface.c:682-763) and `glo_readpixels` usage; record the exact GL calls used.

- [ ] **Step 3: Build**

Run the build script. Expected: capture.c + surface.c compile; both exes link.

- [ ] **Step 4: Commit**

```bash
git add xemu-frameinspect-capture.h xemu-frameinspect-capture.c hw/xbox/nv2a/pgraph/gl/surface.c hw/xbox/nv2a/pgraph/gl/renderer.h
git commit -m "frameinspect: Capture colour-target readback + colour-history wiring"
```

---

### Task 3: Drive colour history from the draw/clear/blit hooks

**Files:**
- Modify: `hw/xbox/nv2a/pgraph/gl/draw.c` (draw_end 333, clear_surface 27)
- Modify: `hw/xbox/nv2a/pgraph/gl/blit.c` (image_blit 72)

**Interfaces:**
- Consumes: `pgraph_gl_fi_readback_color`, `xemu_frameinspect_capture_writer` (Task 2), the Plan-2A `pgraph_gl_fi_intern_current_color`.
- Produces: replaces the Plan-2A skeleton event calls (`begin_batch`/`event`) with the real `capture_writer` flow that both records the event AND captures colour history.

- [ ] **Step 1: draw_end — capture the batch's colour result**

In `pgraph_gl_draw_end` (after `flush_draw` + draw_time updates), replace the Plan-2A `xemu_frameinspect_capture_end_batch()` skeleton call with:

```c
    if (xemu_frameinspect_capture_state() == FI_CAP_CAPTURING) {
        uint32_t gen = pgraph_gl_fi_intern_current_color(d);
        uint32_t w = 0, h = 0;
        uint32_t *rgba = pgraph_gl_fi_readback_color(d, &w, &h);
        xemu_frameinspect_capture_writer(FI_EV_BATCH, gen, rgba, w, h);
        g_free(rgba);
        xemu_frameinspect_capture_end_batch();
    }
```

(Keep `begin_batch` in `draw_begin` for the method-log batch grouping in Task 4. `capture_writer` appends the FI_EV_BATCH event; `begin_batch`'s own event append is now redundant — remove the event append from `begin_batch` in the capture module OR have `begin_batch` only set `open_batch_gen`/record the method-log batch start, and let `capture_writer` own the event. Record which; the event must appear exactly once per batch.)

- [ ] **Step 2: clear_surface + image_blit**

In `pgraph_gl_clear_surface` (after `glClear`), replace the skeleton `capture_event(FI_EV_CLEAR,...)` with the readback+writer flow (`FI_EV_CLEAR`). In `pgraph_gl_image_blit` (end), the destination is a VRAM surface — resolve the destination surface generation (from `surf_dest`/`dest_addr` in blit.c:153) and, if it maps to a tracked colour generation, read it back and call `capture_writer(FI_EV_BLIT, dest_gen, rgba, w, h)`. If the blit destination isn't a GL colour surface (pure VRAM), record the `FI_EV_BLIT` event with `surface_gen = INVALID` and no pixels (documented limitation).

- [ ] **Step 3: Build + commit**

```bash
git add hw/xbox/nv2a/pgraph/gl/draw.c hw/xbox/nv2a/pgraph/gl/blit.c xemu-frameinspect-capture.c
git commit -m "frameinspect: Record colour history per draw/clear/blit writer event"
```

---

### Task 4: PFIFO method-event logging with guest origins

**Files:**
- Modify: `hw/xbox/nv2a/pgraph/pfifo.c` (pusher word loop ~295-346)
- Modify: `xemu-frameinspect-capture.h` / `.c` (method-log storage + append entry point)

**Interfaces:**
- Consumes: `xemu_frameinspect_lookup_tag(uint64_t phys)` (Plan 1), `FIMethodLog` (Task 1), the DMA get/put offsets in the pusher.
- Produces: `void xemu_frameinspect_capture_method(uint32_t method, uint16_t subchannel, uint32_t param, uint64_t phys_addr)` — looks up the writer tag for `phys_addr`, derives confidence from the tag (`FI_TAG_PARTIAL` → PARTIAL; tag 0 → UNATTRIBUTED; else ATTRIBUTED), and appends to the capture's `FIMethodLog`. Gated on `FI_CAP_CAPTURING`. Plus `xemu_frameinspect_capture_method_batch_start()` / `_end(uint32_t batch_event)` so the method log can group records under the batch.

- [ ] **Step 1: Add the method log to the capture module**

Add a `FIMethodLog methods` field to `FICapture` (init in `fi_capture_alloc`, free in `fi_capture_reset`). Implement `xemu_frameinspect_capture_method(...)`: compute confidence from `xemu_frameinspect_lookup_tag(phys)` (`tag==0`→UNATTRIBUTED, `tag & FI_TAG_PARTIAL`→PARTIAL else ATTRIBUTED; writer_node = `FI_TAG_NODE(tag)` when attributed), `fi_methodlog_append`. Track a running "batch first record" so `begin_batch`/`draw_end` can `fi_methodlog_mark_batch`.

- [ ] **Step 2: Hook the pusher**

In `pfifo_run_pusher` (pfifo.c), the pusher reads each command word from `dma + dma_get_v` (pfifo.c:309-311). For each method word actually dispatched to `pgraph_method`, compute its guest physical address (the DMA object base + `dma_get_v`) and call `xemu_frameinspect_capture_method(method, subchannel, param, phys)` — **before** PGRAPH lookahead can squash it (log the raw decoded words). Verify the exact spot where `method`/`subchannel`/`param` are known and the source offset is still `dma_get_v` for that word; the physical base is the mapped `dma` pointer minus `d->vram_ptr` (or the DMA object's frame base). Record the exact address computation used. Gate the whole thing on `xemu_frameinspect_capture_state() == FI_CAP_CAPTURING` so disarmed traffic is untouched.

- [ ] **Step 3: Build + commit**

```bash
git add hw/xbox/nv2a/pgraph/pfifo.c xemu-frameinspect-capture.h xemu-frameinspect-capture.c
git commit -m "frameinspect: Log PFIFO method events with guest-code origins"
```

---

### Task 5: Per-draw register + resource snapshots

**Files:**
- Modify: `hw/xbox/nv2a/pgraph/gl/draw.c` (draw_begin/draw_end)
- Modify: `xemu-frameinspect-capture.h` / `.c` (resource-ref list per batch)

**Interfaces:**
- Consumes: `FIResourcePool` (Plan 2A) via a new `xemu_frameinspect_capture_resource(uint32_t kind, const void *data, uint32_t len, uint64_t meta)` → resource id; `pg->regs_` (the `uint32_t[0x2000]` register file, pgraph.h:240); the bound-texture info in the GL draw path (`gl/texture.c` — texture_vram_offset, length, palette).
- Produces: each `FI_EV_BATCH` event carries a small list of resource ids: the register snapshot (kind=REGS), and the consumed textures/palettes (kind=TEXTURE/PALETTE). Resource kinds enum: `FI_RESK_REGS, FI_RESK_TEXTURE, FI_RESK_PALETTE, FI_RESK_VERTEX, FI_RESK_INDEX, FI_RESK_SHADER`.

- [ ] **Step 1: Add resource interning + per-batch ref list to the capture module**

`xemu_frameinspect_capture_resource(kind, data, len, meta)` = `fi_resources_intern(&fi_cap.resources, kind, data, len, meta)` gated on capturing. Store per-batch resource ids in a small growable side array keyed by batch event index (or an `a1..a3` slot on the FI_EV_BATCH event for the first few + an overflow list). Keep it simple: a `FICaptureBatchRes { uint32_t event; uint32_t res_ids[8]; uint8_t n; }` growable array; the UI joins batch→resources by event index.

- [ ] **Step 2: Snapshot registers + textures at draw time**

In `pgraph_gl_draw_begin` (or draw_end, after bindings resolved), when capturing: intern `pg->regs_` (`sizeof(pg->regs_)` bytes, kind=REGS, meta=0) and the currently-bound textures. For each active texture unit, if it is a normal guest texture, intern its bytes from `d->vram_ptr + texture_vram_offset` (length from the texture path; palette separately, kind=PALETTE); if it is a **render-target-backed** texture (`surf_to_tex` in gl/texture.c), do NOT read guest RAM — intern a zero-length REGS-style dependency marker recording the producing surface generation in `meta` (kind=TEXTURE, len=0, meta=producing surface addr) so the UI can link it. Record the exact texture-length/offset symbols used (they were seen in `gl/texture.c:319-323` as `texture_vram_offset`, `length`, `palette_vram_offset`, `palette_length`).

- [ ] **Step 3: Build + commit**

```bash
git add hw/xbox/nv2a/pgraph/gl/draw.c xemu-frameinspect-capture.h xemu-frameinspect-capture.c
git commit -m "frameinspect: Snapshot registers + textures per draw"
```

---

### Task 6: Scanout event

**Files:**
- Modify: `hw/xbox/nv2a/pgraph/gl/display.c` (`pgraph_gl_sync` 375-404)
- Modify: `xemu-frameinspect-capture.h` / `.c` (scanout fields)

**Interfaces:**
- Consumes: the display surface chosen in `pgraph_gl_sync` (surface.c `pgraph_gl_surface_get_within(d, d->pcrtc.start + line_offset)`, display.c:380), `d->pcrtc.start`, the pvideo overlay state (`d->pvideo.regs[NV_PVIDEO_BUFFER]`, display.c:179-180).
- Produces: `void xemu_frameinspect_capture_scanout(uint32_t src_surface_gen, uint32_t pcrtc_start, uint32_t line_offset, uint32_t flags, const uint32_t *rgba, uint32_t w, uint32_t h)` appending an `FI_EV_SCANOUT` event and storing the final displayed image (RGBA8888) as a resource (kind=SCANOUT) + the PCRTC/pvideo state on the event's `a0..a3`.

- [ ] **Step 1: Add the scanout entry point + capture the display image**

In `pgraph_gl_sync` (display.c), when a capture just completed this frame (state transitioning to DONE — call this from the flip path OR guard on `capture_state()==FI_CAP_CAPTURING` at the last sync before publish), read back the chosen display `surface` as RGBA8888 (reuse `pgraph_gl_fi_readback_color`-style logic against the display surface), resolve its surface generation, and call `xemu_frameinspect_capture_scanout(gen, d->pcrtc.start, line_offset, pvideo_active?FI_SCANOUT_PVIDEO:0, rgba, w, h)`. Mark pvideo/overlay regions unsupported in `flags`. Record the exact fields for `line_offset` and the pvideo-active test.

Note: the scanout must be recorded for the CAPTURING frame before `on_flip` publishes. If the timing is awkward (sync happens on a different pass than flip_stall), record the scanout inside `capture_on_flip`'s CAPTURING→DONE branch by having the capture module pull the last-known display surface — record whichever integration point you used and why.

- [ ] **Step 2: Build + commit**

```bash
git add hw/xbox/nv2a/pgraph/gl/display.c xemu-frameinspect-capture.h xemu-frameinspect-capture.c
git commit -m "frameinspect: Capture scanout event (displayed image + PCRTC/pvideo state)"
```

---

### Task 7: Budget wiring, truncation roll-up, atomicity, and capture-cancel

**Files:**
- Modify: `xemu-frameinspect-capture.c` (budget, truncation, `fi_state` atomic, cancel)
- Modify: `xemu-frameinspect-eventlog.h` (budget overflow check)
- Modify: `ui/xemu.c` (SDL_SCANCODE_I re-press cancels)

**Interfaces:**
- Consumes: everything above.
- Produces: budget-enforced capture; complete truncation reporting; race-free `fi_state`; a user cancel path.

- [ ] **Step 1: Fix the budget overflow check (F-carryover)**

In `xemu-frameinspect-eventlog.h`, change `fi_budget_try` to overflow-safe form: `if (bytes > b->limit - b->used) return false; b->used += bytes; return true;` (the invariant `used <= limit` holds). Re-run the eventlog test (`/c/msys64/ucrt64/bin/gcc.exe -O2 -Wall -o tests/frameinspect/test-fi-eventlog.exe tests/frameinspect/test-fi-eventlog.c && ./tests/frameinspect/test-fi-eventlog.exe` → PASS).

- [ ] **Step 2: Enforce budget + roll up truncation**

In the capture module, charge the budget in `capture_writer` (colour deltas + keyframes bytes), `capture_resource` (blob len), and `capture_method` (record size) — `fi_budget_try` before each store; on refusal set the relevant store's truncation and stop feeding that store (never abort). Set `FICapture.truncated = surfaces.truncated || events.truncated || resources.truncated || methods.truncated || (any hist[g].truncated) || budget-exhausted`. Update `capture_summary` to report each truncated store by name.

- [ ] **Step 3: Make `fi_state` cross-thread-safe (F1)**

Route all `fi_state` reads/writes through `qatomic_read`/`qatomic_set` (it is set on the UI thread in `capture_arm`/cancel and read on the pfifo thread everywhere else). This documents and hardens the cross-thread access, matching the existing `fi_published` treatment.

- [ ] **Step 4: Add capture-cancel (F2)**

Add `void xemu_frameinspect_capture_cancel(void)`: if state is `ARMED` or `CAPTURING`, free the in-progress `fi_cap`, set state `IDLE`, and `xemu_frameinspect_disarm()` (end the Plan-1 lead-in). In `ui/xemu.c`'s `SDL_SCANCODE_I` case, if `capture_state()` is `ARMED`/`CAPTURING`, call cancel + toast "Frame capture cancelled"; else arm as before.

- [ ] **Step 5: Remove dead fields (F6)**

Remove `fi_cap_ram_size` (capture.c — written, never read) and `has_baseline` (colorhist.h — written, never read; `reconstruct` uses `after_event==0`). Re-run the colorhist test → PASS.

- [ ] **Step 6: Build + commit**

```bash
git add xemu-frameinspect-capture.c xemu-frameinspect-eventlog.h xemu-frameinspect-colorhist.h ui/xemu.c
git commit -m "frameinspect: Budget enforcement, truncation roll-up, fi_state atomicity, capture-cancel"
```

- [ ] **Step 7: Manual integration check (deferred to human — list in report)**

Do NOT run (needs a live game); list for the human:
1. Capture a menu; the pause toast now reads e.g. `Captured frame: N events, M surfaces` (unchanged), but the published capture now holds colour history, method log, and resources (verified in Plan 3's UI, or via a temporary debug dump if desired).
2. Confirm no regression: game runs full-speed when idle; capture completes and pauses; cancel (Ctrl+Alt+I while capturing) aborts cleanly and resumes instrumentation-off.
3. Confirm heavy frames hit caps gracefully (truncation banner in the summary), not a crash.

---

## Self-review checklist (run after writing, before execution)

- **Spec coverage:** colour history readback+diff ✓ (T2/T3), method-origin logging ✓ (T4), register+resource snapshots ✓ (T5), scanout ✓ (T6), budget+truncation+atomicity+cancel ✓ (T7). RT-backed-texture dependency handling ✓ (T5 Step 2). Guarantee-model confidence states ✓ (T1/T4).
- **Carry-over findings:** F1 atomic (T7.3), F2 cancel (T7.4), F5 truncation roll-up (T7.2), F6 dead fields (T7.5), budget overflow (T7.1), F4 open_batch pairing (addressed by making `capture_writer` own the single batch event, T3.1).
- **Placeholder scan:** T1 is complete TDD code; integration tasks give exact sites + representative code with explicit "record the exact symbol" verification points (implementer confirms field/offset names against the tree, as in Plan 2A Task 7).
- **Ambiguity:** the scanout timing (T6) and the "batch event appears exactly once" ownership (T3.1) are the two spots called out for the implementer to resolve-and-record.

## Out of scope (v1 — matches the spec's deferrals)

- Integer writer-ID / last-visible-fragment coverage sidecar.
- PVIDEO/software-VGA attribution (recorded + flagged unsupported).
- Exact sampled-texel ancestry across RT hops (dependency edge only).
- Vulkan/null renderers.
- The inspector UI itself (Plan 3).

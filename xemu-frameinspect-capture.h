/*
 * xemu frame inspector: capture state machine + immutable publish
 *
 * Owns the one-shot capture state machine (arm -> capturing -> done) and
 * the immutable capture snapshot published for the UI thread to read.
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
#ifndef XEMU_FRAMEINSPECT_CAPTURE_H
#define XEMU_FRAMEINSPECT_CAPTURE_H

#include "xemu-frameinspect-surfaces.h"
#include "xemu-frameinspect-colorhist.h"
#include "xemu-frameinspect-resources.h"
#include "xemu-frameinspect-eventlog.h"
#include "xemu-frameinspect-methodlog.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FI_CAP_BUDGET_DEFAULT (1ull << 30)

typedef enum {
    FI_CAP_IDLE,
    FI_CAP_ARMING,
    FI_CAP_ARMED,
    FI_CAP_CAPTURING,
    FI_CAP_PAUSE_PENDING,
    FI_CAP_DONE,
} FICaptureState;

typedef enum {
    FI_CAP_FLIP_NONE,
    FI_CAP_FLIP_COMPLETE,
    FI_CAP_FLIP_FAILED,
} FICaptureFlipResult;

/* Resource kinds recorded via xemu_frameinspect_capture_resource(). RTREF is
 * a dependency reference to a rendered surface (not guest RAM): len is 0 and
 * meta is the surface's vram addr, since the surface's RAM bytes are
 * stale/invalid while it's a render target. */
enum {
    FI_RESK_REGS = 1,
    FI_RESK_TEXTURE,
    FI_RESK_PALETTE,
    FI_RESK_TEXTURE_RTREF,
};

/* One batch->resource reference: `event` is the FI_EV_BATCH event index the
 * resource belongs to, `res_id` indexes FICapture.resources. */
typedef struct FIBatchResRef {
    uint32_t event;
    uint32_t res_id;
} FIBatchResRef;

typedef struct FICapture {
    FISurfaceStore surfaces;
    FIResourcePool resources;
    FIEventLog events;
    FIMethodLog methods;
    FIBudget budget;
    uint64_t methods_bytes;  /* total budget charged for `methods`, released
                              * in full on reset (init alloc + per-call
                              * growth charges, symmetric with fi_budget_try) */
    FIColorHist *hist;        /* [surfaces.num_gens], allocated lazily in 2B */
    uint32_t hist_count;
    uint32_t open_batch_gen;  /* surface gen of the batch currently open, or INVALID */
    uint32_t open_batch_zeta_gen;
    uint32_t open_batch_event; /* event idx begin_batch appended for the
                                * currently open batch, or FI_EVENT_INVALID;
                                * used (instead of last_event) to mark the
                                * batch's method-log range, since a clear/blit
                                * inside the batch can move last_event. */
    uint32_t batch_first_rec; /* methods.num_recs when the open batch began */
    uint32_t last_event;      /* index of the most-recently appended event
                               * (batch/clear/blit), or FI_EVENT_INVALID;
                               * used by attach_pixels() to find the event
                               * to feed the colour history for. */
    FIBatchResRef *batch_res; /* growable [cap_batch_res], batch->resource
                               * references appended by
                               * capture_batch_resource_ref() */
    uint32_t num_batch_res, cap_batch_res;
    uint64_t batch_res_bytes; /* total budget charged for `batch_res`,
                               * released in full on reset, symmetric with
                               * fi_budget_try (like methods_bytes) */
    uint32_t refcount;        /* protected by the capture-module lock */
    bool batch_open;
    bool truncated;
} FICapture;

/* Arm a one-shot capture: begins the lead-in (Plan-1 instrumentation on). */
bool xemu_frameinspect_capture_arm(uint64_t ram_size);
/* Called on the pfifo thread at each NV097_FLIP_STALL. */
FICaptureFlipResult xemu_frameinspect_capture_on_flip(bool opengl_active);
bool xemu_frameinspect_capture_pause_complete(void);
bool xemu_frameinspect_capture_cancel(void);
void xemu_frameinspect_capture_shutdown(void);
FICaptureState xemu_frameinspect_capture_state(void);
/* Skeleton event recorders (Task 7 calls these; 2B extends them). */
bool xemu_frameinspect_capture_begin_batch(uint32_t surface_gen,
                                           uint32_t zeta_surface_gen);
void xemu_frameinspect_capture_end_batch(void);
void xemu_frameinspect_capture_split_batch(void);
void xemu_frameinspect_capture_clear(uint32_t surface_gen,
                                     uint32_t zeta_surface_gen,
                                     uint32_t parameter);
void xemu_frameinspect_capture_blit(uint32_t surface_gen, uint32_t source_addr,
                                    uint32_t dest_addr, uint32_t size,
                                    uint32_t operation);
/* Flag bits for xemu_frameinspect_capture_scanout()'s `flags` parameter. */
#define FI_SCANOUT_PVIDEO 0x1u /* PVIDEO overlay was active at scanout time.
                                * Known v1 limitation: the overlay's content
                                * is NOT captured or attributed anywhere in
                                * the capture; this bit only records that it
                                * was on screen. */
/* Record the scanout event: which surface was actually displayed, its final
 * pixels, and the PCRTC/PVIDEO state that produced it. Intended to be called
 * once, as the last event of a captured frame (see
 * pgraph_gl_fi_capture_scanout() in the GL renderer, called from
 * NV097_FLIP_STALL before the capture module's own on_flip publish runs).
 * `surface_gen` may be FI_SURFGEN_INVALID and `rgba` may be NULL if the
 * display surface could not be resolved -- the event is still recorded
 * (missing, never wrong). The appended FI_EV_SCANOUT event's a0/a1/a2 are
 * pcrtc_start/line_offset/flags; a3 is unused (0). No-op unless capturing.
 * Caller retains ownership of rgba. */
void xemu_frameinspect_capture_scanout(uint32_t surface_gen,
                                       uint32_t pcrtc_start,
                                       uint32_t line_offset, uint32_t flags,
                                       const uint32_t *rgba, uint32_t width,
                                       uint32_t height);
uint32_t xemu_frameinspect_capture_intern_surface(const FISurfaceKey *k);
/* The ONE entry point every writer event (batch/clear/blit) calls with the
 * post-writer RGBA8888 image of the affected colour generation. Appends the
 * matching event (tagged with surface_gen) and feeds the image to that
 * generation's colour history (lazily allocated; the first image seen for a
 * generation becomes its baseline). No-op unless capturing. If surface_gen
 * is invalid or rgba is NULL, the event is still recorded but colour history
 * is skipped (missing data, never wrong data). Caller retains ownership of
 * rgba. */
void xemu_frameinspect_capture_writer(uint8_t kind, uint32_t surface_gen,
                                      const uint32_t *rgba, uint32_t width,
                                      uint32_t height);
/* Attach a readback image to the most-recently appended event (the one left
 * by begin_batch/clear/blit) without appending a new event of its own. Feeds
 * the image to surface_gen's colour history the same way capture_writer()
 * does. No-op unless capturing, or if there is no pending event, or if
 * surface_gen is invalid, or if rgba is NULL (missing data, never wrong
 * data). Caller retains ownership of rgba. */
void xemu_frameinspect_capture_attach_pixels(uint32_t surface_gen,
                                             const uint32_t *rgba,
                                             uint32_t width, uint32_t height);
/* Log a burst of pushbuffer method words dispatched by the PFIFO pusher in
 * one pfifo_run_puller() call, with each word's guest-code origin looked up
 * from the Plan-1 RAM-wide tag map. `first_method` is the method of word 0;
 * subsequent words are at first_method + 4*i if method_inc, else all at
 * first_method (non-incrementing method type). `phys_base` is the guest
 * physical (vram-relative) address of word 0; word i is at phys_base + 4*i.
 * Words 0..n_labeled-1 are parameters of first_method; words n_labeled..n-1
 * are extra dwords consumed by pgraph_method's lookahead squash (raw
 * pushbuffer command headers/params belonging to later commands, not
 * parameters of first_method) and are logged as FI_METHOD_RAW_WORD instead
 * of being mislabeled with fabricated method/param values. No-op unless
 * capturing (lock-free fast-path). */
void xemu_frameinspect_capture_methods(uint32_t first_method, bool method_inc,
                                       uint16_t subchannel,
                                       const uint32_t *words, uint32_t n,
                                       uint32_t n_labeled,
                                       uint64_t phys_base);
/* Intern a content-hash-deduplicated resource snapshot (register file,
 * texture, palette, or an RTREF dependency reference) into the capture's
 * resource pool. Returns its id, or FI_RES_INVALID if not capturing or the
 * pool is full/truncated. No-op (returns FI_RES_INVALID) unless capturing
 * (lock-free fast-path). */
uint32_t xemu_frameinspect_capture_resource(uint32_t kind, const void *data,
                                            uint32_t len, uint64_t meta);
/* Reference a resource (by id, as returned by capture_resource()) from the
 * currently open batch, so the UI can join batch -> resources. No-op unless
 * capturing, no batch is open, or res_id is FI_RES_INVALID (lock-free
 * fast-path). */
void xemu_frameinspect_capture_batch_resource_ref(uint32_t res_id);
/* Published immutable capture for the UI (Plan 3); caller must release it. */
const FICapture *xemu_frameinspect_capture_acquire(void);
void xemu_frameinspect_capture_release(const FICapture *capture);
/* Human-readable one-line summary of the published capture (g_strdup'd). */
char *xemu_frameinspect_capture_summary(void);

#ifdef __cplusplus
}
#endif

#endif

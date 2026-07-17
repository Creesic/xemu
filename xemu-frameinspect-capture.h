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

typedef struct FICapture {
    FISurfaceStore surfaces;
    FIResourcePool resources;
    FIEventLog events;
    FIBudget budget;
    FIColorHist *hist;        /* [surfaces.num_gens], allocated lazily in 2B */
    uint32_t hist_count;
    uint32_t open_batch_gen;  /* surface gen of the batch currently open, or INVALID */
    uint32_t open_batch_zeta_gen;
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
/* Published immutable capture for the UI (Plan 3); caller must release it. */
const FICapture *xemu_frameinspect_capture_acquire(void);
void xemu_frameinspect_capture_release(const FICapture *capture);
/* Human-readable one-line summary of the published capture (g_strdup'd). */
char *xemu_frameinspect_capture_summary(void);

#ifdef __cplusplus
}
#endif

#endif

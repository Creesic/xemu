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

void xemu_frameinspect_capture_writer(uint8_t kind, uint32_t surface_gen,
                                      const uint32_t *rgba, uint32_t width,
                                      uint32_t height)
{
    if (fi_state != FI_CAP_CAPTURING) return;

    uint32_t idx = fi_eventlog_append(&fi_cap.events, kind, surface_gen,
                                      0, 0, 0, 0);
    if (surface_gen == FI_SURFGEN_INVALID || !rgba || idx == FI_EVENT_INVALID) {
        /* Event recorded (if it fit); no pixels to diff -> "missing", never
         * wrong. */
        return;
    }

    if (surface_gen >= fi_cap.hist_count) {
        uint32_t new_count = surface_gen + 1;
        FIColorHist *nh = (FIColorHist *)realloc(
            fi_cap.hist, new_count * sizeof(FIColorHist));
        if (!nh) {
            fi_cap.truncated = true;
            return;
        }
        memset(&nh[fi_cap.hist_count], 0,
              (new_count - fi_cap.hist_count) * sizeof(FIColorHist));
        fi_cap.hist = nh;
        fi_cap.hist_count = new_count;
    }

    FIColorHist *ch = &fi_cap.hist[surface_gen];
    if (ch->width == 0) {
        /* First sight of this generation this frame: it becomes the
         * baseline for its colour history. */
        if (!fi_colorhist_init(ch, width, height, 16) ||
            !fi_colorhist_set_baseline(ch, rgba)) {
            fi_cap.truncated = true;
        }
    } else if (ch->width != width || ch->height != height) {
        /* Dimensions changed mid-capture for this generation (e.g. a
         * rebind that reused the gen id): skip the diff rather than risk
         * corrupting the history. Missing, not wrong. */
    } else if (!fi_colorhist_add_event(ch, idx, rgba)) {
        fi_cap.truncated = true;
    }
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

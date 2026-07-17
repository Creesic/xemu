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
#include "qemu/thread.h"
#include "xemu-frameinspect.h"
#include "xemu-frameinspect-capture.h"

static FICapture fi_cap;                 /* capture in progress */
static FICaptureState fi_state = FI_CAP_IDLE;
static FICapture *fi_published;          /* immutable, protected by fi_lock */
static QemuMutex fi_lock;
static int fi_lock_state;

static void fi_capture_sync_init(void)
{
    if (qatomic_load_acquire(&fi_lock_state) == 2) {
        return;
    }
    if (qatomic_cmpxchg(&fi_lock_state, 0, 1) == 0) {
        qemu_mutex_init(&fi_lock);
        qatomic_store_release(&fi_lock_state, 2);
        return;
    }
    while (qatomic_load_acquire(&fi_lock_state) != 2) {
        cpu_relax();
    }
}

static void fi_capture_reset(FICapture *c)
{
    for (uint32_t i = 0; i < c->hist_count; i++) {
        fi_budget_release(&c->budget, c->hist[i].bytes_used);
        fi_colorhist_free(&c->hist[i]);
    }
    if (c->hist) {
        fi_budget_release(&c->budget,
                          (uint64_t)c->hist_count * sizeof(FIColorHist));
    }
    free(c->hist);
    if (c->surfaces.gens) {
        fi_budget_release(&c->budget,
                          (uint64_t)FI_SURF_MAX_GENS * sizeof(FISurfaceGen));
    }
    fi_surfaces_free(&c->surfaces);
    fi_resources_free(&c->resources, &c->budget);
    fi_eventlog_free(&c->events, &c->budget);
    memset(c, 0, sizeof(*c));
}

static void fi_capture_destroy(FICapture *c)
{
    fi_capture_reset(c);
    free(c);
}

static bool fi_capture_alloc(FICapture *c)
{
    memset(c, 0, sizeof(*c));
    c->open_batch_gen = FI_SURFGEN_INVALID;
    c->open_batch_zeta_gen = FI_SURFGEN_INVALID;
    c->last_event = FI_EVENT_INVALID;
    c->budget.limit = FI_CAP_BUDGET_DEFAULT;
    uint64_t surface_bytes =
        (uint64_t)FI_SURF_MAX_GENS * sizeof(FISurfaceGen);
    if (!fi_budget_try(&c->budget, surface_bytes) ||
        !fi_surfaces_init(&c->surfaces) ||
        !fi_resources_init(&c->resources, &c->budget) ||
        !fi_eventlog_init(&c->events, &c->budget)) {
        fi_capture_reset(c);
        return false;
    }
    return true;
}

bool xemu_frameinspect_capture_arm(uint64_t ram_size)
{
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (qatomic_read(&fi_state) != FI_CAP_IDLE &&
        qatomic_read(&fi_state) != FI_CAP_DONE) {
        qemu_mutex_unlock(&fi_lock);
        return false;
    }
    qatomic_set(&fi_state, FI_CAP_ARMING);
    /* Lead-in: enable Plan-1 guest instrumentation now so writes during the
     * frame before the captured frame are tagged. */
    xemu_frameinspect_arm(ram_size);
    bool armed = xemu_frameinspect_is_armed();

    if (qatomic_read(&fi_state) == FI_CAP_ARMING && armed) {
        qatomic_set(&fi_state, FI_CAP_ARMED);
        qemu_mutex_unlock(&fi_lock);
        return true;
    }
    qatomic_set(&fi_state, FI_CAP_IDLE);
    if (armed) {
        xemu_frameinspect_disarm();
    }
    qemu_mutex_unlock(&fi_lock);
    return false;
}

FICaptureFlipResult xemu_frameinspect_capture_on_flip(bool opengl_active)
{
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (!opengl_active &&
        (qatomic_read(&fi_state) == FI_CAP_ARMED ||
         qatomic_read(&fi_state) == FI_CAP_CAPTURING)) {
        if (qatomic_read(&fi_state) == FI_CAP_CAPTURING) {
            fi_capture_reset(&fi_cap);
        }
        qatomic_set(&fi_state, FI_CAP_IDLE);
        xemu_frameinspect_disarm();
        qemu_mutex_unlock(&fi_lock);
        return FI_CAP_FLIP_FAILED;
    }
    switch (qatomic_read(&fi_state)) {
    case FI_CAP_ARMED:
        /* First flip after arming: the captured frame starts now. */
        if (!fi_capture_alloc(&fi_cap)) {
            qatomic_set(&fi_state, FI_CAP_IDLE);
            xemu_frameinspect_disarm();
            qemu_mutex_unlock(&fi_lock);
            return FI_CAP_FLIP_FAILED;
        }
        qatomic_set(&fi_state, FI_CAP_CAPTURING);
        qemu_mutex_unlock(&fi_lock);
        return FI_CAP_FLIP_NONE;
    case FI_CAP_CAPTURING: {
        /* Second flip: the captured frame ended. Finalize + publish. */
        FICapture *done = (FICapture *)malloc(sizeof(FICapture));
        if (!done) {
            fi_capture_reset(&fi_cap);
            qatomic_set(&fi_state, FI_CAP_IDLE);
            xemu_frameinspect_disarm();
            qemu_mutex_unlock(&fi_lock);
            return FI_CAP_FLIP_FAILED;
        }
        *done = fi_cap;                 /* transfer ownership of buffers */
        memset(&fi_cap, 0, sizeof(fi_cap));
        done->refcount = 1;             /* published-pointer ownership */
        FICapture *old = fi_published;
        fi_published = done;
        bool destroy_old = old && --old->refcount == 0;
        qatomic_set(&fi_state, FI_CAP_PAUSE_PENDING);
        xemu_frameinspect_disarm();     /* end lead-in instrumentation */
        qemu_mutex_unlock(&fi_lock);
        if (destroy_old) {
            fi_capture_destroy(old);
        }
        return FI_CAP_FLIP_COMPLETE;
    }
    default:
        qemu_mutex_unlock(&fi_lock);
        return FI_CAP_FLIP_NONE;
    }
}

bool xemu_frameinspect_capture_pause_complete(void)
{
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    bool pending = qatomic_read(&fi_state) == FI_CAP_PAUSE_PENDING;
    if (qatomic_read(&fi_state) == FI_CAP_PAUSE_PENDING) {
        qatomic_set(&fi_state, FI_CAP_DONE);
    }
    qemu_mutex_unlock(&fi_lock);
    return pending;
}

bool xemu_frameinspect_capture_cancel(void)
{
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    bool active = qatomic_read(&fi_state) == FI_CAP_ARMING ||
                  qatomic_read(&fi_state) == FI_CAP_ARMED ||
                  qatomic_read(&fi_state) == FI_CAP_CAPTURING ||
                  qatomic_read(&fi_state) == FI_CAP_PAUSE_PENDING;
    if (qatomic_read(&fi_state) == FI_CAP_CAPTURING) {
        fi_capture_reset(&fi_cap);
    }
    if (active) {
        qatomic_set(&fi_state, FI_CAP_IDLE);
        xemu_frameinspect_disarm();
    }
    qemu_mutex_unlock(&fi_lock);
    return active;
}

void xemu_frameinspect_capture_shutdown(void)
{
    xemu_frameinspect_capture_cancel();
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    FICapture *published = fi_published;
    fi_published = NULL;
    bool destroy = published && --published->refcount == 0;
    qatomic_set(&fi_state, FI_CAP_IDLE);
    qemu_mutex_unlock(&fi_lock);
    if (destroy) {
        fi_capture_destroy(published);
    }
}

FICaptureState xemu_frameinspect_capture_state(void)
{
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    FICaptureState state = qatomic_read(&fi_state);
    qemu_mutex_unlock(&fi_lock);
    return state;
}

uint32_t xemu_frameinspect_capture_intern_surface(const FISurfaceKey *k)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        return FI_SURFGEN_INVALID;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    uint32_t id = qatomic_read(&fi_state) == FI_CAP_CAPTURING && k
                      ? fi_surfaces_intern(&fi_cap.surfaces, k)
                      : FI_SURFGEN_INVALID;
    qemu_mutex_unlock(&fi_lock);
    return id;
}

bool xemu_frameinspect_capture_begin_batch(uint32_t surface_gen,
                                           uint32_t zeta_surface_gen)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        return false;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        qemu_mutex_unlock(&fi_lock);
        return false;
    }
    fi_cap.open_batch_gen = surface_gen;
    fi_cap.open_batch_zeta_gen = zeta_surface_gen;
    fi_cap.batch_open = true;
    uint32_t idx = fi_eventlog_append(&fi_cap.events, &fi_cap.budget,
                                      FI_EV_BATCH, surface_gen,
                                      zeta_surface_gen, 0, 0, 0);
    bool appended = idx != FI_EVENT_INVALID;
    if (appended) {
        fi_cap.last_event = idx;
    }
    qemu_mutex_unlock(&fi_lock);
    return appended;
}

void xemu_frameinspect_capture_end_batch(void)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        return;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (qatomic_read(&fi_state) == FI_CAP_CAPTURING) {
        fi_cap.open_batch_gen = FI_SURFGEN_INVALID;
        fi_cap.open_batch_zeta_gen = FI_SURFGEN_INVALID;
        fi_cap.batch_open = false;
    }
    qemu_mutex_unlock(&fi_lock);
}

void xemu_frameinspect_capture_split_batch(void)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        return;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (qatomic_read(&fi_state) == FI_CAP_CAPTURING && fi_cap.batch_open) {
        fi_eventlog_append(&fi_cap.events, &fi_cap.budget, FI_EV_BATCH,
                           fi_cap.open_batch_gen,
                           fi_cap.open_batch_zeta_gen, 0, 0, 0);
    }
    qemu_mutex_unlock(&fi_lock);
}

void xemu_frameinspect_capture_clear(uint32_t surface_gen,
                                     uint32_t zeta_surface_gen,
                                     uint32_t parameter)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        return;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (qatomic_read(&fi_state) == FI_CAP_CAPTURING) {
        uint32_t idx = fi_eventlog_append(&fi_cap.events, &fi_cap.budget,
                                          FI_EV_CLEAR, surface_gen,
                                          zeta_surface_gen, parameter, 0, 0);
        if (idx != FI_EVENT_INVALID) {
            fi_cap.last_event = idx;
        }
    }
    qemu_mutex_unlock(&fi_lock);
}

void xemu_frameinspect_capture_blit(uint32_t surface_gen, uint32_t source_addr,
                                    uint32_t dest_addr, uint32_t size,
                                    uint32_t operation)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        return;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (qatomic_read(&fi_state) == FI_CAP_CAPTURING) {
        uint32_t idx = fi_eventlog_append(&fi_cap.events, &fi_cap.budget,
                                          FI_EV_BLIT, surface_gen, source_addr,
                                          dest_addr, size, operation);
        if (idx != FI_EVENT_INVALID) {
            fi_cap.last_event = idx;
        }
    }
    qemu_mutex_unlock(&fi_lock);
}

/* Lazy-alloc + baseline/add_event + budget logic shared by capture_writer()
 * and attach_pixels(). Feeds `rgba` into surface_gen's colour history,
 * tagging the diff with event_idx. Caller must hold fi_lock and must have
 * already verified we are FI_CAP_CAPTURING, surface_gen is valid, and rgba
 * is non-NULL. */
static void fi_feed_colorhist(uint32_t surface_gen, uint32_t event_idx,
                              const uint32_t *rgba, uint32_t width,
                              uint32_t height)
{
    if (surface_gen >= fi_cap.hist_count) {
        uint32_t new_count = surface_gen + 1;
        uint64_t old_bytes = (uint64_t)fi_cap.hist_count * sizeof(FIColorHist);
        uint64_t new_bytes = (uint64_t)new_count * sizeof(FIColorHist);
        if (!fi_budget_try(&fi_cap.budget, new_bytes - old_bytes)) {
            fi_cap.truncated = true;
            return;
        }
        FIColorHist *nh = (FIColorHist *)realloc(
            fi_cap.hist, (size_t)new_bytes);
        if (!nh) {
            fi_budget_release(&fi_cap.budget, new_bytes - old_bytes);
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
        uint64_t remaining = fi_cap.budget.limit - fi_cap.budget.used;
        if (!fi_colorhist_init(ch, width, height, 16) ||
            ch->bytes_used > remaining) {
            fi_colorhist_free(ch);
            fi_cap.truncated = true;
            return;
        }
        fi_budget_try(&fi_cap.budget, ch->bytes_used);
        remaining = fi_cap.budget.limit - fi_cap.budget.used;
        ch->byte_budget = MIN(ch->byte_budget, ch->bytes_used + remaining);
        uint64_t old_bytes = ch->bytes_used;
        if (!fi_colorhist_set_baseline(ch, rgba)) {
            fi_cap.truncated = true;
        }
        fi_budget_try(&fi_cap.budget, ch->bytes_used - old_bytes);
    } else if (ch->width != width || ch->height != height) {
        /* Dimensions changed mid-capture for this generation: skip the diff
         * rather than corrupting the history. Missing, not wrong. */
        fi_cap.truncated = true;
    } else {
        uint64_t remaining = fi_cap.budget.limit - fi_cap.budget.used;
        ch->byte_budget = MIN((uint64_t)FI_CH_BYTE_BUDGET,
                              ch->bytes_used + remaining);
        uint64_t old_bytes = ch->bytes_used;
        if (!fi_colorhist_add_event(ch, event_idx, rgba)) {
            fi_cap.truncated = true;
        }
        fi_budget_try(&fi_cap.budget, ch->bytes_used - old_bytes);
    }
}

void xemu_frameinspect_capture_writer(uint8_t kind, uint32_t surface_gen,
                                      const uint32_t *rgba, uint32_t width,
                                      uint32_t height)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        return;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        qemu_mutex_unlock(&fi_lock);
        return;
    }

    uint32_t idx = fi_eventlog_append(&fi_cap.events, &fi_cap.budget, kind,
                                      surface_gen, 0, 0, 0, 0);
    if (surface_gen == FI_SURFGEN_INVALID || !rgba || idx == FI_EVENT_INVALID) {
        /* Event recorded (if it fit); no pixels to diff -> "missing", never
         * wrong. */
        qemu_mutex_unlock(&fi_lock);
        return;
    }

    fi_feed_colorhist(surface_gen, idx, rgba, width, height);
    qemu_mutex_unlock(&fi_lock);
}

void xemu_frameinspect_capture_attach_pixels(uint32_t surface_gen,
                                             const uint32_t *rgba,
                                             uint32_t width, uint32_t height)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        return;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (qatomic_read(&fi_state) == FI_CAP_CAPTURING &&
        fi_cap.last_event != FI_EVENT_INVALID &&
        surface_gen != FI_SURFGEN_INVALID && rgba) {
        fi_feed_colorhist(surface_gen, fi_cap.last_event, rgba, width, height);
    }
    qemu_mutex_unlock(&fi_lock);
}

const FICapture *xemu_frameinspect_capture_acquire(void)
{
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    FICapture *capture = fi_published;
    if (capture) {
        capture->refcount++;
    }
    qemu_mutex_unlock(&fi_lock);
    return capture;
}

void xemu_frameinspect_capture_release(const FICapture *capture)
{
    if (!capture) {
        return;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    FICapture *mutable_capture = (FICapture *)capture;
    assert(mutable_capture->refcount > 0);
    if (mutable_capture->refcount == 0) {
        qemu_mutex_unlock(&fi_lock);
        return;
    }
    bool destroy = --mutable_capture->refcount == 0;
    qemu_mutex_unlock(&fi_lock);
    if (destroy) {
        fi_capture_destroy(mutable_capture);
    }
}

char *xemu_frameinspect_capture_summary(void)
{
    const FICapture *c = xemu_frameinspect_capture_acquire();
    if (!c) {
        return g_strdup("Frame inspector: no capture");
    }
    char *summary = g_strdup_printf(
        "Captured frame: %u events, %u surfaces%s", c->events.count,
        c->surfaces.num_gens,
        (c->truncated || c->events.truncated || c->surfaces.truncated ||
         c->resources.truncated) ? " [TRUNCATED]" : "");
    xemu_frameinspect_capture_release(c);
    return summary;
}

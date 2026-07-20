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
#include "xemu-frameinspect-tagmap.h"
#include "xemu-frameinspect-capture.h"

static FICapture fi_cap;                 /* capture in progress */
static FICaptureState fi_state = FI_CAP_IDLE;
static FICapture *fi_published;          /* immutable, protected by fi_lock */
static QemuMutex fi_lock;
static int fi_lock_state;
static uint64_t fi_next_capture_id;

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
    if (c->methods_bytes) {
        fi_budget_release(&c->budget, c->methods_bytes);
    }
    fi_methodlog_free(&c->methods);
    fi_commandlog_free(&c->commands, &c->budget);
    if (c->batch_res_bytes) {
        fi_budget_release(&c->budget, c->batch_res_bytes);
    }
    free(c->batch_res);
    if (c->origins.bytes) {
        fi_budget_release(&c->budget, c->origins.bytes);
    }
    fi_origin_snapshot_free(&c->origins);
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
    c->open_batch_event = FI_EVENT_INVALID;
    c->last_event = FI_EVENT_INVALID;
    c->budget.limit = FI_CAP_BUDGET_DEFAULT;
    uint64_t surface_bytes =
        (uint64_t)FI_SURF_MAX_GENS * sizeof(FISurfaceGen);
    /* fi_methodlog_init()'s fixed initial allocation (65536 recs + 1024
     * batches); charged to the budget here since the header (unlike the
     * other stores) doesn't take a FIBudget itself. */
    uint64_t methods_bytes = (uint64_t)65536 * sizeof(FIMethodRec) +
                             (uint64_t)1024 * sizeof(FIMethodBatch);
    if (!fi_budget_try(&c->budget, surface_bytes) ||
        !fi_surfaces_init(&c->surfaces) ||
        !fi_resources_init(&c->resources, &c->budget) ||
        !fi_eventlog_init(&c->events, &c->budget) ||
        !fi_commandlog_init(&c->commands, &c->budget) ||
        !fi_budget_try(&c->budget, methods_bytes)) {
        fi_capture_reset(c);
        return false;
    }
    /* Charged regardless of the init outcome below, so fi_capture_reset()
     * (which releases exactly c->methods_bytes) stays symmetric even if
     * fi_methodlog_init() itself fails the malloc. */
    c->methods_bytes = methods_bytes;
    if (!fi_methodlog_init(&c->methods)) {
        fi_capture_reset(c);
        return false;
    }
    c->origin_generation = xemu_frameinspect_generation();
    return true;
}

static void fi_capture_command_diag(const FICapture *c)
{
    uint32_t kinds[FI_CMD_RESERVED + 1] = {};
    uint32_t unknown = 0;
    uint32_t orphan_params = 0;
    uint32_t invalid_links = 0;
    uint32_t malformed = 0;
    uint32_t unresolved_origins = 0;

    for (uint32_t i = 0; i < c->commands.num_recs; i++) {
        const FICommandRec *rec = &c->commands.recs[i];
        if (rec->kind <= FI_CMD_RESERVED) {
            kinds[rec->kind]++;
        } else {
            unknown++;
            malformed++;
            continue;
        }
        if (rec->seq != i) {
            malformed++;
        }
        if ((rec->confidence == FI_ORIG_ATTRIBUTED ||
             rec->confidence == FI_ORIG_PARTIAL) &&
            !fi_origin_snapshot_node(&c->origins, rec->writer_node)) {
            unresolved_origins++;
        }

        switch (rec->kind) {
        case FI_CMD_METHOD_HEADER: {
            uint32_t raw_type = rec->raw & 0xe0030003;
            uint8_t method_type = raw_type == 0 ?
                FI_CMD_METHOD_INCREASING : FI_CMD_METHOD_NON_INCREASING;
            if ((raw_type != 0 && raw_type != 0x40000000) ||
                rec->data.header.method != (rec->raw & 0x1ffc) ||
                rec->data.header.subchannel != ((rec->raw >> 13) & 7) ||
                rec->data.header.count != ((rec->raw >> 18) & 0x7ff) ||
                rec->data.header.method_type != method_type) {
                malformed++;
            }
            break;
        }
        case FI_CMD_METHOD_PARAM: {
            uint32_t packet = rec->data.parameter.packet;
            if (packet == FI_COMMAND_INVALID) {
                orphan_params++;
                break;
            }
            if (packet >= i || packet >= c->commands.num_recs ||
                c->commands.recs[packet].kind != FI_CMD_METHOD_HEADER) {
                invalid_links++;
                break;
            }
            const FICommandRec *header = &c->commands.recs[packet];
            uint32_t parameter_index = rec->data.parameter.parameter_index;
            uint32_t expected_method = header->data.header.method;
            if (header->data.header.method_type == FI_CMD_METHOD_INCREASING) {
                expected_method += 4 * parameter_index;
            }
            if (parameter_index >= header->data.header.count ||
                rec->data.parameter.method != expected_method ||
                rec->data.parameter.subchannel !=
                    header->data.header.subchannel ||
                rec->data.parameter.method_type !=
                    header->data.header.method_type) {
                invalid_links++;
            }
            break;
        }
        case FI_CMD_OLD_JUMP:
            if ((rec->raw & 0xe0000003) != 0x20000000 ||
                rec->data.control.target != (rec->raw & 0x1fffffff)) {
                malformed++;
            }
            break;
        case FI_CMD_JUMP:
            if ((rec->raw & 3) != 1 ||
                rec->data.control.target != (rec->raw & 0xfffffffc)) {
                malformed++;
            }
            break;
        case FI_CMD_CALL:
            if ((rec->raw & 3) != 2 ||
                rec->data.control.target != (rec->raw & 0xfffffffc)) {
                malformed++;
            }
            break;
        case FI_CMD_RETURN:
            if (rec->raw != 0x00020000) {
                malformed++;
            }
            break;
        case FI_CMD_RESERVED:
            break;
        default:
            g_assert_not_reached();
        }
    }

    fprintf(stderr,
            "FI_COMMAND_DIAG records=%u headers=%u params=%u jumps=%u "
            "old_jumps=%u calls=%u returns=%u reserved=%u unknown=%u "
            "orphan_params=%u invalid_links=%u malformed=%u "
            "unresolved_origins=%u truncated=%u\n",
            c->commands.num_recs, kinds[FI_CMD_METHOD_HEADER],
            kinds[FI_CMD_METHOD_PARAM], kinds[FI_CMD_JUMP],
            kinds[FI_CMD_OLD_JUMP], kinds[FI_CMD_CALL], kinds[FI_CMD_RETURN],
            kinds[FI_CMD_RESERVED], unknown, orphan_params, invalid_links,
            malformed, unresolved_origins, c->commands.truncated);
}

static void fi_capture_batch_origin_diag(const FICapture *c)
{
    for (uint32_t i = 0; i < c->methods.num_batches; i++) {
        const FIMethodBatch *batch = &c->methods.batches[i];
        if (batch->first_rec > c->methods.num_recs ||
            batch->rec_count > c->methods.num_recs - batch->first_rec) {
            fprintf(stderr,
                    "FI_BATCH_ORIGIN batch=%u event=%u invalid_range=1\n",
                    i, batch->batch_event);
            continue;
        }
        uint32_t resolved = 0;
        uint32_t root = 0;
        uint32_t unattributed = 0;
        uint32_t phys_min = UINT32_MAX;
        uint32_t phys_max = 0;
        for (uint32_t j = batch->first_rec;
             j < batch->first_rec + batch->rec_count; j++) {
            const FIMethodRec *rec = &c->methods.recs[j];
            if (rec->phys_addr < phys_min) {
                phys_min = rec->phys_addr;
            }
            if (rec->phys_addr > phys_max) {
                phys_max = rec->phys_addr;
            }
            if (rec->confidence != FI_ORIG_ATTRIBUTED &&
                rec->confidence != FI_ORIG_PARTIAL) {
                unattributed++;
            } else if (rec->writer_node == FI_NODE_ROOT) {
                root++;
            } else {
                resolved++;
            }
        }
        fprintf(stderr,
                "FI_BATCH_ORIGIN batch=%u event=%u methods=%u resolved=%u "
                "root=%u unattributed=%u phys=0x%08x-0x%08x\n",
                i, batch->batch_event, batch->rec_count, resolved, root,
                unattributed, phys_min == UINT32_MAX ? 0 : phys_min, phys_max);
    }
    fflush(stderr);
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
    /* Lead-in: enable guest instrumentation now so writes during the frame
     * before the captured frame are tagged. */
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
         qatomic_read(&fi_state) == FI_CAP_LEAD_IN ||
         qatomic_read(&fi_state) == FI_CAP_LEAD_IN_2 ||
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
        /* The hotkey can arrive immediately before a flip. Establish a frame
         * boundary first so provenance always gets one complete lead-in. */
        qatomic_set(&fi_state, FI_CAP_LEAD_IN);
        qemu_mutex_unlock(&fi_lock);
        return FI_CAP_FLIP_NONE;
    case FI_CAP_LEAD_IN:
        qatomic_set(&fi_state, FI_CAP_LEAD_IN_2);
        qemu_mutex_unlock(&fi_lock);
        return FI_CAP_FLIP_NONE;
    case FI_CAP_LEAD_IN_2:
        /* Two complete instrumented frames elapsed; capture the next frame. */
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
        /* Following flip: the captured frame ended. Finalize + publish. */
        xemu_frameinspect_disarm();
        uint64_t origin_limit = fi_cap.budget.limit - fi_cap.budget.used;
        bool origins_copied = xemu_frameinspect_snapshot_origins(
            &fi_cap.origins, origin_limit);
        if (origins_copied &&
            !fi_budget_try(&fi_cap.budget, fi_cap.origins.bytes)) {
            uint32_t truncation =
                fi_cap.origins.truncation | FI_ORIGIN_TRUNC_BUDGET;
            fi_origin_snapshot_free(&fi_cap.origins);
            fi_cap.origins.truncation = truncation;
            origins_copied = false;
        }
        if (!origins_copied || fi_cap.origins.truncation) {
            fi_cap.truncated = true;
        }

        FICapture *done = (FICapture *)malloc(sizeof(FICapture));
        if (!done) {
            fi_capture_reset(&fi_cap);
            qatomic_set(&fi_state, FI_CAP_IDLE);
            qemu_mutex_unlock(&fi_lock);
            return FI_CAP_FLIP_FAILED;
        }
        *done = fi_cap;                 /* transfer ownership of buffers */
        memset(&fi_cap, 0, sizeof(fi_cap));
        done->refcount = 1;             /* published-pointer ownership */
        done->capture_id = ++fi_next_capture_id;
        uint32_t attributed = 0;
        uint32_t partial = 0;
        uint32_t unattributed = 0;
        uint32_t root = 0;
        for (uint32_t i = 0; i < done->methods.num_recs; i++) {
            if ((done->methods.recs[i].confidence == FI_ORIG_ATTRIBUTED ||
                 done->methods.recs[i].confidence == FI_ORIG_PARTIAL) &&
                done->methods.recs[i].writer_node == FI_NODE_ROOT) {
                root++;
            }
            switch (done->methods.recs[i].confidence) {
            case FI_ORIG_ATTRIBUTED:
                attributed++;
                break;
            case FI_ORIG_PARTIAL:
                partial++;
                break;
            default:
                unattributed++;
                break;
            }
        }
        char *origin_status = xemu_frameinspect_status_line();
        fprintf(stderr,
                "FI_ORIGIN_DIAG methods=%u attributed=%u partial=%u "
                "unattributed=%u root=%u %s\n",
                done->methods.num_recs, attributed, partial, unattributed, root,
                origin_status);
        g_free(origin_status);
        fi_capture_batch_origin_diag(done);
        fi_capture_command_diag(done);
        FICapture *old = fi_published;
        fi_published = done;
        bool destroy_old = old && --old->refcount == 0;
        qatomic_set(&fi_state, FI_CAP_PAUSE_PENDING);
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
                  qatomic_read(&fi_state) == FI_CAP_LEAD_IN ||
                  qatomic_read(&fi_state) == FI_CAP_LEAD_IN_2 ||
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
    /* All fi_state writes are already qatomic_set under fi_lock; reading it
     * needs no lock (and no sync_init, which only exists to lazily init that
     * lock). This removes the residual per-draw/clear/blit locked call from
     * the hot path when idle. */
    return qatomic_read(&fi_state);
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
    fi_cap.last_event = idx;
    fi_cap.open_batch_event = idx;
    fi_cap.batch_first_rec = MIN(fi_cap.batch_pending_first_rec,
                                 fi_cap.methods.num_recs);
    qemu_mutex_unlock(&fi_lock);
    return idx != FI_EVENT_INVALID;
}

void xemu_frameinspect_capture_end_batch(void)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        return;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (qatomic_read(&fi_state) == FI_CAP_CAPTURING) {
        if (fi_cap.batch_open && fi_cap.open_batch_event != FI_EVENT_INVALID) {
            uint32_t rec_count = fi_cap.methods.num_recs - fi_cap.batch_first_rec;
            if (rec_count > 0) {
                fi_methodlog_mark_batch(&fi_cap.methods, fi_cap.open_batch_event,
                                         fi_cap.batch_first_rec, rec_count);
            }
        }
        /* draw_end runs while PFIFO is dispatching SET_BEGIN_END(END), before
         * capture_methods() appends that parameter. Skip it; subsequent state
         * setup belongs to the next draw batch. */
        fi_cap.batch_pending_first_rec = fi_cap.methods.num_recs + 1;
        fi_cap.open_batch_gen = FI_SURFGEN_INVALID;
        fi_cap.open_batch_zeta_gen = FI_SURFGEN_INVALID;
        fi_cap.open_batch_event = FI_EVENT_INVALID;
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
        uint32_t idx = fi_eventlog_append(&fi_cap.events, &fi_cap.budget,
                                          FI_EV_BATCH, fi_cap.open_batch_gen,
                                          fi_cap.open_batch_zeta_gen, 0, 0, 0);
        fi_cap.last_event = idx;
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
        fi_cap.last_event = idx;
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
        fi_cap.last_event = idx;
    }
    qemu_mutex_unlock(&fi_lock);
}

/* Return the initialized history for a surface, growing and charging the
 * capture-owned array as needed. Caller holds fi_lock. */
static FIColorHist *fi_get_colorhist(uint32_t surface_gen, uint32_t width,
                                     uint32_t height)
{
    if (surface_gen >= fi_cap.hist_count) {
        uint32_t new_count = surface_gen + 1;
        uint64_t old_bytes = (uint64_t)fi_cap.hist_count * sizeof(FIColorHist);
        uint64_t new_bytes = (uint64_t)new_count * sizeof(FIColorHist);
        if (!fi_budget_try(&fi_cap.budget, new_bytes - old_bytes)) {
            fi_cap.truncated = true;
            return NULL;
        }
        FIColorHist *nh = (FIColorHist *)realloc(
            fi_cap.hist, (size_t)new_bytes);
        if (!nh) {
            fi_budget_release(&fi_cap.budget, new_bytes - old_bytes);
            fi_cap.truncated = true;
            return NULL;
        }
        memset(&nh[fi_cap.hist_count], 0,
              (new_count - fi_cap.hist_count) * sizeof(FIColorHist));
        fi_cap.hist = nh;
        fi_cap.hist_count = new_count;
    }

    FIColorHist *ch = &fi_cap.hist[surface_gen];
    if (ch->width == 0) {
        uint64_t remaining = fi_cap.budget.limit - fi_cap.budget.used;
        if (!fi_colorhist_init(ch, width, height, 16) ||
            ch->bytes_used > remaining) {
            fi_colorhist_free(ch);
            fi_cap.truncated = true;
            return NULL;
        }
        if (!fi_budget_try(&fi_cap.budget, ch->bytes_used)) {
            fi_colorhist_free(ch);
            fi_cap.truncated = true;
            return NULL;
        }
    } else if (ch->width != width || ch->height != height) {
        fi_cap.truncated = true;
        return NULL;
    }
    return ch;
}

static bool fi_charge_colorhist_growth(FIColorHist *ch, uint64_t old_bytes)
{
    uint64_t growth = ch->bytes_used - old_bytes;
    if (!fi_budget_try(&fi_cap.budget, growth)) {
        fi_cap.truncated = true;
        return false;
    }
    return true;
}

static bool fi_set_colorhist_baseline(uint32_t surface_gen,
                                      const uint32_t *rgba, uint32_t width,
                                      uint32_t height)
{
    FIColorHist *ch = fi_get_colorhist(surface_gen, width, height);
    if (!ch) {
        return false;
    }
    if (ch->has_baseline) {
        return true;
    }
    uint64_t remaining = fi_cap.budget.limit - fi_cap.budget.used;
    ch->byte_budget = MIN((uint64_t)FI_CH_BYTE_BUDGET,
                          ch->bytes_used + remaining);
    uint64_t old_bytes = ch->bytes_used;
    bool set = fi_colorhist_set_baseline(ch, rgba);
    bool charged = fi_charge_colorhist_growth(ch, old_bytes);
    if (!set || !charged) {
        fi_cap.truncated = true;
        return false;
    }
    return true;
}

/* Feed a post-writer image into a surface's history. If a producer missed the
 * pre-write baseline hook, retain this image as a fallback baseline and still
 * append a zero-delta event so scanout-only surfaces remain reconstructable. */
static void fi_feed_colorhist(uint32_t surface_gen, uint32_t event_idx,
                              const uint32_t *rgba, uint32_t width,
                              uint32_t height)
{
    if (!fi_set_colorhist_baseline(surface_gen, rgba, width, height)) {
        return;
    }
    FIColorHist *ch = &fi_cap.hist[surface_gen];
    uint64_t remaining = fi_cap.budget.limit - fi_cap.budget.used;
    ch->byte_budget = MIN((uint64_t)FI_CH_BYTE_BUDGET,
                          ch->bytes_used + remaining);
    uint64_t old_bytes = ch->bytes_used;
    bool added = fi_colorhist_add_event(ch, event_idx, rgba);
    bool charged = fi_charge_colorhist_growth(ch, old_bytes);
    if (!added || !charged) {
        fi_cap.truncated = true;
    }
}

bool xemu_frameinspect_capture_needs_baseline(uint32_t surface_gen)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING ||
        surface_gen == FI_SURFGEN_INVALID) {
        return false;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    bool needed = qatomic_read(&fi_state) == FI_CAP_CAPTURING &&
                  (surface_gen >= fi_cap.hist_count ||
                   !fi_cap.hist[surface_gen].has_baseline);
    qemu_mutex_unlock(&fi_lock);
    return needed;
}

void xemu_frameinspect_capture_baseline(uint32_t surface_gen,
                                        const uint32_t *rgba,
                                        uint32_t width, uint32_t height)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING ||
        surface_gen == FI_SURFGEN_INVALID || !rgba) {
        return;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (qatomic_read(&fi_state) == FI_CAP_CAPTURING) {
        fi_set_colorhist_baseline(surface_gen, rgba, width, height);
    }
    qemu_mutex_unlock(&fi_lock);
}

bool xemu_frameinspect_capture_readback_allowed(uint32_t width,
                                                uint32_t height)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING || !width || !height ||
        width > UINT32_MAX / height) {
        return false;
    }
    uint64_t bytes = (uint64_t)width * height * sizeof(uint32_t);
    if (bytes > FI_CH_BYTE_BUDGET) {
        return false;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    bool allowed = qatomic_read(&fi_state) == FI_CAP_CAPTURING &&
                   fi_cap.budget.used <= fi_cap.budget.limit &&
                   bytes <= fi_cap.budget.limit - fi_cap.budget.used;
    qemu_mutex_unlock(&fi_lock);
    return allowed;
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

void xemu_frameinspect_capture_scanout(uint32_t surface_gen,
                                       uint32_t pcrtc_start,
                                       uint32_t line_offset, uint32_t flags,
                                       const uint32_t *surface_rgba,
                                       uint32_t surface_width,
                                       uint32_t surface_height,
                                       const uint32_t *display_rgba,
                                       uint32_t display_width,
                                       uint32_t display_height)
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

    uint32_t display_res = FI_RES_INVALID;
    if (display_rgba && display_width && display_height &&
        display_width <= UINT32_MAX / display_height &&
        (uint64_t)display_width * display_height <= UINT32_MAX / 4) {
        uint32_t len = display_width * display_height * sizeof(uint32_t);
        uint64_t meta = ((uint64_t)display_width << 32) | display_height;
        display_res = fi_resources_intern(&fi_cap.resources, &fi_cap.budget,
                                          FI_RESK_SCANOUT_RGBA, display_rgba,
                                          len, meta);
        if (display_res == FI_RES_INVALID) {
            fi_cap.truncated = true;
        }
    }
    uint32_t idx = fi_eventlog_append(&fi_cap.events, &fi_cap.budget,
                                      FI_EV_SCANOUT, surface_gen, pcrtc_start,
                                      line_offset, flags, display_res);
    fi_cap.last_event = idx;
    if (idx != FI_EVENT_INVALID && surface_gen != FI_SURFGEN_INVALID &&
        surface_rgba) {
        fi_feed_colorhist(surface_gen, idx, surface_rgba, surface_width,
                          surface_height);
    }
    qemu_mutex_unlock(&fi_lock);
}

void xemu_frameinspect_capture_methods(uint32_t first_method, bool method_inc,
                                       uint16_t subchannel,
                                       const uint32_t *words, uint32_t n,
                                       uint32_t n_labeled,
                                       uint64_t phys_base,
                                       const uint32_t *writer_tags)
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

    /* Approximate the charge as n full records up front (the log may still
     * be well under cap_recs and not actually grow) and release the
     * remainder below if we stop early; simpler than tracking the log's
     * internal realloc growth exactly, and never under-charges. */
    uint64_t bytes = (uint64_t)n * sizeof(FIMethodRec);
    if (!fi_budget_try(&fi_cap.budget, bytes)) {
        fi_cap.truncated = true;
        fi_cap.methods.truncated = true;
        qemu_mutex_unlock(&fi_lock);
        return;
    }
    fi_cap.methods_bytes += bytes;

    uint32_t appended = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t method_i = i < n_labeled ?
            (method_inc ? first_method + 4 * i : first_method) :
            FI_METHOD_RAW_WORD;
        uint64_t phys_i = phys_base + 4ull * i;
        uint32_t tag = writer_tags ? writer_tags[i] : 0;
        uint16_t confidence = tag == 0 ? FI_ORIG_UNATTRIBUTED :
                              (tag & FI_TAG_PARTIAL) ? FI_ORIG_PARTIAL :
                                                        FI_ORIG_ATTRIBUTED;
        uint32_t writer_node = tag == 0 ? 0 : FI_TAG_NODE(tag);
        uint32_t idx = fi_methodlog_append(&fi_cap.methods, method_i,
                                           subchannel, words[i],
                                           (uint32_t)phys_i, writer_node,
                                           confidence);
        if (idx == FI_METHOD_INVALID) {
            break;
        }
        appended++;
    }
    if (appended < n) {
        uint64_t remainder = (uint64_t)(n - appended) * sizeof(FIMethodRec);
        fi_budget_release(&fi_cap.budget, remainder);
        fi_cap.methods_bytes -= remainder;
    }
    qemu_mutex_unlock(&fi_lock);
}

uint32_t xemu_frameinspect_capture_command(const FICommandRec *rec)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING || !rec) {
        return FI_COMMAND_INVALID;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        qemu_mutex_unlock(&fi_lock);
        return FI_COMMAND_INVALID;
    }
    uint32_t id = fi_commandlog_append(&fi_cap.commands, &fi_cap.budget, rec);
    if (id == FI_COMMAND_INVALID) {
        fi_cap.truncated = true;
    }
    qemu_mutex_unlock(&fi_lock);
    return id;
}

uint32_t xemu_frameinspect_capture_resource(uint32_t kind, const void *data,
                                            uint32_t len, uint64_t meta)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        return FI_RES_INVALID;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        qemu_mutex_unlock(&fi_lock);
        return FI_RES_INVALID;
    }
    uint32_t id = fi_resources_intern(&fi_cap.resources, &fi_cap.budget, kind,
                                      data, len, meta);
    if (id == FI_RES_INVALID) {
        /* The pool already flags itself truncated on every failure path;
         * mirror that onto the capture-wide flag too. */
        fi_cap.truncated = true;
    }
    qemu_mutex_unlock(&fi_lock);
    return id;
}

void xemu_frameinspect_capture_batch_resource_ref(uint32_t res_id)
{
    if (qatomic_read(&fi_state) != FI_CAP_CAPTURING) {
        return;
    }
    fi_capture_sync_init();
    qemu_mutex_lock(&fi_lock);
    if (qatomic_read(&fi_state) == FI_CAP_CAPTURING && fi_cap.batch_open &&
        fi_cap.open_batch_event != FI_EVENT_INVALID &&
        res_id != FI_RES_INVALID) {
        if (fi_cap.num_batch_res >= fi_cap.cap_batch_res) {
            uint32_t nc = fi_cap.cap_batch_res ? fi_cap.cap_batch_res * 2 : 256;
            uint64_t old_bytes =
                (uint64_t)fi_cap.cap_batch_res * sizeof(FIBatchResRef);
            uint64_t new_bytes = (uint64_t)nc * sizeof(FIBatchResRef);
            if (fi_budget_try(&fi_cap.budget, new_bytes - old_bytes)) {
                FIBatchResRef *nb = (FIBatchResRef *)realloc(
                    fi_cap.batch_res, (size_t)new_bytes);
                if (nb) {
                    fi_cap.batch_res = nb;
                    fi_cap.cap_batch_res = nc;
                    fi_cap.batch_res_bytes += new_bytes - old_bytes;
                } else {
                    fi_budget_release(&fi_cap.budget, new_bytes - old_bytes);
                    fi_cap.truncated = true;
                }
            } else {
                fi_cap.truncated = true;
            }
        }
        if (fi_cap.num_batch_res < fi_cap.cap_batch_res) {
            fi_cap.batch_res[fi_cap.num_batch_res].event =
                fi_cap.open_batch_event;
            fi_cap.batch_res[fi_cap.num_batch_res].res_id = res_id;
            fi_cap.num_batch_res++;
        }
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
        "Captured frame: %u events, %u surfaces, %u commands, %u methods, "
        "%u resources%s",
        c->events.count, c->surfaces.num_gens, c->commands.num_recs,
        c->methods.num_recs, c->resources.num_res,
        (c->truncated || c->events.truncated || c->surfaces.truncated ||
         c->resources.truncated || c->methods.truncated ||
         c->commands.truncated) ? " [TRUNCATED]" : "");
    xemu_frameinspect_capture_release(c);
    return summary;
}

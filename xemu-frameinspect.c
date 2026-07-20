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
#include "qemu/atomic.h"
#include "qemu/thread.h"
#include "cpu.h"
#include "exec/tb-flush.h"
#include "xemu-xbe.h"
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
static uint32_t fi_generation;
static QemuMutex fi_origin_lock;
static int fi_origin_lock_state;
static struct {
    uint64_t calls;
    uint64_t clean_rets;
    uint64_t unmatched_rets;
    uint64_t empty_rets;
    uint64_t skipped_ret_frames;
    uint64_t esp_resets;
    uint64_t esp_frames_popped;
    uint64_t materializations;
    uint64_t materialized_nodes;
    uint64_t root_materializations;
    uint64_t root_stores;
    uint32_t max_depth;
    uint32_t max_nodes_per_materialization;
} fi_health;

static void fi_origin_sync_init(void);

static void fi_log_saturation(const char *pool, uint32_t thread_key,
                              uint32_t call_site, uint32_t callee,
                              uint32_t nodes, uint32_t argsets,
                              uint64_t stores, uint32_t generation)
{
    struct xbe *xbe = xemu_get_xbe_info();
    if (xbe && xbe->header && xbe->cert) {
        fprintf(stderr,
                "FI_ORIGIN_SATURATION pool=%s generation=%u nodes=%u "
                "argsets=%u stores=%" PRIu64 " thread=0x%08x "
                "call_site=0x%08x callee=0x%08x xbe=active "
                "title_id=0x%08x image_base=0x%08x\n",
                pool, generation, nodes, argsets, stores, thread_key,
                call_site, callee, ldl_le_p(&xbe->cert->m_titleid),
                ldl_le_p(&xbe->header->m_base));
    } else {
        fprintf(stderr,
                "FI_ORIGIN_SATURATION pool=%s generation=%u nodes=%u "
                "argsets=%u stores=%" PRIu64 " thread=0x%08x "
                "call_site=0x%08x callee=0x%08x xbe=none\n",
                pool, generation, nodes, argsets, stores, thread_key,
                call_site, callee);
    }
    fflush(stderr);
}

static uint32_t fi_materialize_writer(uint32_t thread_key)
{
    uint32_t node = fi_shadow_current(&fi_shadow, thread_key);
    if (node != FI_NODE_INVALID) {
        return node;
    }

    fi_origin_sync_init();
    qemu_mutex_lock(&fi_origin_lock);
    bool nodes_were_truncated = fi_tree.nodes_truncated;
    bool argsets_were_truncated = fi_tree.pool_truncated;
    const FIFrame *frame = fi_shadow_current_frame(&fi_shadow, thread_key);
    uint32_t call_site = frame ? frame->call_site : 0;
    uint32_t callee = frame ? frame->callee : 0;
    uint32_t old_nodes = fi_tree.num_nodes;
    node = fi_shadow_materialize(&fi_shadow, thread_key);
    uint32_t nodes_added = fi_tree.num_nodes - old_nodes;
    fi_health.materializations++;
    fi_health.materialized_nodes += nodes_added;
    if (nodes_added > fi_health.max_nodes_per_materialization) {
        fi_health.max_nodes_per_materialization = nodes_added;
    }
    if (node == FI_NODE_ROOT) {
        fi_health.root_materializations++;
    }
    bool nodes_saturated = !nodes_were_truncated && fi_tree.nodes_truncated;
    bool argsets_saturated =
        !argsets_were_truncated && fi_tree.pool_truncated;
    uint32_t nodes = fi_tree.num_nodes;
    uint32_t argsets = fi_tree.num_argsets;
    uint64_t stores = fi_stores_tagged;
    uint32_t generation = fi_generation;
    qemu_mutex_unlock(&fi_origin_lock);

    if (nodes_saturated) {
        fi_log_saturation("nodes", thread_key, call_site, callee, nodes,
                          argsets, stores, generation);
    }
    if (argsets_saturated) {
        fi_log_saturation("argsets", thread_key, call_site, callee, nodes,
                          argsets, stores, generation);
    }
    return node;
}

/* Watch addresses persist across captures; the engine is rebuilt on
 * every arm and seeded from this list. */
static uint32_t fi_watch_addrs[FI_WATCH_MAX];

static void fi_origin_sync_init(void)
{
    if (qatomic_load_acquire(&fi_origin_lock_state) == 2) {
        return;
    }
    if (qatomic_cmpxchg(&fi_origin_lock_state, 0, 1) == 0) {
        qemu_mutex_init(&fi_origin_lock);
        qatomic_store_release(&fi_origin_lock_state, 2);
        return;
    }
    while (qatomic_load_acquire(&fi_origin_lock_state) != 2) {
        cpu_relax();
    }
}

void xemu_frameinspect_arm(uint64_t ram_size)
{
    if (xemu_frameinspect_armed) {
        return;
    }
    fi_origin_sync_init();
    qemu_mutex_lock(&fi_origin_lock);
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
        qemu_mutex_unlock(&fi_origin_lock);
        return;                       /* allocation failed; stay disarmed */
    }
    memcpy(fi_watch.watches, fi_watch_addrs, sizeof(fi_watch_addrs));
    fi_shadow_init(&fi_shadow, &fi_tree);
    fi_stores_tagged = 0;
    memset(&fi_health, 0, sizeof(fi_health));
    fi_alive = true;
    qatomic_inc(&fi_generation);
    bool any_watch = false;
    for (int i = 0; i < FI_WATCH_MAX; i++) {
        any_watch |= fi_watch_addrs[i] != 0;
    }
    xemu_frameinspect_watch_mode = any_watch;
    /* Arm before flushing so retranslated code is instrumented
     * (same pattern as xemu-calltrace.c:174-176). */
    xemu_frameinspect_armed = true;
    queue_tb_flush(qemu_get_cpu(0));
    qemu_mutex_unlock(&fi_origin_lock);
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
    /* Stale TB may fire between disarm and flush completion. */
    if (!xemu_frameinspect_armed) {
        return false;
    }
    return fi_alive && fi_watch_find(&fi_watch, callee) >= 0;
}

uint32_t xemu_frameinspect_record_call(uint32_t thread_key, uint32_t call_site,
                                       uint32_t callee, uint32_t ret_addr,
                                       uint32_t esp, const uint32_t args[6])
{
    /* Stale TB may fire between disarm and flush completion. */
    if (!xemu_frameinspect_armed) {
        return FI_NODE_ROOT;
    }
    if (!fi_alive) {
        return FI_NODE_ROOT;
    }
    fi_origin_sync_init();
    qemu_mutex_lock(&fi_origin_lock);
    if (!xemu_frameinspect_armed || !fi_alive) {
        qemu_mutex_unlock(&fi_origin_lock);
        return FI_NODE_ROOT;
    }
    FIStack *stack = fi_shadow_stack(&fi_shadow, thread_key);
    uint32_t esp_pops = 0;
    while (stack && esp_pops < stack->depth &&
           esp > stack->frames[stack->depth - 1 - esp_pops].esp_at_call) {
        esp_pops++;
    }
    if (esp_pops) {
        fi_health.esp_resets++;
        fi_health.esp_frames_popped += esp_pops;
    }
    uint32_t node = fi_shadow_call_lazy(&fi_shadow, thread_key, call_site,
                                        callee, ret_addr, esp, args);
    fi_health.calls++;
    stack = fi_shadow_stack(&fi_shadow, thread_key);
    if (stack && stack->depth > fi_health.max_depth) {
        fi_health.max_depth = stack->depth;
    }
    qemu_mutex_unlock(&fi_origin_lock);
    return node;
}

void xemu_frameinspect_record_call_watched(uint32_t thread_key,
                                           uint32_t callee,
                                           const uint32_t regs[8],
                                           uint32_t esp,
                                           const uint32_t stack16[16])
{
    /* Stale TB may fire between disarm and flush completion. */
    if (!xemu_frameinspect_armed) {
        return;
    }
    if (!fi_alive) {
        return;
    }
    int idx = fi_watch_find(&fi_watch, callee);
    if (idx < 0) {
        return;
    }
    uint32_t node = fi_materialize_writer(thread_key);
    uint32_t invoc = fi_watch_enter(&fi_watch, idx, node, regs, esp, stack16);
    if (invoc != FI_INVOC_INVALID) {
        fi_shadow_set_watch(&fi_shadow, thread_key, invoc);
    }
}

void xemu_frameinspect_record_ret(uint32_t thread_key, uint32_t ret_target,
                                  uint32_t eax)
{
    /* Stale TB may fire between disarm and flush completion. */
    if (!xemu_frameinspect_armed) {
        return;
    }
    if (!fi_alive) {
        return;
    }
    FIPopped popped[FI_SS_MAX_POPS];
    uint32_t n = 0;
    fi_origin_sync_init();
    qemu_mutex_lock(&fi_origin_lock);
    FIStack *stack = fi_shadow_stack(&fi_shadow, thread_key);
    if (!stack || stack->depth == 0) {
        fi_health.empty_rets++;
    } else {
        uint32_t limit = stack->depth < FI_SS_MAX_POPS ?
                             stack->depth : FI_SS_MAX_POPS;
        bool matched = false;
        for (uint32_t k = 0; k < limit; k++) {
            if (stack->frames[stack->depth - 1 - k].expected_ret ==
                ret_target) {
                fi_health.clean_rets++;
                fi_health.skipped_ret_frames += k;
                matched = true;
                break;
            }
        }
        if (!matched) {
            fi_health.unmatched_rets++;
        }
    }
    fi_shadow_ret(&fi_shadow, thread_key, ret_target, popped, &n);
    qemu_mutex_unlock(&fi_origin_lock);
    for (uint32_t i = 0; i < n; i++) {
        fi_watch_exit(&fi_watch, popped[i].watch_invoc, popped[i].clean, eax);
    }
}

void xemu_frameinspect_record_store(uint32_t thread_key, uint64_t paddr,
                                    uint32_t len)
{
    /* Stale TB may fire between disarm and flush completion. */
    if (!xemu_frameinspect_armed) {
        return;
    }
    if (!fi_alive) {
        return;
    }
    uint32_t node = fi_materialize_writer(thread_key);
    fi_tagmap_tag(&fi_tags, paddr, len, node);
    fi_stores_tagged++;
    if (node == FI_NODE_ROOT) {
        fi_health.root_stores++;
    }
}

void xemu_frameinspect_record_store_watched(uint32_t thread_key,
                                            uint64_t paddr, uint32_t len,
                                            const uint8_t *old_bytes,
                                            const uint8_t *new_bytes,
                                            bool is_ram)
{
    /* Stale TB may fire between disarm and flush completion. */
    if (!xemu_frameinspect_armed) {
        return;
    }
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

/* Resolves against the LIVE fi_tree: valid only until the next arm (which
 * frees and replaces it, same lifetime caveat as xemu_frameinspect_arm()'s
 * "Captured data intentionally retained until next arm" note above, except
 * the tree is the one live structure that IS discarded on arm, not
 * retained). Same guard pattern as xemu_frameinspect_lookup_tag(): no lock,
 * just the fi_alive flag -- reading is only meaningful once the UI thread
 * is walking a paused capture. */
uint32_t xemu_frameinspect_generation(void)
{
    return qatomic_read(&fi_generation);
}

FINodeInfo xemu_frameinspect_node_info(uint32_t node_id,
                                       uint32_t generation)
{
    FINodeInfo info;
    memset(&info, 0, sizeof(info));
    fi_origin_sync_init();
    qemu_mutex_lock(&fi_origin_lock);
    if (!fi_alive || generation != qatomic_read(&fi_generation) ||
        node_id >= fi_tree.num_nodes) {
        qemu_mutex_unlock(&fi_origin_lock);
        return info; /* valid = false */
    }
    const FINode *n = &fi_tree.nodes[node_id];
    info.parent = n->parent;
    info.call_site = n->call_site;
    info.callee = n->callee;
    if (n->n_argsets > 0 && n->argset_ids[0] < fi_tree.num_argsets) {
        memcpy(info.args, fi_tree.argsets[n->argset_ids[0]],
              sizeof(info.args));
    }
    info.valid = true;
    qemu_mutex_unlock(&fi_origin_lock);
    return info;
}

bool xemu_frameinspect_snapshot_origins(FIOriginSnapshot *snapshot,
                                        uint64_t byte_limit)
{
    if (!snapshot) {
        return false;
    }
    fi_origin_sync_init();
    qemu_mutex_lock(&fi_origin_lock);
    if (!fi_alive) {
        fi_origin_snapshot_init(snapshot);
        snapshot->truncation = FI_ORIGIN_TRUNC_INVALID;
        qemu_mutex_unlock(&fi_origin_lock);
        return false;
    }

    QEMU_BUILD_BUG_MSG(sizeof(FINode) != sizeof(FIOriginNode),
                       "live and captured origin node layouts differ");
    QEMU_BUILD_BUG_MSG(offsetof(FINode, argset_ids) !=
                           offsetof(FIOriginNode, argset_ids),
                       "live and captured origin node fields differ");
    uint32_t truncation =
        (fi_tree.nodes_truncated ? FI_ORIGIN_TRUNC_NODES : 0) |
        (fi_tree.pool_truncated ? FI_ORIGIN_TRUNC_ARGSETS : 0);
    bool copied = fi_origin_snapshot_copy(
        snapshot, (const FIOriginNode *)fi_tree.nodes, fi_tree.num_nodes,
        (const uint32_t (*)[6])fi_tree.argsets, fi_tree.num_argsets,
        byte_limit, truncation);
    qemu_mutex_unlock(&fi_origin_lock);
    return copied;
}

uint64_t xemu_frameinspect_ram_size(void)
{
    return fi_alive ? fi_tags.ram_size : 0;
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
    fi_origin_sync_init();
    qemu_mutex_lock(&fi_origin_lock);
    if (!fi_alive) {
        qemu_mutex_unlock(&fi_origin_lock);
        return g_strdup("Frame inspector: no capture");
    }
    char *status = g_strdup_printf(
        "FI: %u nodes, %u argsets, %u threads, %" PRIu64 " stores, "
        "%u invocs, %u writes; calls=%" PRIu64 " rets=%" PRIu64
        "/%" PRIu64 "/%" PRIu64 " skipped=%" PRIu64
        " esp=%" PRIu64 "/%" PRIu64 " depth=%u; materialize=%" PRIu64
        " nodes=%" PRIu64 " max=%u root=%" PRIu64
        " root_stores=%" PRIu64 "%s",
        fi_tree.num_nodes, fi_tree.num_argsets, fi_shadow.n_stacks,
        fi_stores_tagged, fi_watch.num_invocs, fi_watch.num_recs,
        fi_health.calls, fi_health.clean_rets, fi_health.unmatched_rets,
        fi_health.empty_rets, fi_health.skipped_ret_frames,
        fi_health.esp_resets, fi_health.esp_frames_popped,
        fi_health.max_depth, fi_health.materializations,
        fi_health.materialized_nodes,
        fi_health.max_nodes_per_materialization,
        fi_health.root_materializations, fi_health.root_stores,
        (fi_tree.nodes_truncated || fi_tree.pool_truncated ||
         fi_watch.invocs_truncated ||
          fi_watch.log_truncated) ? " [TRUNCATED]" : "");
    qemu_mutex_unlock(&fi_origin_lock);
    return status;
}

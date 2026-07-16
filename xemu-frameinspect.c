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
    uint32_t node = fi_shadow_current(&fi_shadow, thread_key);
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
    fi_shadow_ret(&fi_shadow, thread_key, ret_target, popped, &n);
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

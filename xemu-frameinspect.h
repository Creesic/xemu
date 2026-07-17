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

#ifndef XEMU_FRAMEINSPECT_H
#define XEMU_FRAMEINSPECT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True while a capture is armed: the x86 translator only emits frame
 * inspector helper calls into translated code while this is set. */
extern bool xemu_frameinspect_armed;
/* True while any callee is watched; gates the pre-store hook. */
extern bool xemu_frameinspect_watch_mode;

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
/* Mirrors xemu-frameinspect-calltree.h's sentinels (same spelling, so
 * redefinition is a no-op if both headers end up included in one TU): the
 * root/unknown call-path node, and "no such node". Exposed here too so UI
 * callers of xemu_frameinspect_node_info() (a C++ TU) don't need to include
 * calltree.h, which is C-only (its calloc()/malloc() results rely on C's
 * implicit void* conversion and won't compile as C++). */
#define FI_NODE_INVALID 0xFFFFFFFFu
#define FI_NODE_ROOT    0u
/* One call-path node's shape (parent/call_site/callee) and its first
 * argument set, resolved against the LIVE call tree. Valid only until the
 * next xemu_frameinspect_arm() (a re-arm frees the tree this reads); UI
 * callers (Plan 3) must treat `valid == false` as "unavailable", never
 * fabricate a chain. */
typedef struct {
    uint32_t parent;
    uint32_t call_site;
    uint32_t callee;
    uint32_t args[6];
    bool valid;
} FINodeInfo;
uint32_t xemu_frameinspect_generation(void);
FINodeInfo xemu_frameinspect_node_info(uint32_t node_id,
                                       uint32_t generation);
/* Armed capture's RAM size (bytes), or 0 if no capture is live. Used by
 * the store helpers to tell real RAM apart from mapped MMIO/device
 * memory before doing a debug-read byte diff. */
uint64_t xemu_frameinspect_ram_size(void);
bool xemu_frameinspect_watch_add(uint32_t callee);
void xemu_frameinspect_watch_remove(uint32_t callee);
char *xemu_frameinspect_status_line(void);  /* g_strdup'd, caller frees */

#ifdef __cplusplus
}
#endif

#endif

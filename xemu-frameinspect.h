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
bool xemu_frameinspect_watch_add(uint32_t callee);
void xemu_frameinspect_watch_remove(uint32_t callee);
char *xemu_frameinspect_status_line(void);  /* g_strdup'd, caller frees */

#ifdef __cplusplus
}
#endif

#endif

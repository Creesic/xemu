/*
 * xemu guest call tracing
 *
 * Records the running game's dynamic call graph as deduplicated
 * (call_site -> callee) edges for offline visualization.
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

#ifndef XEMU_CALLTRACE_H
#define XEMU_CALLTRACE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * True while recording. Read by the x86 translator, which only emits
 * trace helper calls into translated code while this is set; toggling
 * flushes the TB cache so stale code never lingers.
 */
extern bool xemu_calltrace_armed;

typedef enum { CT_OFF, CT_EDGES, CT_TIMED, CT_DATA } CalltraceMode;

void xemu_calltrace_start_mode(CalltraceMode mode);
void xemu_calltrace_start(void); /* legacy: start in CT_EDGES */
void xemu_calltrace_stop(void);
CalltraceMode xemu_calltrace_mode(void);
uint64_t xemu_calltrace_edge_count(void);
bool xemu_calltrace_truncated(void);
uint64_t xemu_calltrace_event_count(void);
bool xemu_calltrace_events_truncated(void);

/* Ignore-list: callee addresses to drop entirely from recording. */
void xemu_calltrace_load_ignore(const char *path, int *added, int *skipped);
void xemu_calltrace_clear_ignore(void);
uint32_t xemu_calltrace_ignore_count(void);

/* Hot path; called from the TCG helper on the vCPU thread. */
void xemu_calltrace_record(uint32_t call_site, uint32_t callee);

/* Data mode: like record(), plus a 6-dword argument snapshot. */
void xemu_calltrace_record_data(uint32_t call_site, uint32_t callee,
                                const uint32_t args[6]);

/*
 * Write the recording to <dir>/<Title>-<timestamp>.xct. Returns the
 * g_malloc'd output path on success (caller frees), or NULL with
 * *err_msg set to a g_malloc'd message (caller frees).
 */
char *xemu_calltrace_save(const char *dir, char **err_msg);

#ifdef __cplusplus
}
#endif

#endif

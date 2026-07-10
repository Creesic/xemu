/*
 * xemu guest call tracing
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
#include "xemu-calltrace.h"
#include "xemu-calltrace-map.h"

#define CT_KERNEL_SPACE 0x80000000u

bool xemu_calltrace_armed;

static CTMap ct_map;
static bool ct_truncated;

void xemu_calltrace_start(void)
{
    if (xemu_calltrace_armed) {
        return;
    }
    if (ct_map.slots) {
        ct_map_free(&ct_map);
    }
    if (!ct_map_init(&ct_map)) {
        return; /* allocation failed; stay disarmed */
    }
    ct_truncated = false;
    /* Arm before flushing so retranslated code is instrumented. */
    xemu_calltrace_armed = true;
    queue_tb_flush(qemu_get_cpu(0));
}

void xemu_calltrace_stop(void)
{
    if (!xemu_calltrace_armed) {
        return;
    }
    xemu_calltrace_armed = false;
    /* Map data intentionally retained until save or next start. */
    queue_tb_flush(qemu_get_cpu(0));
}

uint64_t xemu_calltrace_edge_count(void)
{
    return ct_map.slots ? ct_map.num_entries : 0;
}

bool xemu_calltrace_truncated(void)
{
    return ct_truncated;
}

void xemu_calltrace_record(uint32_t call_site, uint32_t callee)
{
    /* Stale TB may fire between disarm and flush completion. */
    if (!xemu_calltrace_armed) {
        return;
    }
    /* Skip kernel-internal calls; keep game<->kernel boundary edges. */
    if (call_site >= CT_KERNEL_SPACE && callee >= CT_KERNEL_SPACE) {
        return;
    }
    if (!ct_map_add(&ct_map, call_site, callee)) {
        ct_truncated = true;
    }
}

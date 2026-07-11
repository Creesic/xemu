/*
 * xemu call-trace argument-set table
 *
 * Per-edge table of up to CT_ARGSET_CAP distinct argument snapshots
 * (CT_ARGSET_DWORDS dwords each). Header-only and QEMU-independent for
 * standalone unit testing (tests/calltrace/test-calltrace-argsets.c).
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

#ifndef XEMU_CALLTRACE_ARGSETS_H
#define XEMU_CALLTRACE_ARGSETS_H

#include <stdint.h>
#include <string.h>

#define CT_ARGSET_DWORDS 6
#define CT_ARGSET_CAP 64           /* Data mode: distinct sets per edge      */
#define CT_ARGSET_CAP_EXTREME 512  /* Data Extreme mode                       */
#define CT_ARGSET_OVERFLOW 0xFFFFu /* set-index sentinel: snapshot not stored */

/*
 * Per-edge table of distinct argument snapshots. The `sets` buffer is
 * caller-allocated to `cap * CT_ARGSET_DWORDS` uint32_t and `cap` is set
 * before the first intern (so the same build can record with different caps
 * per mode). Index/nsets are 16-bit so caps above 255 (Extreme) fit; the
 * on-disk width is chosen from the cap (u8 when <=255, else u16).
 */
typedef struct CTEdgeArgs {
    uint16_t nsets;   /* 0..cap                                          */
    uint16_t cap;     /* active cap for this recording                   */
    uint32_t *sets;   /* caller-allocated: cap * CT_ARGSET_DWORDS words  */
} CTEdgeArgs;

/*
 * Intern a snapshot into this edge's table. Returns the set index
 * (0..cap-1) for a matched or newly-stored set, or CT_ARGSET_OVERFLOW when
 * the table is full and the snapshot is new (then not stored).
 */
static inline uint16_t ct_argset_intern(CTEdgeArgs *ea,
                                        const uint32_t args[CT_ARGSET_DWORDS])
{
    for (uint16_t i = 0; i < ea->nsets; i++) {
        if (memcmp(&ea->sets[(size_t)i * CT_ARGSET_DWORDS], args,
                   CT_ARGSET_DWORDS * sizeof(uint32_t)) == 0) {
            return i;
        }
    }
    if (ea->nsets >= ea->cap) {
        return (uint16_t)CT_ARGSET_OVERFLOW;
    }
    memcpy(&ea->sets[(size_t)ea->nsets * CT_ARGSET_DWORDS], args,
           CT_ARGSET_DWORDS * sizeof(uint32_t));
    return ea->nsets++;
}

#endif

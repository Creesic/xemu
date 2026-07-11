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
#define CT_ARGSET_CAP 64
#define CT_ARGSET_OVERFLOW 0xFFu

typedef struct CTEdgeArgs {
    uint8_t nsets;                                    /* 0..CT_ARGSET_CAP */
    uint32_t sets[CT_ARGSET_CAP][CT_ARGSET_DWORDS];
} CTEdgeArgs;

/*
 * Intern a snapshot into this edge's table. Returns the set index
 * (0..CT_ARGSET_CAP-1) for a matched or newly-stored set, or
 * CT_ARGSET_OVERFLOW when the table is full and the snapshot is new (the
 * snapshot is then not stored).
 */
static inline uint8_t ct_argset_intern(CTEdgeArgs *ea,
                                       const uint32_t args[CT_ARGSET_DWORDS])
{
    for (uint8_t i = 0; i < ea->nsets; i++) {
        if (memcmp(ea->sets[i], args,
                   CT_ARGSET_DWORDS * sizeof(uint32_t)) == 0) {
            return i;
        }
    }
    if (ea->nsets >= CT_ARGSET_CAP) {
        return (uint8_t)CT_ARGSET_OVERFLOW;
    }
    memcpy(ea->sets[ea->nsets], args, CT_ARGSET_DWORDS * sizeof(uint32_t));
    return ea->nsets++;
}

#endif

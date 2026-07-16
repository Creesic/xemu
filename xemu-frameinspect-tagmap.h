/*
 * xemu frame inspector: RAM-wide writer tag map
 *
 * One uint32_t per dword of guest RAM recording the call-path node that
 * last stored there. Tag 0 = unattributed; otherwise (node_id + 1), with
 * the high bit flagging a partially covered dword. Written on the vCPU
 * thread (release), read on the PFIFO thread (acquire). Header-only and
 * QEMU-independent for standalone unit testing.
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

#ifndef XEMU_FRAMEINSPECT_TAGMAP_H
#define XEMU_FRAMEINSPECT_TAGMAP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_TAG_PARTIAL 0x80000000u
#define FI_TAG_NODE(tag) ((((tag) & 0x7FFFFFFFu)) - 1u)

typedef struct FITagMap {
    uint32_t *tags;                   /* one per RAM dword */
    uint64_t ram_size;
} FITagMap;

static inline bool fi_tagmap_init(FITagMap *m, uint64_t ram_size)
{
    m->ram_size = ram_size;
    m->tags = calloc(ram_size / 4, sizeof(uint32_t));
    return m->tags != NULL;
}

static inline void fi_tagmap_free(FITagMap *m)
{
    free(m->tags);
    m->tags = NULL;
    m->ram_size = 0;
}

static inline void fi_tagmap_tag(FITagMap *m, uint64_t paddr, uint32_t len,
                                 uint32_t node)
{
    if (!m->tags || paddr >= m->ram_size) {
        return;
    }
    if (len > m->ram_size - paddr) {
        len = (uint32_t)(m->ram_size - paddr);
    }
    uint64_t end = paddr + len;
    for (uint64_t d = paddr & ~3ull; d < end; d += 4) {
        uint32_t tag = node + 1;
        if (d < paddr || d + 4 > end) {
            tag |= FI_TAG_PARTIAL;    /* store covers only part of dword */
        }
        __atomic_store_n(&m->tags[d >> 2], tag, __ATOMIC_RELEASE);
    }
}

static inline uint32_t fi_tagmap_lookup(const FITagMap *m, uint64_t paddr)
{
    if (!m->tags || paddr >= m->ram_size) {
        return 0;
    }
    return __atomic_load_n(&m->tags[paddr >> 2], __ATOMIC_ACQUIRE);
}

#endif

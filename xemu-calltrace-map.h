/*
 * xemu call-trace edge map
 *
 * Open-addressing hash map from (call_site, callee) address pairs to hit
 * counts. Header-only and free of QEMU dependencies so it can be unit
 * tested standalone (see tests/calltrace/test-calltrace-map.c).
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

#ifndef XEMU_CALLTRACE_MAP_H
#define XEMU_CALLTRACE_MAP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define CT_MAP_CAPACITY (1u << 21)    /* slots; power of two (32 MiB)   */
#define CT_MAP_MAX_ENTRIES (1u << 20) /* load-factor cap: 1M edges max  */

typedef struct CTEdge {
    uint64_t key; /* (call_site << 32) | callee; 0 = empty slot */
    uint64_t count;
} CTEdge;

typedef struct CTMap {
    CTEdge *slots;
    uint32_t num_entries;
} CTMap;

static inline bool ct_map_init(CTMap *m)
{
    m->slots = (CTEdge *)calloc(CT_MAP_CAPACITY, sizeof(CTEdge));
    m->num_entries = 0;
    return m->slots != NULL;
}

static inline void ct_map_free(CTMap *m)
{
    free(m->slots);
    m->slots = NULL;
    m->num_entries = 0;
}

static inline uint64_t ct_map_key(uint32_t call_site, uint32_t callee)
{
    return ((uint64_t)call_site << 32) | callee;
}

static inline uint32_t ct_map_hash(uint64_t key)
{
    /* Fibonacci hash; top 21 bits index the 2^21 slots */
    return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 43);
}

/*
 * Record one call. Returns false only when the map is full and the edge
 * is new (the edge is dropped); existing edges always keep counting.
 */
static inline bool ct_map_add(CTMap *m, uint32_t call_site, uint32_t callee)
{
    uint64_t key = ct_map_key(call_site, callee);
    if (key == 0) {
        return true; /* reserved empty sentinel; not a valid code address */
    }
    uint32_t idx = ct_map_hash(key) & (CT_MAP_CAPACITY - 1);
    for (;;) {
        CTEdge *e = &m->slots[idx];
        if (e->key == key) {
            e->count++;
            return true;
        }
        if (e->key == 0) {
            if (m->num_entries >= CT_MAP_MAX_ENTRIES) {
                return false;
            }
            e->key = key;
            e->count = 1;
            m->num_entries++;
            return true;
        }
        idx = (idx + 1) & (CT_MAP_CAPACITY - 1);
    }
}

#endif

/*
 * xemu frame inspector: surface-generation identity store
 *
 * Owns the surface generation store, providing capture-local generation ids
 * for surfaces seen at a given address, with rebind history and alias detection.
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
#ifndef XEMU_FRAMEINSPECT_SURFACES_H
#define XEMU_FRAMEINSPECT_SURFACES_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_SURFGEN_INVALID 0xFFFFFFFFu
#define FI_SURF_MAX_GENS   4096u

typedef struct FISurfaceKey {
    uint64_t addr;
    uint64_t size;
    uint32_t format;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t aa;
    uint32_t scale;   /* internal scale factor at capture time */
    uint8_t swizzle;
    uint8_t color;    /* 1 = colour surface, 0 = zeta */
    uint8_t z_format; /* fixed/float zeta interpretation */
} FISurfaceKey;

typedef struct FISurfaceGen {
    FISurfaceKey key;
    uint64_t extent;        /* addr + pitch*height (byte end, exclusive) */
    uint32_t generation;    /* per-addr rebind counter */
    uint32_t prev_at_addr;  /* previous gen id at same addr, or INVALID */
    bool alias;             /* overlaps a different-addr generation */
} FISurfaceGen;

typedef struct FISurfaceStore {
    FISurfaceGen *gens;     /* [FI_SURF_MAX_GENS] */
    uint32_t num_gens;
    bool truncated;
} FISurfaceStore;

static inline bool fi_surfaces_init(FISurfaceStore *s)
{
    memset(s, 0, sizeof(*s));
    s->gens = (FISurfaceGen *)calloc(FI_SURF_MAX_GENS, sizeof(FISurfaceGen));
    return s->gens != NULL;
}

static inline void fi_surfaces_free(FISurfaceStore *s)
{
    free(s->gens);
    memset(s, 0, sizeof(*s));
    s->gens = NULL;
}

static inline bool fi_surf_key_eq(const FISurfaceKey *a, const FISurfaceKey *b)
{
    return a->addr == b->addr && a->size == b->size &&
           a->format == b->format && a->pitch == b->pitch &&
           a->width == b->width && a->height == b->height &&
           a->aa == b->aa && a->scale == b->scale &&
           a->swizzle == b->swizzle && a->color == b->color &&
           a->z_format == b->z_format;
}

static inline uint32_t fi_surfaces_intern(FISurfaceStore *s,
                                          const FISurfaceKey *k)
{
    /* Reuse the most recent generation at this addr if its key is identical. */
    for (uint32_t i = s->num_gens; i-- > 0; ) {
        if (s->gens[i].key.addr == k->addr) {
            if (fi_surf_key_eq(&s->gens[i].key, k)) {
                return i;
            }
            break; /* newest gen at addr differs -> this is a rebind */
        }
    }
    if (s->num_gens >= FI_SURF_MAX_GENS) {
        s->truncated = true;
        return FI_SURFGEN_INVALID;
    }
    if (!k->size || k->addr > UINT64_MAX - k->size) {
        s->truncated = true;
        return FI_SURFGEN_INVALID;
    }
    uint32_t id = s->num_gens++;
    FISurfaceGen *g = &s->gens[id];
    memset(g, 0, sizeof(*g));
    g->key = *k;
    g->extent = k->addr + k->size;
    g->prev_at_addr = FI_SURFGEN_INVALID;
    g->generation = 0;
    for (uint32_t i = id; i-- > 0; ) {
        if (s->gens[i].key.addr == k->addr) {
            g->prev_at_addr = i;
            g->generation = s->gens[i].generation + 1;
            break;
        }
    }
    /* Alias detection: overlap in VRAM byte range with a different addr. */
    for (uint32_t i = 0; i < id; i++) {
        if (s->gens[i].key.addr == k->addr) {
            continue;
        }
        bool overlap = k->addr < s->gens[i].extent &&
                       s->gens[i].key.addr < g->extent;
        if (overlap) {
            g->alias = true;
            s->gens[i].alias = true;
        }
    }
    return id;
}

#endif

/*
 * xemu frame inspector: content-hash resource snapshot pool
 *
 * Owns the resource pool, providing deduplication by content hash of resource
 * snapshots (textures, palettes, vertex/index buffers, shaders, etc.) with
 * per-resource metadata tagging (format, dimensions, ...).
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
#ifndef XEMU_FRAMEINSPECT_RESOURCES_H
#define XEMU_FRAMEINSPECT_RESOURCES_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xemu-frameinspect-eventlog.h"

#define FI_RES_INVALID  0xFFFFFFFFu
#define FI_RES_MAX      65536u
#define FI_RES_HASH_CAP (1u << 17)   /* 2x FI_RES_MAX, power of two */
#define FI_RES_BLOB_CAP (256ull << 20)

typedef struct FIResource {
    uint32_t kind;
    uint32_t len;
    uint64_t off;
    uint64_t meta;
    uint64_t hash;
} FIResource;

typedef struct FIResourcePool {
    FIResource *res;      /* [FI_RES_MAX] */
    uint32_t num_res;
    uint8_t *blob;        /* growable up to FI_RES_BLOB_CAP */
    uint64_t blob_used, blob_cap;
    uint32_t *hash_slots; /* [FI_RES_HASH_CAP], value = id+1, 0 = empty */
    bool truncated;
} FIResourcePool;

static inline uint64_t fi_res_hash(uint32_t kind, const void *data,
                                   uint32_t len, uint64_t meta)
{
    /* FNV-1a over kind, meta, and bytes. */
    uint64_t h = 14695981039346656037ull;
    const uint8_t *pfx = (const uint8_t *)&kind;
    for (int i = 0; i < 4; i++) { h ^= pfx[i]; h *= 1099511628211ull; }
    const uint8_t *mpx = (const uint8_t *)&meta;
    for (int i = 0; i < 8; i++) { h ^= mpx[i]; h *= 1099511628211ull; }
    const uint8_t *d = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) { h ^= d[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

static inline bool fi_resources_init(FIResourcePool *p, FIBudget *budget)
{
    memset(p, 0, sizeof(*p));
    uint64_t res_bytes = (uint64_t)FI_RES_MAX * sizeof(FIResource);
    uint64_t hash_bytes = (uint64_t)FI_RES_HASH_CAP * sizeof(uint32_t);
    uint64_t blob_bytes = 1u << 20;
    uint64_t initial_bytes = res_bytes + hash_bytes + blob_bytes;
    if (!fi_budget_try(budget, initial_bytes)) {
        return false;
    }
    p->res = (FIResource *)calloc(FI_RES_MAX, sizeof(FIResource));
    p->hash_slots = (uint32_t *)calloc(FI_RES_HASH_CAP, sizeof(uint32_t));
    p->blob_cap = blob_bytes;
    p->blob = (uint8_t *)malloc((size_t)p->blob_cap);
    if (!p->res || !p->hash_slots || !p->blob) {
        free(p->res); free(p->hash_slots); free(p->blob);
        fi_budget_release(budget, initial_bytes);
        memset(p, 0, sizeof(*p));
        return false;
    }
    return true;
}

static inline void fi_resources_free(FIResourcePool *p, FIBudget *budget)
{
    if (p->res || p->hash_slots || p->blob) {
        uint64_t bytes = (uint64_t)FI_RES_MAX * sizeof(FIResource) +
                         (uint64_t)FI_RES_HASH_CAP * sizeof(uint32_t) +
                         p->blob_cap;
        fi_budget_release(budget, bytes);
    }
    free(p->res); free(p->hash_slots); free(p->blob);
    memset(p, 0, sizeof(*p));
    p->blob = NULL;
}

static inline uint32_t fi_resources_intern(FIResourcePool *p,
                                           FIBudget *budget, uint32_t kind,
                                           const void *data, uint32_t len,
                                           uint64_t meta)
{
    if (!p || !p->res || !p->hash_slots || !p->blob ||
        len > FI_RES_BLOB_CAP || (!data && len)) {
        if (p) p->truncated = true;
        return FI_RES_INVALID;
    }
    uint64_t h = fi_res_hash(kind, data, len, meta);
    uint32_t i = (uint32_t)h & (FI_RES_HASH_CAP - 1);
    for (;;) {
        uint32_t slot = p->hash_slots[i];
        if (slot == 0) break;
        FIResource *r = &p->res[slot - 1];
        if (r->hash == h && r->kind == kind && r->len == len &&
            r->meta == meta &&
            (!len || memcmp(p->blob + r->off, data, len) == 0)) {
            return slot - 1;
        }
        i = (i + 1) & (FI_RES_HASH_CAP - 1);
    }
    if (p->num_res >= FI_RES_MAX) { p->truncated = true; return FI_RES_INVALID; }
    uint64_t data_off = 0;
    uintptr_t data_addr = (uintptr_t)data;
    uintptr_t blob_addr = (uintptr_t)p->blob;
    bool aliases_blob = len && data_addr >= blob_addr &&
                        data_addr - blob_addr <= p->blob_used &&
                        len <= p->blob_used - (data_addr - blob_addr);
    if (aliases_blob) {
        data_off = data_addr - blob_addr;
    }
    while ((uint64_t)len > p->blob_cap - p->blob_used) {
        uint64_t nc = p->blob_cap * 2;
        if (nc > FI_RES_BLOB_CAP) { p->truncated = true; return FI_RES_INVALID; }
        if (!fi_budget_try(budget, nc - p->blob_cap)) {
            p->truncated = true;
            return FI_RES_INVALID;
        }
        uint8_t *nb = (uint8_t *)realloc(p->blob, (size_t)nc);
        if (!nb) {
            fi_budget_release(budget, nc - p->blob_cap);
            p->truncated = true;
            return FI_RES_INVALID;
        }
        p->blob = nb; p->blob_cap = nc;
        if (aliases_blob) {
            data = p->blob + data_off;
        }
    }
    uint32_t id = p->num_res++;
    FIResource *r = &p->res[id];
    r->kind = kind; r->len = len; r->meta = meta; r->hash = h;
    r->off = p->blob_used;
    if (len) memcpy(p->blob + r->off, data, len);
    p->blob_used += len;
    p->hash_slots[i] = id + 1;
    return id;
}

#endif

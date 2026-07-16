/*
 * xemu frame inspector: interned call-path tree + argument-set pool
 *
 * Path nodes are interned by (parent, call_site, callee) so one node
 * represents one distinct call path. Argument sets (6 dwords captured at
 * call time) are interned in a global pool and referenced by id, so
 * varying arguments never duplicate path structure. Header-only and
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

#ifndef XEMU_FRAMEINSPECT_CALLTREE_H
#define XEMU_FRAMEINSPECT_CALLTREE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_CT_MAX_NODES   (1u << 19)  /* 512 K path nodes */
#define FI_CT_HASH_CAP    (1u << 20)  /* open-addressed slots (2x nodes) */
#define FI_CT_MAX_ARGSETS (1u << 18)  /* 256 K distinct argument sets */
#define FI_CT_ARGHASH_CAP (1u << 19)
#define FI_NODE_ARGSETS   4           /* argset-id slots per node */

#define FI_NODE_INVALID 0xFFFFFFFFu
#define FI_NODE_ROOT    0u            /* the unknown root */

typedef struct FINode {
    uint32_t parent;                  /* node id or FI_NODE_INVALID */
    uint32_t call_site;
    uint32_t callee;
    uint16_t n_argsets;
    uint16_t argsets_truncated;
    uint32_t argset_ids[FI_NODE_ARGSETS];
} FINode;

typedef struct FICallTree {
    FINode *nodes;                    /* [FI_CT_MAX_NODES] */
    uint32_t num_nodes;
    uint64_t *hash_keys;              /* [FI_CT_HASH_CAP], 0 = empty */
    uint32_t *hash_vals;
    uint32_t (*argsets)[6];           /* [FI_CT_MAX_ARGSETS] */
    uint32_t num_argsets;
    uint64_t *arg_keys;               /* [FI_CT_ARGHASH_CAP], 0 = empty */
    uint32_t *arg_vals;
    bool nodes_truncated;
    bool pool_truncated;
} FICallTree;

/* 64-bit avalanche mix (splitmix64 finalizer); | 1 reserves 0 = empty. */
static inline uint64_t fi_ct_mix(uint64_t x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x | 1;
}

static inline bool fi_calltree_init(FICallTree *t)
{
    memset(t, 0, sizeof(*t));
    t->nodes = calloc(FI_CT_MAX_NODES, sizeof(FINode));
    t->hash_keys = calloc(FI_CT_HASH_CAP, sizeof(uint64_t));
    t->hash_vals = calloc(FI_CT_HASH_CAP, sizeof(uint32_t));
    t->argsets = calloc(FI_CT_MAX_ARGSETS, sizeof(*t->argsets));
    t->arg_keys = calloc(FI_CT_ARGHASH_CAP, sizeof(uint64_t));
    t->arg_vals = calloc(FI_CT_ARGHASH_CAP, sizeof(uint32_t));
    if (!t->nodes || !t->hash_keys || !t->hash_vals || !t->argsets ||
        !t->arg_keys || !t->arg_vals) {
        free(t->nodes); free(t->hash_keys); free(t->hash_vals);
        free(t->argsets); free(t->arg_keys); free(t->arg_vals);
        memset(t, 0, sizeof(*t));
        return false;
    }
    t->nodes[FI_NODE_ROOT].parent = FI_NODE_INVALID;
    t->num_nodes = 1;
    return true;
}

static inline void fi_calltree_free(FICallTree *t)
{
    free(t->nodes); free(t->hash_keys); free(t->hash_vals);
    free(t->argsets); free(t->arg_keys); free(t->arg_vals);
    memset(t, 0, sizeof(*t));
    t->nodes = NULL;
}

static inline uint32_t fi_calltree_intern(FICallTree *t, uint32_t parent,
                                          uint32_t call_site, uint32_t callee)
{
    uint64_t key = fi_ct_mix(((uint64_t)parent << 32) ^
                             ((uint64_t)call_site << 16) ^ callee);
    uint32_t i = (uint32_t)key & (FI_CT_HASH_CAP - 1);
    for (;;) {
        if (t->hash_keys[i] == key) {
            FINode *n = &t->nodes[t->hash_vals[i]];
            if (n->parent == parent && n->call_site == call_site &&
                n->callee == callee) {
                return t->hash_vals[i];
            }
        } else if (t->hash_keys[i] == 0) {
            break;
        }
        i = (i + 1) & (FI_CT_HASH_CAP - 1);
    }
    if (t->num_nodes >= FI_CT_MAX_NODES) {
        t->nodes_truncated = true;
        return FI_NODE_INVALID;
    }
    uint32_t id = t->num_nodes++;
    FINode *n = &t->nodes[id];
    n->parent = parent;
    n->call_site = call_site;
    n->callee = callee;
    t->hash_keys[i] = key;
    t->hash_vals[i] = id;
    return id;
}

static inline void fi_calltree_add_args(FICallTree *t, uint32_t node,
                                        const uint32_t args[6])
{
    if (node >= t->num_nodes) {
        return;
    }
    uint64_t key = fi_ct_mix(((uint64_t)args[0] << 32) ^ args[1]) ^
                   fi_ct_mix(((uint64_t)args[2] << 32) ^ args[3]) ^
                   fi_ct_mix(((uint64_t)args[4] << 32) ^ args[5]);
    key |= 1;
    uint32_t i = (uint32_t)key & (FI_CT_ARGHASH_CAP - 1);
    uint32_t id = FI_NODE_INVALID;
    for (;;) {
        if (t->arg_keys[i] == key &&
            memcmp(t->argsets[t->arg_vals[i]], args, 24) == 0) {
            id = t->arg_vals[i];
            break;
        }
        if (t->arg_keys[i] == 0) {
            if (t->num_argsets >= FI_CT_MAX_ARGSETS) {
                t->pool_truncated = true;
                return;
            }
            id = t->num_argsets++;
            memcpy(t->argsets[id], args, 24);
            t->arg_keys[i] = key;
            t->arg_vals[i] = id;
            break;
        }
        i = (i + 1) & (FI_CT_ARGHASH_CAP - 1);
    }
    FINode *n = &t->nodes[node];
    for (uint32_t k = 0; k < n->n_argsets; k++) {
        if (n->argset_ids[k] == id) {
            return;                   /* already attached */
        }
    }
    if (n->n_argsets < FI_NODE_ARGSETS) {
        n->argset_ids[n->n_argsets++] = id;
    } else {
        n->argsets_truncated = 1;
    }
}

#endif

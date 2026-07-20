/*
 * xemu frame inspector: immutable guest origin snapshot
 *
 * Compact, QEMU-independent storage for the call-path nodes and argument sets
 * retained by a finalized frame capture. The live call tree's hash tables are
 * deliberately excluded. Synchronizing with live call-tree mutation is the
 * caller's responsibility.
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
#ifndef XEMU_FRAMEINSPECT_ORIGIN_H
#define XEMU_FRAMEINSPECT_ORIGIN_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_ORIGIN_NODE_INVALID 0xFFFFFFFFu
#define FI_ORIGIN_NODE_ROOT    0u
#define FI_ORIGIN_NODE_ARGSETS 4u

enum {
    FI_ORIGIN_TRUNC_NODES = 1u << 0,
    FI_ORIGIN_TRUNC_ARGSETS = 1u << 1,
    FI_ORIGIN_TRUNC_BUDGET = 1u << 2,
    FI_ORIGIN_TRUNC_INVALID = 1u << 3,
};

typedef struct FIOriginNode {
    uint32_t parent;
    uint32_t call_site;
    uint32_t callee;
    uint16_t n_argsets;
    uint16_t argsets_truncated;
    uint32_t argset_ids[FI_ORIGIN_NODE_ARGSETS];
} FIOriginNode;

typedef struct FIOriginSnapshot {
    FIOriginNode *nodes;
    uint32_t num_nodes;
    uint32_t (*argsets)[6];
    uint32_t num_argsets;
    uint64_t bytes;
    uint32_t truncation;
} FIOriginSnapshot;

static inline void fi_origin_snapshot_init(FIOriginSnapshot *s)
{
    memset(s, 0, sizeof(*s));
}

static inline void fi_origin_snapshot_free(FIOriginSnapshot *s)
{
    free(s->nodes);
    free(s->argsets);
    memset(s, 0, sizeof(*s));
}

/* Copy a complete dense node array and argument-set pool. Publishing a
 * partial copy would leave parent and argset IDs dangling, so budget or
 * allocation failure produces an empty, explicitly truncated snapshot. */
static inline bool fi_origin_snapshot_copy(
    FIOriginSnapshot *dst, const FIOriginNode *nodes, uint32_t num_nodes,
    const uint32_t (*argsets)[6], uint32_t num_argsets, uint64_t byte_limit,
    uint32_t source_truncation)
{
    if (!dst || (num_nodes && !nodes) || (num_argsets && !argsets)) {
        if (dst) {
            fi_origin_snapshot_init(dst);
            dst->truncation = source_truncation | FI_ORIGIN_TRUNC_INVALID;
        }
        return false;
    }

    fi_origin_snapshot_init(dst);
    dst->truncation = source_truncation;

    uint64_t node_bytes = (uint64_t)num_nodes * sizeof(FIOriginNode);
    uint64_t argset_bytes = (uint64_t)num_argsets * sizeof(*argsets);
    if (node_bytes > UINT64_MAX - argset_bytes) {
        dst->truncation |= FI_ORIGIN_TRUNC_INVALID;
        return false;
    }
    uint64_t total = node_bytes + argset_bytes;
    if (total > byte_limit || node_bytes > SIZE_MAX || argset_bytes > SIZE_MAX) {
        dst->truncation |= FI_ORIGIN_TRUNC_BUDGET;
        return false;
    }

    FIOriginNode *node_copy = NULL;
    uint32_t (*argset_copy)[6] = NULL;
    if (node_bytes) {
        node_copy = (FIOriginNode *)malloc((size_t)node_bytes);
    }
    if (argset_bytes) {
        argset_copy = (uint32_t (*)[6])malloc((size_t)argset_bytes);
    }
    if ((node_bytes && !node_copy) || (argset_bytes && !argset_copy)) {
        free(node_copy);
        free(argset_copy);
        dst->truncation |= FI_ORIGIN_TRUNC_BUDGET;
        return false;
    }

    if (node_bytes) {
        memcpy(node_copy, nodes, (size_t)node_bytes);
    }
    if (argset_bytes) {
        memcpy(argset_copy, argsets, (size_t)argset_bytes);
    }
    dst->nodes = node_copy;
    dst->num_nodes = num_nodes;
    dst->argsets = argset_copy;
    dst->num_argsets = num_argsets;
    dst->bytes = total;
    return true;
}

static inline const FIOriginNode *fi_origin_snapshot_node(
    const FIOriginSnapshot *s, uint32_t node_id)
{
    return s && s->nodes && node_id < s->num_nodes ? &s->nodes[node_id] : NULL;
}

static inline const uint32_t *fi_origin_snapshot_argset(
    const FIOriginSnapshot *s, const FIOriginNode *node, uint32_t index)
{
    if (!s || !node || index >= node->n_argsets ||
        index >= FI_ORIGIN_NODE_ARGSETS) {
        return NULL;
    }
    uint32_t id = node->argset_ids[index];
    return s->argsets && id < s->num_argsets ? s->argsets[id] : NULL;
}

#endif

/*
 * xemu frame inspector: per-guest-thread shadow call stacks
 *
 * Tracks the live call path per guest thread (keyed by KTHREAD pointer)
 * from CALL/RET instrumentation. RETs pop by matching the recorded
 * return address (bounded search); mismatches and ESP discontinuities
 * reset toward the unknown root so attribution degrades to missing,
 * never fabricated. Watched frames propagate a watch depth so store
 * instrumentation knows when it is inside a watched subtree.
 * Header-only and QEMU-independent for standalone unit testing.
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

#ifndef XEMU_FRAMEINSPECT_SHADOWSTACK_H
#define XEMU_FRAMEINSPECT_SHADOWSTACK_H

#include "xemu-frameinspect-calltree.h"

#define FI_SS_MAX_THREADS 64
#define FI_SS_MAX_DEPTH   512
#define FI_SS_MAX_POPS    64
#define FI_INVOC_INVALID  0xFFFFFFFFu

typedef struct FIFrame {
    uint32_t node;
    uint32_t call_site;
    uint32_t callee;
    uint32_t args[6];
    uint32_t expected_ret;
    uint32_t esp_at_call;             /* ESP before the CALL pushed */
    uint32_t watch_invoc;             /* FI_INVOC_INVALID if unwatched */
} FIFrame;

typedef struct FIStack {
    uint32_t thread_key;              /* 0 = slot unused */
    uint32_t depth;
    uint32_t watch_depth;             /* frames at/below a watched frame */
    FIFrame frames[FI_SS_MAX_DEPTH];
} FIStack;

typedef struct FIPopped {
    uint32_t watch_invoc;
    bool clean;                       /* popped by its matching RET */
} FIPopped;

typedef struct FIShadow {
    FICallTree *tree;
    FIStack stacks[FI_SS_MAX_THREADS];
    uint32_t n_stacks;
    uint32_t last_idx;                /* single-hot-thread cache */
    bool threads_truncated;
} FIShadow;

static inline void fi_shadow_init(FIShadow *s, FICallTree *tree)
{
    memset(s, 0, sizeof(*s));
    s->tree = tree;
}

static inline FIStack *fi_shadow_stack(FIShadow *s, uint32_t thread_key)
{
    if (s->n_stacks && s->stacks[s->last_idx].thread_key == thread_key) {
        return &s->stacks[s->last_idx];
    }
    for (uint32_t i = 0; i < s->n_stacks; i++) {
        if (s->stacks[i].thread_key == thread_key) {
            s->last_idx = i;
            return &s->stacks[i];
        }
    }
    if (s->n_stacks >= FI_SS_MAX_THREADS) {
        s->threads_truncated = true;
        return NULL;
    }
    FIStack *st = &s->stacks[s->n_stacks];
    st->thread_key = thread_key;
    st->depth = 0;
    st->watch_depth = 0;
    s->last_idx = s->n_stacks++;
    return st;
}

static inline void fi_ss_pop_one(FIStack *st, bool clean,
                                 FIPopped *popped, uint32_t *n_popped)
{
    FIFrame *f = &st->frames[--st->depth];
    if (st->watch_depth) {
        st->watch_depth--;
    }
    if (f->watch_invoc != FI_INVOC_INVALID && popped &&
        *n_popped < FI_SS_MAX_POPS) {
        popped[*n_popped].watch_invoc = f->watch_invoc;
        popped[*n_popped].clean = clean;
        (*n_popped)++;
    }
}

static inline uint32_t fi_shadow_current(FIShadow *s, uint32_t thread_key)
{
    FIStack *st = fi_shadow_stack(s, thread_key);
    if (!st || st->depth == 0) {
        return FI_NODE_ROOT;
    }
    return st->frames[st->depth - 1].node;
}

static inline const FIFrame *fi_shadow_current_frame(FIShadow *s,
                                                      uint32_t thread_key)
{
    FIStack *st = fi_shadow_stack(s, thread_key);
    return st && st->depth ? &st->frames[st->depth - 1] : NULL;
}

static inline void fi_shadow_add_args(FICallTree *tree, uint32_t node,
                                      const uint32_t args[6])
{
    if (node >= tree->num_nodes) {
        return;
    }
    FINode *n = &tree->nodes[node];
    for (uint32_t i = 0; i < n->n_argsets; i++) {
        uint32_t id = n->argset_ids[i];
        if (id < tree->num_argsets &&
            memcmp(tree->argsets[id], args, sizeof(uint32_t) * 6) == 0) {
            return;
        }
    }
    if (n->n_argsets >= FI_NODE_ARGSETS || n->argsets_truncated) {
        n->argsets_truncated = 1;
        return;
    }
    uint16_t old_count = n->n_argsets;
    fi_calltree_add_args(tree, node, args);
    if (tree->pool_truncated && n->n_argsets == old_count) {
        n->argsets_truncated = 1;
    }
}

/* Materialize only paths that reach a RAM store. Each live frame caches its
 * resulting immutable node, so repeated stores under the same path are O(1). */
static inline uint32_t fi_shadow_materialize(FIShadow *s, uint32_t thread_key)
{
    FIStack *st = fi_shadow_stack(s, thread_key);
    if (!st || st->depth == 0) {
        return FI_NODE_ROOT;
    }
    uint32_t parent = FI_NODE_ROOT;
    for (uint32_t i = 0; i < st->depth; i++) {
        FIFrame *f = &st->frames[i];
        if (f->node == FI_NODE_ROOT) {
            for (uint32_t j = i + 1; j < st->depth; j++) {
                st->frames[j].node = FI_NODE_ROOT;
            }
            return FI_NODE_ROOT;
        }
        if (f->node == FI_NODE_INVALID) {
            uint32_t node = fi_calltree_intern(s->tree, parent, f->call_site,
                                               f->callee);
            if (node == FI_NODE_INVALID) {
                for (uint32_t j = i; j < st->depth; j++) {
                    st->frames[j].node = FI_NODE_ROOT;
                }
                return FI_NODE_ROOT;
            }
            f->node = node;
            fi_shadow_add_args(s->tree, node, f->args);
        }
        parent = f->node;
    }
    return parent;
}

static inline uint32_t fi_shadow_call_lazy(FIShadow *s, uint32_t thread_key,
                                            uint32_t call_site,
                                            uint32_t callee,
                                            uint32_t ret_addr, uint32_t esp,
                                            const uint32_t args[6])
{
    FIStack *st = fi_shadow_stack(s, thread_key);
    if (!st) {
        return FI_NODE_ROOT;
    }
    while (st->depth > 0 && esp > st->frames[st->depth - 1].esp_at_call) {
        fi_ss_pop_one(st, false, NULL, NULL);
    }
    if (st->depth < FI_SS_MAX_DEPTH) {
        FIFrame *f = &st->frames[st->depth++];
        f->node = FI_NODE_INVALID;
        f->call_site = call_site;
        f->callee = callee;
        memcpy(f->args, args, sizeof(f->args));
        f->expected_ret = ret_addr;
        f->esp_at_call = esp;
        f->watch_invoc = FI_INVOC_INVALID;
        if (st->watch_depth) {
            st->watch_depth++;
        }
    }
    return FI_NODE_ROOT;
}

static inline uint32_t fi_shadow_call(FIShadow *s, uint32_t thread_key,
                                      uint32_t call_site, uint32_t callee,
                                      uint32_t ret_addr, uint32_t esp)
{
    FIStack *st = fi_shadow_stack(s, thread_key);
    if (!st) {
        return FI_NODE_ROOT;
    }
    /* ESP discontinuity: frames whose ESP is below the caller's current
     * ESP were unwound without RETs (longjmp/exception). Pop them dead. */
    while (st->depth > 0 && esp > st->frames[st->depth - 1].esp_at_call) {
        fi_ss_pop_one(st, false, NULL, NULL);
    }
    uint32_t parent = st->depth ? st->frames[st->depth - 1].node
                                : FI_NODE_ROOT;
    uint32_t node = fi_calltree_intern(s->tree, parent, call_site, callee);
    if (node == FI_NODE_INVALID) {
        node = FI_NODE_ROOT;          /* tree capped: attribute to root */
    }
    if (st->depth < FI_SS_MAX_DEPTH) {
        FIFrame *f = &st->frames[st->depth++];
        f->node = node;
        f->call_site = call_site;
        f->callee = callee;
        memset(f->args, 0, sizeof(f->args));
        f->expected_ret = ret_addr;
        f->esp_at_call = esp;
        f->watch_invoc = FI_INVOC_INVALID;
        if (st->watch_depth) {
            st->watch_depth++;
        }
    }
    return node;
}

static inline uint32_t fi_shadow_ret(FIShadow *s, uint32_t thread_key,
                                     uint32_t ret_target,
                                     FIPopped popped[], uint32_t *n_popped)
{
    *n_popped = 0;
    FIStack *st = fi_shadow_stack(s, thread_key);
    if (!st || st->depth == 0) {
        return FI_NODE_ROOT;
    }
    uint32_t limit = st->depth < FI_SS_MAX_POPS ? st->depth : FI_SS_MAX_POPS;
    for (uint32_t k = 0; k < limit; k++) {
        if (st->frames[st->depth - 1 - k].expected_ret == ret_target) {
            for (uint32_t j = 0; j < k; j++) {       /* skipped frames */
                fi_ss_pop_one(st, false, popped, n_popped);
            }
            fi_ss_pop_one(st, true, popped, n_popped);
            return st->depth ? st->frames[st->depth - 1].node : FI_NODE_ROOT;
        }
    }
    /* No match within bound: desync. Reset; report all watched frames. */
    while (st->depth > 0) {
        fi_ss_pop_one(st, false, popped, n_popped);
    }
    st->watch_depth = 0;
    return FI_NODE_ROOT;
}

static inline void fi_shadow_set_watch(FIShadow *s, uint32_t thread_key,
                                       uint32_t invoc)
{
    FIStack *st = fi_shadow_stack(s, thread_key);
    if (st && st->depth > 0) {
        st->frames[st->depth - 1].watch_invoc = invoc;
        if (!st->watch_depth) {
            st->watch_depth = 1;
        }
    }
}

static inline bool fi_shadow_in_watch(FIShadow *s, uint32_t thread_key)
{
    FIStack *st = fi_shadow_stack(s, thread_key);
    return st && st->watch_depth > 0;
}

#endif

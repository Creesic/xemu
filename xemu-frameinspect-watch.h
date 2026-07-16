/*
 * xemu frame inspector: watched-call engine
 *
 * A small set of watched callee addresses; each invocation records entry
 * registers, 16 stack dwords, and completion state, plus a write log of
 * (addr, old bytes, new bytes) for stores made inside the watched
 * subtree. Contiguous writes from one invocation coalesce into range
 * records (REP-friendly). Header-only and QEMU-independent for
 * standalone unit testing.
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

#ifndef XEMU_FRAMEINSPECT_WATCH_H
#define XEMU_FRAMEINSPECT_WATCH_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef FI_INVOC_INVALID
#define FI_INVOC_INVALID 0xFFFFFFFFu
#endif

#define FI_WATCH_MAX       16
#define FI_WATCH_MAX_INVOC 1024                /* per watch */
#define FI_WLOG_MAX_RECS   (1u << 20)
#define FI_WLOG_POOL_BYTES (24u << 20)

enum { FI_INVOC_OPEN = 0, FI_INVOC_COMPLETE = 1, FI_INVOC_INCOMPLETE = 2 };

typedef struct FIInvocation {
    uint32_t watch_idx;
    uint32_t order;
    uint32_t node;
    uint32_t regs[8];                          /* EAX..EDI, R_EAX order */
    uint32_t esp;
    uint32_t stack16[16];
    uint32_t eax_ret;
    uint32_t mmio_writes;
    uint8_t state;
} FIInvocation;

typedef struct FIWriteRec {
    uint32_t addr;
    uint32_t len;
    uint32_t old_off;                          /* offset into pool */
    uint32_t new_off;
    uint32_t invoc;
} FIWriteRec;

typedef struct FIWatchEngine {
    uint32_t watches[FI_WATCH_MAX];            /* 0 = slot unused */
    uint32_t invoc_count[FI_WATCH_MAX];
    FIInvocation *invocs;                      /* [FI_WATCH_MAX*FI_WATCH_MAX_INVOC] */
    uint32_t num_invocs;
    FIWriteRec *recs;                          /* [FI_WLOG_MAX_RECS] */
    uint32_t num_recs;
    uint8_t *pool;                             /* [FI_WLOG_POOL_BYTES] */
    uint32_t pool_used;
    bool invocs_truncated;
    bool log_truncated;
} FIWatchEngine;

static inline bool fi_watch_init(FIWatchEngine *w)
{
    memset(w, 0, sizeof(*w));
    w->invocs = calloc(FI_WATCH_MAX * FI_WATCH_MAX_INVOC,
                       sizeof(FIInvocation));
    w->recs = calloc(FI_WLOG_MAX_RECS, sizeof(FIWriteRec));
    w->pool = malloc(FI_WLOG_POOL_BYTES);
    if (!w->invocs || !w->recs || !w->pool) {
        free(w->invocs); free(w->recs); free(w->pool);
        memset(w, 0, sizeof(*w));
        return false;
    }
    return true;
}

static inline void fi_watch_deinit(FIWatchEngine *w)
{
    free(w->invocs); free(w->recs); free(w->pool);
    memset(w, 0, sizeof(*w));
    w->recs = NULL;
}

static inline int fi_watch_find(const FIWatchEngine *w, uint32_t callee)
{
    for (int i = 0; i < FI_WATCH_MAX; i++) {
        if (w->watches[i] == callee && callee != 0) {
            return i;
        }
    }
    return -1;
}

static inline bool fi_watch_add(FIWatchEngine *w, uint32_t callee)
{
    if (callee == 0 || fi_watch_find(w, callee) >= 0) {
        return callee != 0;
    }
    for (int i = 0; i < FI_WATCH_MAX; i++) {
        if (w->watches[i] == 0) {
            w->watches[i] = callee;
            w->invoc_count[i] = 0;
            return true;
        }
    }
    return false;
}

static inline void fi_watch_remove(FIWatchEngine *w, uint32_t callee)
{
    int i = fi_watch_find(w, callee);
    if (i >= 0) {
        w->watches[i] = 0;
    }
}

static inline uint32_t fi_watch_enter(FIWatchEngine *w, int watch_idx,
                                      uint32_t node, const uint32_t regs[8],
                                      uint32_t esp, const uint32_t stack16[16])
{
    if (!w->invocs || watch_idx < 0 ||
        w->invoc_count[watch_idx] >= FI_WATCH_MAX_INVOC ||
        w->num_invocs >= FI_WATCH_MAX * FI_WATCH_MAX_INVOC) {
        w->invocs_truncated = true;
        return FI_INVOC_INVALID;
    }
    uint32_t id = w->num_invocs++;
    FIInvocation *iv = &w->invocs[id];
    iv->watch_idx = (uint32_t)watch_idx;
    iv->order = w->invoc_count[watch_idx]++;
    iv->node = node;
    memcpy(iv->regs, regs, sizeof(iv->regs));
    iv->esp = esp;
    memcpy(iv->stack16, stack16, sizeof(iv->stack16));
    iv->state = FI_INVOC_OPEN;
    return id;
}

static inline void fi_watch_exit(FIWatchEngine *w, uint32_t invoc,
                                 bool clean, uint32_t eax)
{
    if (invoc >= w->num_invocs || w->invocs[invoc].state != FI_INVOC_OPEN) {
        return;
    }
    if (clean) {
        w->invocs[invoc].state = FI_INVOC_COMPLETE;
        w->invocs[invoc].eax_ret = eax;
    } else {
        w->invocs[invoc].state = FI_INVOC_INCOMPLETE;
    }
}

static inline void fi_watch_log_write(FIWatchEngine *w, uint32_t invoc,
                                      uint32_t addr, const uint8_t *old_bytes,
                                      const uint8_t *new_bytes, uint32_t len)
{
    if (!w->recs || invoc == FI_INVOC_INVALID) {
        return;
    }
    /* Coalesce with the previous record when this write extends it.
     * Invariant: the most recent record's runs sit at the pool tail as
     * [old run L][new run L] with new_off == old_off + L and
     * pool_used == new_off + L. */
    if (w->num_recs > 0) {
        FIWriteRec *p = &w->recs[w->num_recs - 1];
        if (p->invoc == invoc && p->addr + p->len == addr &&
            p->old_off + p->len == p->new_off &&
            w->pool_used == p->new_off + p->len &&
            w->pool_used + 2 * len <= FI_WLOG_POOL_BYTES) {
            uint32_t L = p->len;
            /* shift the new run up by len to open room for old bytes */
            memmove(&w->pool[p->new_off + len], &w->pool[p->new_off], L);
            memcpy(&w->pool[p->old_off + L], old_bytes, len);
            p->new_off += len;
            memcpy(&w->pool[p->new_off + L], new_bytes, len);
            p->len = L + len;
            w->pool_used = p->new_off + p->len;
            return;
        }
    }
    if (w->num_recs >= FI_WLOG_MAX_RECS ||
        w->pool_used + 2 * len > FI_WLOG_POOL_BYTES) {
        w->log_truncated = true;
        return;
    }
    FIWriteRec *r = &w->recs[w->num_recs++];
    r->addr = addr;
    r->len = len;
    r->invoc = invoc;
    r->old_off = w->pool_used;
    memcpy(&w->pool[r->old_off], old_bytes, len);
    r->new_off = w->pool_used + len;
    memcpy(&w->pool[r->new_off], new_bytes, len);
    w->pool_used += 2 * len;
}

static inline void fi_watch_count_mmio(FIWatchEngine *w, uint32_t invoc)
{
    if (invoc < w->num_invocs) {
        w->invocs[invoc].mmio_writes++;
    }
}

#endif

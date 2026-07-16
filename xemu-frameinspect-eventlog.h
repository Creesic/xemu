/*
 * xemu frame inspector: capture event log + byte budget
 *
 * Owns the capture event log and budget tracking, providing per-surface,
 * per-batch operation logging with configurable capacity and budget limits.
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
#ifndef XEMU_FRAMEINSPECT_EVENTLOG_H
#define XEMU_FRAMEINSPECT_EVENTLOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_EVENT_INVALID 0xFFFFFFFFu
#define FI_EVENTLOG_CAP  (1u << 20)

enum {
    FI_EV_METHOD = 0,
    FI_EV_BATCH,
    FI_EV_CLEAR,
    FI_EV_BLIT,
    FI_EV_UPLOAD,
    FI_EV_SCANOUT,
};

typedef struct FIEvent {
    uint8_t kind;
    uint32_t surface_gen;
    uint32_t seq;
    uint32_t a0, a1, a2, a3;
} FIEvent;

typedef struct FIEventLog {
    FIEvent *events;
    uint32_t count, cap;
    bool truncated;
} FIEventLog;

typedef struct FIBudget {
    uint64_t limit;
    uint64_t used;
} FIBudget;

static inline bool fi_eventlog_init(FIEventLog *l)
{
    memset(l, 0, sizeof(*l));
    l->cap = 4096;
    l->events = (FIEvent *)malloc(l->cap * sizeof(FIEvent));
    return l->events != NULL;
}

static inline void fi_eventlog_free(FIEventLog *l)
{
    free(l->events);
    memset(l, 0, sizeof(*l));
    l->events = NULL;
}

static inline uint32_t fi_eventlog_append(FIEventLog *l, uint8_t kind,
                                          uint32_t surface_gen, uint32_t a0,
                                          uint32_t a1, uint32_t a2, uint32_t a3)
{
    if (l->count >= FI_EVENTLOG_CAP) { l->truncated = true; return FI_EVENT_INVALID; }
    if (l->count >= l->cap) {
        uint32_t nc = l->cap * 2;
        if (nc > FI_EVENTLOG_CAP) nc = FI_EVENTLOG_CAP;
        FIEvent *ne = (FIEvent *)realloc(l->events, nc * sizeof(FIEvent));
        if (!ne) { l->truncated = true; return FI_EVENT_INVALID; }
        l->events = ne; l->cap = nc;
    }
    uint32_t idx = l->count++;
    FIEvent *e = &l->events[idx];
    e->kind = kind; e->surface_gen = surface_gen; e->seq = idx;
    e->a0 = a0; e->a1 = a1; e->a2 = a2; e->a3 = a3;
    return idx;
}

static inline bool fi_budget_try(FIBudget *b, uint64_t bytes)
{
    if (b->used + bytes > b->limit) return false;
    b->used += bytes;
    return true;
}

#endif

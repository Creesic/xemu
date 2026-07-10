/*
 * xemu call-trace event log
 *
 * Append-only, chunked buffer of u32 edge indices (one per recorded call,
 * in execution order). Header-only and QEMU-independent for standalone
 * unit testing (tests/calltrace/test-calltrace-events.c).
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

#ifndef XEMU_CALLTRACE_EVENTS_H
#define XEMU_CALLTRACE_EVENTS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef CT_EVENT_CHUNK
#define CT_EVENT_CHUNK (1u << 20) /* u32 events per chunk (4 MiB) */
#endif
#ifndef CT_EVENT_CAP
#define CT_EVENT_CAP 20000000u    /* max events (~80 MB) before truncation */
#endif

typedef struct CTEventChunk {
    struct CTEventChunk *next;
    uint32_t fill;                 /* used entries in data */
    uint32_t data[CT_EVENT_CHUNK];
} CTEventChunk;

typedef struct CTEvents {
    CTEventChunk *head;
    CTEventChunk *tail;
    uint64_t count;                /* total appended events */
} CTEvents;

static inline void ct_events_init(CTEvents *e)
{
    e->head = e->tail = NULL;
    e->count = 0;
}

static inline void ct_events_free(CTEvents *e)
{
    CTEventChunk *c = e->head;
    while (c) {
        CTEventChunk *n = c->next;
        free(c);
        c = n;
    }
    e->head = e->tail = NULL;
    e->count = 0;
}

/* Append one event. Returns false (and drops it) once the cap is reached. */
static inline bool ct_events_append(CTEvents *e, uint32_t v)
{
    if (e->count >= CT_EVENT_CAP) {
        return false;
    }
    if (!e->tail || e->tail->fill == CT_EVENT_CHUNK) {
        CTEventChunk *c = (CTEventChunk *)malloc(sizeof(CTEventChunk));
        if (!c) {
            return false;
        }
        c->next = NULL;
        c->fill = 0;
        if (e->tail) {
            e->tail->next = c;
        } else {
            e->head = c;
        }
        e->tail = c;
    }
    e->tail->data[e->tail->fill++] = v;
    e->count++;
    return true;
}

#endif

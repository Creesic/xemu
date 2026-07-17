/*
 * xemu frame inspector: method-origin log
 *
 * Owns the method-origin recording log, tracking GPU method submissions
 * with origin confidence levels, batching information, and physical addresses.
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
#ifndef XEMU_FRAMEINSPECT_METHODLOG_H
#define XEMU_FRAMEINSPECT_METHODLOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_METHOD_INVALID     0xFFFFFFFFu
#define FI_METHOD_RAW_WORD 0xFFFFu  /* record is a raw pushbuffer dword consumed by lookahead, not a decoded method; param holds the raw word */
#define FI_METHODLOG_CAP       (1u << 22)
#define FI_METHODLOG_BATCH_CAP (1u << 18)

enum {
    FI_ORIG_ATTRIBUTED = 0,
    FI_ORIG_PARTIAL,
    FI_ORIG_UNATTRIBUTED,
    FI_ORIG_LOSTSYNC,
};

typedef struct FIMethodRec {
    uint32_t method;
    uint16_t subchannel;
    uint16_t confidence;
    uint32_t param;
    uint32_t phys_addr;
    uint32_t writer_node;
} FIMethodRec;

typedef struct FIMethodBatch {
    uint32_t batch_event;
    uint32_t first_rec;
    uint32_t rec_count;
} FIMethodBatch;

typedef struct FIMethodLog {
    FIMethodRec *recs;
    uint32_t num_recs, cap_recs;
    FIMethodBatch *batches;
    uint32_t num_batches, cap_batches;
    bool truncated;
} FIMethodLog;

static inline bool fi_methodlog_init(FIMethodLog *m)
{
    memset(m, 0, sizeof(*m));
    m->cap_recs = 65536;
    m->cap_batches = 1024;
    m->recs = (FIMethodRec *)malloc(m->cap_recs * sizeof(FIMethodRec));
    m->batches = (FIMethodBatch *)malloc(m->cap_batches * sizeof(FIMethodBatch));
    if (!m->recs || !m->batches) {
        free(m->recs); free(m->batches);
        memset(m, 0, sizeof(*m));
        return false;
    }
    return true;
}

static inline void fi_methodlog_free(FIMethodLog *m)
{
    free(m->recs); free(m->batches);
    memset(m, 0, sizeof(*m));
    m->recs = NULL;
}

static inline uint32_t fi_methodlog_append(FIMethodLog *m, uint32_t method,
                                           uint16_t subchannel, uint32_t param,
                                           uint32_t phys_addr,
                                           uint32_t writer_node,
                                           uint16_t confidence)
{
    if (m->num_recs >= FI_METHODLOG_CAP) { m->truncated = true; return FI_METHOD_INVALID; }
    if (m->num_recs >= m->cap_recs) {
        uint32_t nc = m->cap_recs * 2;
        if (nc > FI_METHODLOG_CAP) nc = FI_METHODLOG_CAP;
        FIMethodRec *nr = (FIMethodRec *)realloc(m->recs, nc * sizeof(FIMethodRec));
        if (!nr) { m->truncated = true; return FI_METHOD_INVALID; }
        m->recs = nr; m->cap_recs = nc;
    }
    uint32_t idx = m->num_recs++;
    FIMethodRec *r = &m->recs[idx];
    r->method = method; r->subchannel = subchannel; r->param = param;
    r->phys_addr = phys_addr; r->writer_node = writer_node;
    r->confidence = confidence;
    return idx;
}

static inline void fi_methodlog_mark_batch(FIMethodLog *m, uint32_t batch_event,
                                           uint32_t first_rec, uint32_t rec_count)
{
    if (m->num_batches >= FI_METHODLOG_BATCH_CAP) { m->truncated = true; return; }
    if (m->num_batches >= m->cap_batches) {
        uint32_t nc = m->cap_batches * 2;
        if (nc > FI_METHODLOG_BATCH_CAP) nc = FI_METHODLOG_BATCH_CAP;
        FIMethodBatch *nb = (FIMethodBatch *)realloc(m->batches, nc * sizeof(FIMethodBatch));
        if (!nb) { m->truncated = true; return; }
        m->batches = nb; m->cap_batches = nc;
    }
    FIMethodBatch *b = &m->batches[m->num_batches++];
    b->batch_event = batch_event; b->first_rec = first_rec; b->rec_count = rec_count;
}

#endif /* XEMU_FRAMEINSPECT_METHODLOG_H */

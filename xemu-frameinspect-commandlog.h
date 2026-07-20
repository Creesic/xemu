/*
 * xemu frame inspector: typed PFIFO pushbuffer command log
 *
 * Retains raw pushbuffer words together with their decoded packet/control-flow
 * meaning and guest writer provenance. Header-only and QEMU-independent for
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
#ifndef XEMU_FRAMEINSPECT_COMMANDLOG_H
#define XEMU_FRAMEINSPECT_COMMANDLOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xemu-frameinspect-eventlog.h"
#include "xemu-frameinspect-methodlog.h"

#define FI_COMMAND_INVALID 0xFFFFFFFFu
#define FI_COMMANDLOG_CAP (1u << 22)
#define FI_COMMANDLOG_INITIAL_CAP 4096u

typedef enum FICommandKind {
    FI_CMD_METHOD_HEADER,
    FI_CMD_METHOD_PARAM,
    FI_CMD_JUMP,
    FI_CMD_OLD_JUMP,
    FI_CMD_CALL,
    FI_CMD_RETURN,
    FI_CMD_RESERVED,
} FICommandKind;

typedef enum FICommandMethodType {
    FI_CMD_METHOD_INCREASING,
    FI_CMD_METHOD_NON_INCREASING,
} FICommandMethodType;

typedef struct FICommandRec {
    uint64_t phys_addr;
    uint32_t seq;
    uint32_t dma_instance;
    uint32_t dma_get;
    uint32_t raw;
    uint32_t writer_node;
    uint16_t confidence;
    uint8_t kind;
    uint8_t reserved;
    union {
        struct {
            uint32_t method;
            uint16_t count;
            uint8_t subchannel;
            uint8_t method_type;
        } header;
        struct {
            uint32_t packet;
            uint32_t method;
            uint16_t parameter_index;
            uint8_t subchannel;
            uint8_t method_type;
        } parameter;
        struct {
            uint32_t target;
        } control;
    } data;
} FICommandRec;

typedef struct FICommandLog {
    FICommandRec *recs;
    uint32_t num_recs;
    uint32_t cap_recs;
    bool truncated;
} FICommandLog;

static inline bool fi_commandlog_init(FICommandLog *l, FIBudget *budget)
{
    memset(l, 0, sizeof(*l));
    uint64_t bytes =
        (uint64_t)FI_COMMANDLOG_INITIAL_CAP * sizeof(FICommandRec);
    if (!fi_budget_try(budget, bytes)) {
        return false;
    }
    l->recs = (FICommandRec *)malloc((size_t)bytes);
    if (!l->recs) {
        fi_budget_release(budget, bytes);
        return false;
    }
    l->cap_recs = FI_COMMANDLOG_INITIAL_CAP;
    return true;
}

static inline void fi_commandlog_free(FICommandLog *l, FIBudget *budget)
{
    if (l->recs) {
        fi_budget_release(
            budget, (uint64_t)l->cap_recs * sizeof(FICommandRec));
    }
    free(l->recs);
    memset(l, 0, sizeof(*l));
}

static inline uint32_t fi_commandlog_append(FICommandLog *l,
                                            FIBudget *budget,
                                            const FICommandRec *source)
{
    if (!l || !l->recs || !source) {
        if (l) {
            l->truncated = true;
        }
        return FI_COMMAND_INVALID;
    }
    if (l->num_recs >= FI_COMMANDLOG_CAP) {
        l->truncated = true;
        return FI_COMMAND_INVALID;
    }
    if (l->num_recs >= l->cap_recs) {
        uint32_t new_cap = l->cap_recs * 2;
        if (new_cap > FI_COMMANDLOG_CAP) {
            new_cap = FI_COMMANDLOG_CAP;
        }
        uint64_t old_bytes = (uint64_t)l->cap_recs * sizeof(FICommandRec);
        uint64_t new_bytes = (uint64_t)new_cap * sizeof(FICommandRec);
        if (!fi_budget_try(budget, new_bytes - old_bytes)) {
            l->truncated = true;
            return FI_COMMAND_INVALID;
        }
        FICommandRec *new_recs =
            (FICommandRec *)realloc(l->recs, (size_t)new_bytes);
        if (!new_recs) {
            fi_budget_release(budget, new_bytes - old_bytes);
            l->truncated = true;
            return FI_COMMAND_INVALID;
        }
        l->recs = new_recs;
        l->cap_recs = new_cap;
    }

    uint32_t id = l->num_recs++;
    l->recs[id] = *source;
    l->recs[id].seq = id;
    return id;
}

#endif

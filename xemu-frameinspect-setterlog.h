/*
 * xemu frame inspector: bounded latest-state setter journal
 *
 * Header-only and QEMU-independent for standalone unit testing.
 *
 * Copyright (C) 2026 xemu contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XEMU_FRAMEINSPECT_SETTERLOG_H
#define XEMU_FRAMEINSPECT_SETTERLOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xemu-frameinspect-drawlog.h"

#define FI_SETTER_TOKEN_INVALID 0ull
#define FI_SETTER_SOURCE_INVALID 0xFFFFFFFFu
#define FI_SETTER_COMMAND_INVALID 0xFFFFFFFFu

#define FI_SETTER_SOURCE_CAP (1u << 20)
#define FI_SETTER_DESTINATION_CAP (1u << 18)
#define FI_SETTER_SOURCE_INITIAL_CAP 1024u
#define FI_SETTER_DESTINATION_INITIAL_CAP 1024u

typedef enum FISetterDestinationKind {
    FI_SETTER_DEST_PGRAPH_REGISTER,
    FI_SETTER_DEST_VSH_PROGRAM,
    FI_SETTER_DEST_VSH_CONSTANT,
    FI_SETTER_DEST_VERTEX_ATTRIBUTE,
    FI_SETTER_DEST_TEXTURE,
    FI_SETTER_DEST_COMBINER,
    FI_SETTER_DEST_RASTER,
    FI_SETTER_DEST_TARGET,
} FISetterDestinationKind;

typedef FIStateDestination FISetterDestination;

typedef struct FISetterSource {
    uint64_t token;
    uint64_t phys_addr;
    uint32_t command_id;
    uint32_t method;
    uint32_t parameter;
    uint32_t writer_node;
    uint16_t subchannel;
    uint16_t confidence;
} FISetterSource;

typedef struct FISetterDestinationEntry {
    FISetterDestination destination;
    uint32_t source_ref; /* source index + 1; zero marks an empty hash slot */
} FISetterDestinationEntry;

typedef struct FISetterLog {
    FISetterSource *sources;
    FISetterDestinationEntry *destinations;
    uint32_t num_sources;
    uint32_t cap_sources;
    uint32_t num_destinations;
    uint32_t cap_destinations;
    uint64_t next_token;
    bool truncated;
} FISetterLog;

typedef enum FISetterBindingStatus {
    FI_SETTER_BINDING_AVAILABLE,
    FI_SETTER_BINDING_INHERITED,
} FISetterBindingStatus;

typedef struct FISetterSnapshotBinding {
    FISetterDestination destination;
    uint32_t source_index;
    uint8_t status;
    uint8_t reserved[3];
} FISetterSnapshotBinding;

typedef struct FISetterSnapshot {
    FISetterSource *sources;
    FISetterSnapshotBinding *bindings;
    uint32_t num_sources;
    uint32_t cap_sources;
    uint32_t num_bindings;
    uint64_t bytes;
    bool source_truncated;
    bool truncated;
} FISetterSnapshot;

static inline uint64_t fi_setterlog_initial_bytes(void)
{
    return (uint64_t)FI_SETTER_SOURCE_INITIAL_CAP * sizeof(FISetterSource) +
           (uint64_t)FI_SETTER_DESTINATION_INITIAL_CAP *
               sizeof(FISetterDestinationEntry);
}

static inline bool fi_setter_destination_equal(
    const FISetterDestination *a, const FISetterDestination *b)
{
    return a->kind == b->kind && a->index == b->index &&
           a->component == b->component && a->reserved == b->reserved;
}

static inline uint32_t fi_setter_destination_hash(
    const FISetterDestination *destination)
{
    uint32_t hash = 2166136261u;
    const uint16_t values[] = {
        destination->kind,
        destination->index,
        destination->component,
        destination->reserved,
    };
    for (uint32_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        hash ^= values[i] & 0xff;
        hash *= 16777619u;
        hash ^= values[i] >> 8;
        hash *= 16777619u;
    }
    return hash;
}

static inline uint32_t fi_setter_destination_slot(
    const FISetterDestinationEntry *entries, uint32_t cap,
    const FISetterDestination *destination, bool *found)
{
    uint32_t slot = fi_setter_destination_hash(destination) & (cap - 1);
    for (;;) {
        const FISetterDestinationEntry *entry = &entries[slot];
        if (entry->source_ref == 0) {
            *found = false;
            return slot;
        }
        if (fi_setter_destination_equal(&entry->destination, destination)) {
            *found = true;
            return slot;
        }
        slot = (slot + 1) & (cap - 1);
    }
}

static inline bool fi_setterlog_grow_sources(FISetterLog *log,
                                             FIBudget *budget,
                                             uint32_t needed)
{
    if (needed <= log->cap_sources) {
        return true;
    }
    if (needed > FI_SETTER_SOURCE_CAP) {
        return false;
    }
    uint32_t new_cap = log->cap_sources ? log->cap_sources :
                                          FI_SETTER_SOURCE_INITIAL_CAP;
    while (new_cap < needed) {
        uint32_t next = new_cap > FI_SETTER_SOURCE_CAP / 2 ?
                            FI_SETTER_SOURCE_CAP : new_cap * 2;
        if (next <= new_cap) {
            return false;
        }
        new_cap = next;
    }
    uint64_t old_bytes = (uint64_t)log->cap_sources * sizeof(FISetterSource);
    uint64_t new_bytes = (uint64_t)new_cap * sizeof(FISetterSource);
    if (new_bytes > SIZE_MAX ||
        !fi_budget_try(budget, new_bytes - old_bytes)) {
        return false;
    }
    FISetterSource *sources = (FISetterSource *)realloc(
        log->sources, (size_t)new_bytes);
    if (!sources) {
        fi_budget_release(budget, new_bytes - old_bytes);
        return false;
    }
    log->sources = sources;
    log->cap_sources = new_cap;
    return true;
}

static inline bool fi_setterlog_grow_destinations(FISetterLog *log,
                                                  FIBudget *budget,
                                                  uint32_t new_cap)
{
    if (new_cap <= log->cap_destinations) {
        return true;
    }
    if (new_cap > FI_SETTER_DESTINATION_CAP ||
        (new_cap & (new_cap - 1)) != 0) {
        return false;
    }
    uint64_t old_bytes = (uint64_t)log->cap_destinations *
                         sizeof(FISetterDestinationEntry);
    uint64_t new_bytes = (uint64_t)new_cap *
                         sizeof(FISetterDestinationEntry);
    if (new_bytes > SIZE_MAX ||
        !fi_budget_try(budget, new_bytes - old_bytes)) {
        return false;
    }
    FISetterDestinationEntry *entries =
        (FISetterDestinationEntry *)calloc(
            new_cap, sizeof(FISetterDestinationEntry));
    if (!entries) {
        fi_budget_release(budget, new_bytes - old_bytes);
        return false;
    }
    for (uint32_t i = 0; i < log->cap_destinations; i++) {
        FISetterDestinationEntry *old = &log->destinations[i];
        if (old->source_ref == 0) {
            continue;
        }
        bool found;
        uint32_t slot = fi_setter_destination_slot(
            entries, new_cap, &old->destination, &found);
        entries[slot] = *old;
    }
    free(log->destinations);
    log->destinations = entries;
    log->cap_destinations = new_cap;
    return true;
}

static inline void fi_setterlog_free(FISetterLog *log, FIBudget *budget)
{
    if (!log) {
        return;
    }
    fi_budget_release(
        budget, (uint64_t)log->cap_sources * sizeof(FISetterSource));
    fi_budget_release(
        budget, (uint64_t)log->cap_destinations *
                    sizeof(FISetterDestinationEntry));
    free(log->sources);
    free(log->destinations);
    memset(log, 0, sizeof(*log));
    log->next_token = 1;
}

static inline bool fi_setterlog_init(FISetterLog *log, FIBudget *budget)
{
    if (!log) {
        return false;
    }
    memset(log, 0, sizeof(*log));
    log->next_token = 1;
    if (!fi_setterlog_grow_sources(log, budget, 1) ||
        !fi_setterlog_grow_destinations(
            log, budget, FI_SETTER_DESTINATION_INITIAL_CAP)) {
        fi_setterlog_free(log, budget);
        return false;
    }
    return true;
}

static inline uint32_t fi_setterlog_source_index(const FISetterLog *log,
                                                 uint64_t token)
{
    if (!log || token == FI_SETTER_TOKEN_INVALID ||
        token > log->num_sources) {
        return FI_SETTER_SOURCE_INVALID;
    }
    uint32_t index = (uint32_t)(token - 1);
    return log->sources[index].token == token ? index :
                                               FI_SETTER_SOURCE_INVALID;
}

static inline uint64_t fi_setterlog_begin_source(
    FISetterLog *log, FIBudget *budget, uint32_t method, uint16_t subchannel,
    uint32_t parameter, uint64_t phys_addr, uint32_t writer_node,
    uint16_t confidence)
{
    if (!log || log->next_token == FI_SETTER_TOKEN_INVALID ||
        log->next_token == UINT64_MAX ||
        !fi_setterlog_grow_sources(log, budget, log->num_sources + 1)) {
        if (log) {
            log->truncated = true;
        }
        return FI_SETTER_TOKEN_INVALID;
    }
    uint64_t token = log->next_token++;
    FISetterSource *source = &log->sources[log->num_sources++];
    *source = (FISetterSource) {
        .token = token,
        .phys_addr = phys_addr,
        .command_id = FI_SETTER_COMMAND_INVALID,
        .method = method,
        .parameter = parameter,
        .writer_node = writer_node,
        .subchannel = subchannel,
        .confidence = confidence,
    };
    return token;
}

static inline bool fi_setterlog_bind_command(FISetterLog *log,
                                             uint64_t token,
                                             uint32_t command_id)
{
    uint32_t index = fi_setterlog_source_index(log, token);
    if (index == FI_SETTER_SOURCE_INVALID) {
        return false;
    }
    log->sources[index].command_id = command_id;
    return true;
}

static inline bool fi_setterlog_record_destination(
    FISetterLog *log, FIBudget *budget, uint64_t token,
    const FISetterDestination *destination)
{
    uint32_t source_index = fi_setterlog_source_index(log, token);
    if (source_index == FI_SETTER_SOURCE_INVALID || !destination) {
        return false;
    }
    bool found;
    uint32_t slot = fi_setter_destination_slot(
        log->destinations, log->cap_destinations, destination, &found);
    if (!found && (log->num_destinations + 1) * 2 >
                      log->cap_destinations) {
        uint32_t new_cap = log->cap_destinations >=
                                   FI_SETTER_DESTINATION_CAP / 2 ?
                               FI_SETTER_DESTINATION_CAP :
                               log->cap_destinations * 2;
        if (new_cap <= log->cap_destinations ||
            !fi_setterlog_grow_destinations(log, budget, new_cap)) {
            log->truncated = true;
            return false;
        }
        slot = fi_setter_destination_slot(
            log->destinations, log->cap_destinations, destination, &found);
    }
    FISetterDestinationEntry *entry = &log->destinations[slot];
    if (!found) {
        entry->destination = *destination;
        log->num_destinations++;
    }
    entry->source_ref = source_index + 1;
    return true;
}

static inline uint32_t fi_setterlog_lookup_source_id(
    const FISetterLog *log, const FISetterDestination *destination)
{
    if (!log || !destination || !log->cap_destinations) {
        return FI_SETTER_SOURCE_INVALID;
    }
    bool found;
    uint32_t slot = fi_setter_destination_slot(
        log->destinations, log->cap_destinations, destination, &found);
    return found ? log->destinations[slot].source_ref - 1 :
                   FI_SETTER_SOURCE_INVALID;
}

static inline const FISetterSource *fi_setterlog_lookup(
    const FISetterLog *log, const FISetterDestination *destination)
{
    uint32_t source_id = fi_setterlog_lookup_source_id(log, destination);
    return source_id == FI_SETTER_SOURCE_INVALID ? NULL :
                                                  &log->sources[source_id];
}

static inline void fi_setter_snapshot_free(FISetterSnapshot *snapshot,
                                           FIBudget *budget)
{
    if (!snapshot) {
        return;
    }
    fi_budget_release(budget, snapshot->bytes);
    free(snapshot->sources);
    free(snapshot->bindings);
    memset(snapshot, 0, sizeof(*snapshot));
}

static inline bool fi_setterlog_snapshot(
    const FISetterLog *log, const FISetterDestination *destinations,
    uint32_t count, FISetterSnapshot *snapshot, FIBudget *budget)
{
    if (!log || !snapshot || (!destinations && count) || snapshot->sources ||
        snapshot->bindings) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (!count) {
        snapshot->source_truncated = log->truncated;
        return true;
    }
    uint64_t source_bytes = (uint64_t)count * sizeof(FISetterSource);
    uint64_t binding_bytes =
        (uint64_t)count * sizeof(FISetterSnapshotBinding);
    if (source_bytes > UINT64_MAX - binding_bytes ||
        source_bytes > SIZE_MAX || binding_bytes > SIZE_MAX ||
        !fi_budget_try(budget, source_bytes + binding_bytes)) {
        snapshot->truncated = true;
        return false;
    }
    snapshot->sources = (FISetterSource *)malloc((size_t)source_bytes);
    snapshot->bindings = (FISetterSnapshotBinding *)malloc(
        (size_t)binding_bytes);
    if (!snapshot->sources || !snapshot->bindings) {
        free(snapshot->sources);
        free(snapshot->bindings);
        snapshot->sources = NULL;
        snapshot->bindings = NULL;
        fi_budget_release(budget, source_bytes + binding_bytes);
        snapshot->truncated = true;
        return false;
    }
    snapshot->bytes = source_bytes + binding_bytes;
    snapshot->cap_sources = count;
    snapshot->num_bindings = count;
    snapshot->source_truncated = log->truncated;

    for (uint32_t i = 0; i < count; i++) {
        FISetterSnapshotBinding *binding = &snapshot->bindings[i];
        memset(binding, 0, sizeof(*binding));
        binding->destination = destinations[i];
        uint32_t source_id = fi_setterlog_lookup_source_id(
            log, &destinations[i]);
        if (source_id == FI_SETTER_SOURCE_INVALID) {
            binding->source_index = FI_SETTER_SOURCE_INVALID;
            binding->status = FI_SETTER_BINDING_INHERITED;
            continue;
        }
        const FISetterSource *source = &log->sources[source_id];
        uint32_t snapshot_source = FI_SETTER_SOURCE_INVALID;
        for (uint32_t j = 0; j < snapshot->num_sources; j++) {
            if (snapshot->sources[j].token == source->token) {
                snapshot_source = j;
                break;
            }
        }
        if (snapshot_source == FI_SETTER_SOURCE_INVALID) {
            snapshot_source = snapshot->num_sources++;
            snapshot->sources[snapshot_source] = *source;
        }
        binding->source_index = snapshot_source;
        binding->status = FI_SETTER_BINDING_AVAILABLE;
    }
    return true;
}

#endif

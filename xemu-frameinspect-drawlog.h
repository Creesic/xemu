/*
 * xemu frame inspector: immutable draw submission log
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
#ifndef XEMU_FRAMEINSPECT_DRAWLOG_H
#define XEMU_FRAMEINSPECT_DRAWLOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xemu-frameinspect-eventlog.h"
#include "xemu-frameinspect-methodlog.h"

#define FI_DRAW_INVALID 0xFFFFFFFFu

#define FI_DRAW_ATTRIBUTE_COUNT 16u
#define FI_DRAW_TEXTURE_COUNT 4u
#define FI_DRAW_SAMPLE_LIMIT 8u
#define FI_DRAW_RAW_ATTRIBUTE_MAX 16u
#define FI_DRAW_DOMINANT_WRITER_LIMIT 16u
#define FI_DRAW_WRITER_CANDIDATE_LIMIT 256u

#define FI_DRAWLOG_SUBMISSION_CAP (1u << 16)
#define FI_DRAWLOG_SEGMENT_CAP (1u << 20)
#define FI_DRAWLOG_INDEX_CAP (1u << 24)
#define FI_DRAWLOG_SAMPLE_CAP (1u << 19)
#define FI_DRAWLOG_SOURCE_CAP (1u << 22)
#define FI_DRAWLOG_WRITER_SET_CAP (1u << 18)
#define FI_DRAWLOG_WRITER_CAP (1u << 20)

#define FI_DRAWLOG_SUBMISSION_INITIAL_CAP 64u
#define FI_DRAWLOG_SEGMENT_INITIAL_CAP 256u
#define FI_DRAWLOG_INDEX_INITIAL_CAP 4096u
#define FI_DRAWLOG_SAMPLE_INITIAL_CAP 64u
#define FI_DRAWLOG_SOURCE_INITIAL_CAP 1024u
#define FI_DRAWLOG_WRITER_SET_INITIAL_CAP 64u
#define FI_DRAWLOG_WRITER_INITIAL_CAP 256u

typedef enum FIDrawRoute {
    FI_DRAW_ROUTE_ARRAYS,
    FI_DRAW_ROUTE_INDEXED,
    FI_DRAW_ROUTE_INLINE_BUFFER,
    FI_DRAW_ROUTE_INLINE_ARRAY,
} FIDrawRoute;

typedef enum FIValueStatus {
    FI_VALUE_UNAVAILABLE,
    FI_VALUE_PRESENT,
    FI_VALUE_CONSTANT,
} FIValueStatus;

typedef enum FITextureLayout {
    FI_TEXTURE_LAYOUT_SWIZZLED,
    FI_TEXTURE_LAYOUT_LINEAR,
    FI_TEXTURE_LAYOUT_CUBEMAP,
} FITextureLayout;

enum {
    FI_TEXTURE_FLAG_RT_BACKED = 1u << 0,
    FI_TEXTURE_FLAG_RANGE_INVALID = 1u << 1,
    FI_TEXTURE_FLAG_RESOURCE_UNAVAILABLE = 1u << 2,
    FI_TEXTURE_FLAG_WRITERS_TRUNCATED = 1u << 3,
    FI_TEXTURE_FLAG_PALETTE_RANGE_INVALID = 1u << 4,
    FI_TEXTURE_FLAG_PALETTE_RESOURCE_UNAVAILABLE = 1u << 5,
    FI_TEXTURE_FLAG_PALETTE_WRITERS_TRUNCATED = 1u << 6,
    FI_TEXTURE_FLAG_CUBEMAP = 1u << 7,
    FI_TEXTURE_FLAG_BORDER = 1u << 8,
    FI_TEXTURE_FLAG_SIGNED_A = 1u << 9,
    FI_TEXTURE_FLAG_SIGNED_R = 1u << 10,
    FI_TEXTURE_FLAG_SIGNED_G = 1u << 11,
    FI_TEXTURE_FLAG_SIGNED_B = 1u << 12,
    FI_TEXTURE_FLAG_PRODUCER_EVENT_UNAVAILABLE = 1u << 13,
    FI_TEXTURE_FLAG_PRODUCER_SURFACE_UNAVAILABLE = 1u << 14,
};

enum {
    FI_DRAW_COMPLETE = 1u << 0,
    FI_DRAW_SAMPLE_TRUNCATED = 1u << 1,
    FI_DRAW_INDEX_TRUNCATED = 1u << 2,
    FI_DRAW_SOURCE_TRUNCATED = 1u << 3,
};

typedef struct FIDrawSegment {
    uint32_t first;
    uint32_t count;
} FIDrawSegment;

typedef struct FIAttributeDesc {
    uint32_t dma_object;
    uint32_t guest_offset;
    uint64_t phys_addr;
    uint16_t format;
    uint16_t stride;
    uint8_t slot;
    uint8_t status;
    uint8_t components;
    uint8_t element_size;
    uint8_t dma_select;
    uint8_t reserved[3];
    uint32_t source_first;
    uint16_t source_count;
    uint16_t reserved2;
} FIAttributeDesc;

typedef struct FIVertexAttributeSample {
    uint8_t raw[FI_DRAW_RAW_ATTRIBUTE_MAX];
    float decoded[4];
    uint32_t source_first;
    uint16_t source_count;
    uint8_t raw_len;
    uint8_t status;
} FIVertexAttributeSample;

typedef struct FIVertexSample {
    uint32_t ordinal;
    uint32_t source_index;
    FIVertexAttributeSample attrs[FI_DRAW_ATTRIBUTE_COUNT];
} FIVertexSample;

typedef struct FIStateDestination {
    uint16_t kind;
    uint16_t index;
    uint16_t component;
    uint16_t reserved;
} FIStateDestination;

typedef struct FIStateSource {
    FIStateDestination destination;
    uint64_t source_token;
    uint64_t phys_addr;
    uint32_t command_id;
    uint32_t method;
    uint32_t parameter;
    uint32_t writer_node;
    uint16_t subchannel;
    uint16_t confidence;
    uint8_t inherited;
    uint8_t reserved[3];
} FIStateSource;

typedef struct FIResourceWriter {
    uint32_t writer_node;
    uint16_t confidence;
    uint16_t reserved;
    uint64_t bytes;
} FIResourceWriter;

typedef struct FIWriterSpan {
    uint32_t writer_node;
    uint16_t confidence;
    uint16_t reserved;
    uint64_t bytes;
} FIWriterSpan;

typedef struct FIWriterCoverage {
    uint64_t total_bytes;
    uint64_t attributed_bytes;
    uint64_t partial_bytes;
    uint64_t unattributed_bytes;
    uint64_t omitted_bytes;
    uint64_t truncated_bytes;
    uint32_t dominant_count;
} FIWriterCoverage;

typedef struct FIResourceWriterSet {
    uint32_t first_writer;
    uint32_t num_writers;
    FIWriterCoverage coverage;
} FIResourceWriterSet;

typedef struct FITextureStage {
    uint64_t guest_addr;
    uint64_t palette_addr;
    uint64_t content_hash;
    uint64_t palette_hash;
    uint32_t dma_object;
    uint32_t palette_dma_object;
    uint32_t guest_offset;
    uint32_t palette_offset;
    uint32_t resource_id;
    uint32_t palette_id;
    uint32_t producer_surface_gen;
    uint32_t producer_event;
    uint32_t writer_set;
    uint32_t palette_writer_set;
    uint32_t content_length;
    uint32_t palette_length;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t pitch;
    uint32_t address_raw;
    uint32_t filter_raw;
    uint32_t control0_raw;
    uint32_t control1_raw;
    uint32_t palette_raw;
    uint16_t format;
    uint16_t flags;
    uint8_t stage;
    uint8_t status;
    uint8_t dimensionality;
    uint8_t mip_count;
    uint8_t layout;
    uint8_t dma_select;
    uint8_t palette_dma_select;
    uint8_t address_u;
    uint8_t address_v;
    uint8_t address_p;
    uint8_t min_filter;
    uint8_t mag_filter;
    uint8_t min_mip_level;
    uint8_t max_mip_level;
} FITextureStage;

typedef struct FIColorSummary {
    uint32_t packed_first;
    float min[4];
    float max[4];
    uint8_t available;
    uint8_t reserved[3];
} FIColorSummary;

typedef struct FIDrawSubmission {
    uint64_t geometry_hash;
    uint32_t batch_event;
    uint32_t submit_index;
    uint32_t topology;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t first_segment;
    uint32_t num_segments;
    uint32_t first_index;
    uint32_t num_indices;
    uint32_t first_sample;
    uint32_t num_samples;
    uint32_t first_source;
    uint32_t num_sources;
    uint32_t first_writer_set;
    uint32_t num_writer_sets;
    uint32_t first_writer;
    uint32_t color_surface_gen;
    uint32_t zeta_surface_gen;
    uint32_t regs_resource;
    uint32_t program_resource;
    uint32_t constants_resource;
    uint16_t route;
    uint16_t flags;
    FIColorSummary color0;
    FIColorSummary color1;
    FIAttributeDesc attrs[FI_DRAW_ATTRIBUTE_COUNT];
    FITextureStage textures[FI_DRAW_TEXTURE_COUNT];
} FIDrawSubmission;

typedef struct FIDrawLog {
    FIDrawSubmission *submissions;
    FIDrawSegment *segments;
    uint32_t *indices;
    FIVertexSample *samples;
    FIStateSource *sources;
    FIResourceWriterSet *writer_sets;
    FIResourceWriter *writers;
    uint32_t num_submissions, cap_submissions;
    uint32_t num_segments, cap_segments;
    uint32_t num_indices, cap_indices;
    uint32_t num_samples, cap_samples;
    uint32_t num_sources, cap_sources;
    uint32_t num_writer_sets, cap_writer_sets;
    uint32_t num_writers, cap_writers;
    uint32_t open_submission;
    bool truncated;
} FIDrawLog;

static inline uint64_t fi_drawlog_initial_bytes(void)
{
    return
        (uint64_t)FI_DRAWLOG_SUBMISSION_INITIAL_CAP *
            sizeof(FIDrawSubmission) +
        (uint64_t)FI_DRAWLOG_SEGMENT_INITIAL_CAP * sizeof(FIDrawSegment) +
        (uint64_t)FI_DRAWLOG_INDEX_INITIAL_CAP * sizeof(uint32_t) +
        (uint64_t)FI_DRAWLOG_SAMPLE_INITIAL_CAP * sizeof(FIVertexSample) +
        (uint64_t)FI_DRAWLOG_SOURCE_INITIAL_CAP * sizeof(FIStateSource) +
        (uint64_t)FI_DRAWLOG_WRITER_SET_INITIAL_CAP *
            sizeof(FIResourceWriterSet) +
        (uint64_t)FI_DRAWLOG_WRITER_INITIAL_CAP * sizeof(FIResourceWriter);
}

static inline bool fi_drawlog_grow(void **data, uint32_t *cap,
                                   uint32_t needed, uint32_t initial_cap,
                                   uint32_t max_cap, size_t item_size,
                                   FIBudget *budget)
{
    if (needed <= *cap) {
        return true;
    }
    if (needed > max_cap || item_size == 0) {
        return false;
    }

    uint32_t new_cap = *cap ? *cap : initial_cap;
    while (new_cap < needed) {
        uint32_t next = new_cap > max_cap / 2 ? max_cap : new_cap * 2;
        if (next <= new_cap) {
            return false;
        }
        new_cap = next;
    }

    uint64_t old_bytes = (uint64_t)*cap * item_size;
    uint64_t new_bytes = (uint64_t)new_cap * item_size;
    if (new_bytes > SIZE_MAX || !fi_budget_try(budget, new_bytes - old_bytes)) {
        return false;
    }
    void *new_data = realloc(*data, (size_t)new_bytes);
    if (!new_data) {
        fi_budget_release(budget, new_bytes - old_bytes);
        return false;
    }
    *data = new_data;
    *cap = new_cap;
    return true;
}

static inline void fi_drawlog_free(FIDrawLog *l, FIBudget *budget)
{
    if (!l) {
        return;
    }
    fi_budget_release(budget,
        (uint64_t)l->cap_submissions * sizeof(FIDrawSubmission));
    fi_budget_release(budget,
        (uint64_t)l->cap_segments * sizeof(FIDrawSegment));
    fi_budget_release(budget, (uint64_t)l->cap_indices * sizeof(uint32_t));
    fi_budget_release(budget,
        (uint64_t)l->cap_samples * sizeof(FIVertexSample));
    fi_budget_release(budget,
        (uint64_t)l->cap_sources * sizeof(FIStateSource));
    fi_budget_release(budget,
        (uint64_t)l->cap_writer_sets * sizeof(FIResourceWriterSet));
    fi_budget_release(budget,
        (uint64_t)l->cap_writers * sizeof(FIResourceWriter));
    free(l->submissions);
    free(l->segments);
    free(l->indices);
    free(l->samples);
    free(l->sources);
    free(l->writer_sets);
    free(l->writers);
    memset(l, 0, sizeof(*l));
    l->open_submission = FI_DRAW_INVALID;
}

static inline bool fi_drawlog_init(FIDrawLog *l, FIBudget *budget)
{
    if (!l) {
        return false;
    }
    memset(l, 0, sizeof(*l));
    l->open_submission = FI_DRAW_INVALID;
    if (!fi_drawlog_grow((void **)&l->submissions, &l->cap_submissions, 1,
                         FI_DRAWLOG_SUBMISSION_INITIAL_CAP,
                         FI_DRAWLOG_SUBMISSION_CAP,
                         sizeof(FIDrawSubmission), budget) ||
        !fi_drawlog_grow((void **)&l->segments, &l->cap_segments, 1,
                         FI_DRAWLOG_SEGMENT_INITIAL_CAP,
                         FI_DRAWLOG_SEGMENT_CAP, sizeof(FIDrawSegment),
                         budget) ||
        !fi_drawlog_grow((void **)&l->indices, &l->cap_indices, 1,
                         FI_DRAWLOG_INDEX_INITIAL_CAP, FI_DRAWLOG_INDEX_CAP,
                         sizeof(uint32_t), budget) ||
        !fi_drawlog_grow((void **)&l->samples, &l->cap_samples, 1,
                         FI_DRAWLOG_SAMPLE_INITIAL_CAP, FI_DRAWLOG_SAMPLE_CAP,
                         sizeof(FIVertexSample), budget) ||
        !fi_drawlog_grow((void **)&l->sources, &l->cap_sources, 1,
                         FI_DRAWLOG_SOURCE_INITIAL_CAP, FI_DRAWLOG_SOURCE_CAP,
                         sizeof(FIStateSource), budget) ||
        !fi_drawlog_grow((void **)&l->writer_sets, &l->cap_writer_sets, 1,
                         FI_DRAWLOG_WRITER_SET_INITIAL_CAP,
                         FI_DRAWLOG_WRITER_SET_CAP,
                         sizeof(FIResourceWriterSet), budget) ||
        !fi_drawlog_grow((void **)&l->writers, &l->cap_writers, 1,
                         FI_DRAWLOG_WRITER_INITIAL_CAP,
                         FI_DRAWLOG_WRITER_CAP, sizeof(FIResourceWriter),
                         budget)) {
        fi_drawlog_free(l, budget);
        return false;
    }
    return true;
}

static inline bool fi_drawlog_open_matches(const FIDrawLog *l,
                                            uint32_t draw_id)
{
    return l && draw_id != FI_DRAW_INVALID &&
           l->open_submission == draw_id && draw_id < l->num_submissions;
}

static inline uint32_t fi_drawlog_begin(FIDrawLog *l, FIBudget *budget,
                                        uint32_t batch_event,
                                        uint32_t submit_index,
                                        uint16_t route, uint32_t topology)
{
    if (!l || l->open_submission != FI_DRAW_INVALID ||
        route > FI_DRAW_ROUTE_INLINE_ARRAY) {
        return FI_DRAW_INVALID;
    }
    if (!fi_drawlog_grow((void **)&l->submissions, &l->cap_submissions,
                         l->num_submissions + 1,
                         FI_DRAWLOG_SUBMISSION_INITIAL_CAP,
                         FI_DRAWLOG_SUBMISSION_CAP,
                         sizeof(FIDrawSubmission), budget)) {
        l->truncated = true;
        return FI_DRAW_INVALID;
    }

    uint32_t id = l->num_submissions++;
    FIDrawSubmission *d = &l->submissions[id];
    memset(d, 0, sizeof(*d));
    d->batch_event = batch_event;
    d->submit_index = submit_index;
    d->route = route;
    d->topology = topology;
    d->first_segment = l->num_segments;
    d->first_index = l->num_indices;
    d->first_sample = l->num_samples;
    d->first_source = l->num_sources;
    d->first_writer_set = l->num_writer_sets;
    d->first_writer = l->num_writers;
    d->color_surface_gen = FI_DRAW_INVALID;
    d->zeta_surface_gen = FI_DRAW_INVALID;
    d->regs_resource = FI_DRAW_INVALID;
    d->program_resource = FI_DRAW_INVALID;
    d->constants_resource = FI_DRAW_INVALID;
    for (uint32_t i = 0; i < FI_DRAW_ATTRIBUTE_COUNT; i++) {
        d->attrs[i].slot = i;
        d->attrs[i].status = FI_VALUE_UNAVAILABLE;
        d->attrs[i].source_first = FI_DRAW_INVALID;
    }
    for (uint32_t i = 0; i < FI_DRAW_TEXTURE_COUNT; i++) {
        d->textures[i].stage = i;
        d->textures[i].status = FI_VALUE_UNAVAILABLE;
        d->textures[i].resource_id = FI_DRAW_INVALID;
        d->textures[i].palette_id = FI_DRAW_INVALID;
        d->textures[i].producer_surface_gen = FI_DRAW_INVALID;
        d->textures[i].producer_event = FI_DRAW_INVALID;
        d->textures[i].writer_set = FI_DRAW_INVALID;
        d->textures[i].palette_writer_set = FI_DRAW_INVALID;
    }
    l->open_submission = id;
    return id;
}

static inline bool fi_drawlog_append_segments(FIDrawLog *l, FIBudget *budget,
                                              uint32_t draw_id,
                                              const FIDrawSegment *segments,
                                              uint32_t count)
{
    if (!fi_drawlog_open_matches(l, draw_id) || (!segments && count)) {
        return false;
    }
    FIDrawSubmission *d = &l->submissions[draw_id];
    if (d->num_segments || d->first_segment != l->num_segments) {
        return false;
    }
    if (count > FI_DRAWLOG_SEGMENT_CAP - l->num_segments ||
        !fi_drawlog_grow((void **)&l->segments, &l->cap_segments,
                         l->num_segments + count,
                         FI_DRAWLOG_SEGMENT_INITIAL_CAP,
                         FI_DRAWLOG_SEGMENT_CAP, sizeof(FIDrawSegment),
                         budget)) {
        l->truncated = true;
        return false;
    }
    if (count) {
        memcpy(&l->segments[l->num_segments], segments,
               (size_t)count * sizeof(*segments));
    }
    l->num_segments += count;
    d->num_segments = count;
    return true;
}

static inline bool fi_drawlog_append_indices(FIDrawLog *l, FIBudget *budget,
                                             uint32_t draw_id,
                                             const uint32_t *indices,
                                             uint32_t count)
{
    if (!fi_drawlog_open_matches(l, draw_id) || (!indices && count)) {
        return false;
    }
    FIDrawSubmission *d = &l->submissions[draw_id];
    if (d->num_indices || d->first_index != l->num_indices) {
        return false;
    }
    if (count > FI_DRAWLOG_INDEX_CAP - l->num_indices ||
        !fi_drawlog_grow((void **)&l->indices, &l->cap_indices,
                         l->num_indices + count,
                         FI_DRAWLOG_INDEX_INITIAL_CAP, FI_DRAWLOG_INDEX_CAP,
                         sizeof(uint32_t), budget)) {
        d->flags |= FI_DRAW_INDEX_TRUNCATED;
        l->truncated = true;
        return false;
    }
    if (count) {
        memcpy(&l->indices[l->num_indices], indices,
               (size_t)count * sizeof(*indices));
    }
    l->num_indices += count;
    d->num_indices = count;
    return true;
}

static inline uint32_t fi_drawlog_append_sample(FIDrawLog *l,
                                                FIBudget *budget,
                                                uint32_t draw_id,
                                                const FIVertexSample *sample)
{
    if (!fi_drawlog_open_matches(l, draw_id) || !sample) {
        return FI_DRAW_INVALID;
    }
    FIDrawSubmission *d = &l->submissions[draw_id];
    if (d->num_samples >= FI_DRAW_SAMPLE_LIMIT) {
        d->flags |= FI_DRAW_SAMPLE_TRUNCATED;
        return FI_DRAW_INVALID;
    }
    if (d->first_sample + d->num_samples != l->num_samples) {
        return FI_DRAW_INVALID;
    }
    if (!fi_drawlog_grow((void **)&l->samples, &l->cap_samples,
                         l->num_samples + 1, FI_DRAWLOG_SAMPLE_INITIAL_CAP,
                         FI_DRAWLOG_SAMPLE_CAP, sizeof(FIVertexSample),
                         budget)) {
        d->flags |= FI_DRAW_SAMPLE_TRUNCATED;
        l->truncated = true;
        return FI_DRAW_INVALID;
    }
    uint32_t id = l->num_samples++;
    l->samples[id] = *sample;
    d->num_samples++;
    return id;
}

static inline uint32_t fi_drawlog_append_source(FIDrawLog *l,
                                                FIBudget *budget,
                                                uint32_t draw_id,
                                                const FIStateSource *source)
{
    if (!fi_drawlog_open_matches(l, draw_id) || !source) {
        return FI_DRAW_INVALID;
    }
    FIDrawSubmission *d = &l->submissions[draw_id];
    if (d->first_source + d->num_sources != l->num_sources) {
        return FI_DRAW_INVALID;
    }
    if (!fi_drawlog_grow((void **)&l->sources, &l->cap_sources,
                         l->num_sources + 1, FI_DRAWLOG_SOURCE_INITIAL_CAP,
                         FI_DRAWLOG_SOURCE_CAP, sizeof(FIStateSource),
                         budget)) {
        d->flags |= FI_DRAW_SOURCE_TRUNCATED;
        l->truncated = true;
        return FI_DRAW_INVALID;
    }
    uint32_t id = l->num_sources++;
    l->sources[id] = *source;
    d->num_sources++;
    return id;
}

static inline void fi_writer_coverage_add(FIWriterCoverage *coverage,
                                          uint16_t confidence, uint64_t bytes)
{
    coverage->total_bytes += bytes;
    if (confidence == FI_ORIG_ATTRIBUTED) {
        coverage->attributed_bytes += bytes;
    } else if (confidence == FI_ORIG_PARTIAL) {
        coverage->partial_bytes += bytes;
    } else {
        coverage->unattributed_bytes += bytes;
    }
}

static inline bool fi_writer_precedes(const FIResourceWriter *a,
                                      const FIResourceWriter *b)
{
    if (a->bytes != b->bytes) {
        return a->bytes > b->bytes;
    }
    if (a->writer_node != b->writer_node) {
        return a->writer_node < b->writer_node;
    }
    return a->confidence < b->confidence;
}

static inline FIWriterCoverage fi_writer_aggregate(
    const FIWriterSpan *spans, uint32_t count,
    FIResourceWriter dominant[FI_DRAW_DOMINANT_WRITER_LIMIT])
{
    FIWriterCoverage coverage = { 0 };
    FIResourceWriter candidates[FI_DRAW_WRITER_CANDIDATE_LIMIT];
    uint32_t num_candidates = 0;
    memset(dominant, 0,
           sizeof(*dominant) * FI_DRAW_DOMINANT_WRITER_LIMIT);

    for (uint32_t i = 0; i < count; i++) {
        const FIWriterSpan *span = &spans[i];
        fi_writer_coverage_add(&coverage, span->confidence, span->bytes);
        uint32_t j;
        for (j = 0; j < num_candidates; j++) {
            if (candidates[j].writer_node == span->writer_node &&
                candidates[j].confidence == span->confidence) {
                candidates[j].bytes += span->bytes;
                break;
            }
        }
        if (j < num_candidates) {
            continue;
        }
        if (num_candidates >= FI_DRAW_WRITER_CANDIDATE_LIMIT) {
            coverage.truncated_bytes += span->bytes;
            continue;
        }
        candidates[num_candidates++] = (FIResourceWriter) {
            .writer_node = span->writer_node,
            .confidence = span->confidence,
            .bytes = span->bytes,
        };
    }

    for (uint32_t i = 1; i < num_candidates; i++) {
        FIResourceWriter value = candidates[i];
        uint32_t j = i;
        while (j > 0 && fi_writer_precedes(&value, &candidates[j - 1])) {
            candidates[j] = candidates[j - 1];
            j--;
        }
        candidates[j] = value;
    }

    coverage.dominant_count =
        num_candidates < FI_DRAW_DOMINANT_WRITER_LIMIT ?
        num_candidates : FI_DRAW_DOMINANT_WRITER_LIMIT;
    for (uint32_t i = 0; i < num_candidates; i++) {
        if (i < coverage.dominant_count) {
            dominant[i] = candidates[i];
        } else {
            coverage.omitted_bytes += candidates[i].bytes;
        }
    }
    return coverage;
}

static inline uint32_t fi_drawlog_append_writer_set(
    FIDrawLog *l, FIBudget *budget, uint32_t draw_id,
    const FIWriterSpan *spans, uint32_t count)
{
    if (!fi_drawlog_open_matches(l, draw_id) || (!spans && count)) {
        return FI_DRAW_INVALID;
    }
    FIDrawSubmission *d = &l->submissions[draw_id];
    if (d->first_writer_set + d->num_writer_sets != l->num_writer_sets) {
        return FI_DRAW_INVALID;
    }
    FIResourceWriter dominant[FI_DRAW_DOMINANT_WRITER_LIMIT];
    FIWriterCoverage coverage = fi_writer_aggregate(spans, count, dominant);
    uint32_t writer_count = coverage.dominant_count;
    if (l->num_writer_sets >= FI_DRAWLOG_WRITER_SET_CAP ||
        writer_count > FI_DRAWLOG_WRITER_CAP - l->num_writers ||
        !fi_drawlog_grow((void **)&l->writers, &l->cap_writers,
                         l->num_writers + writer_count,
                         FI_DRAWLOG_WRITER_INITIAL_CAP,
                         FI_DRAWLOG_WRITER_CAP, sizeof(FIResourceWriter),
                         budget) ||
        !fi_drawlog_grow((void **)&l->writer_sets, &l->cap_writer_sets,
                         l->num_writer_sets + 1,
                         FI_DRAWLOG_WRITER_SET_INITIAL_CAP,
                         FI_DRAWLOG_WRITER_SET_CAP,
                         sizeof(FIResourceWriterSet), budget)) {
        l->truncated = true;
        return FI_DRAW_INVALID;
    }

    uint32_t set_id = l->num_writer_sets++;
    FIResourceWriterSet *set = &l->writer_sets[set_id];
    set->first_writer = l->num_writers;
    set->num_writers = writer_count;
    set->coverage = coverage;
    if (writer_count) {
        memcpy(&l->writers[l->num_writers], dominant,
               (size_t)writer_count * sizeof(*dominant));
    }
    l->num_writers += writer_count;
    d->num_writer_sets++;
    return set_id;
}

static inline bool fi_drawlog_complete(FIDrawLog *l, uint32_t draw_id)
{
    if (!fi_drawlog_open_matches(l, draw_id)) {
        return false;
    }
    l->submissions[draw_id].flags |= FI_DRAW_COMPLETE;
    l->open_submission = FI_DRAW_INVALID;
    return true;
}

static inline bool fi_drawlog_abort(FIDrawLog *l, uint32_t draw_id)
{
    if (!fi_drawlog_open_matches(l, draw_id) ||
        draw_id + 1 != l->num_submissions) {
        return false;
    }
    const FIDrawSubmission *d = &l->submissions[draw_id];
    l->num_segments = d->first_segment;
    l->num_indices = d->first_index;
    l->num_samples = d->first_sample;
    l->num_sources = d->first_source;
    l->num_writer_sets = d->first_writer_set;
    l->num_writers = d->first_writer;
    l->num_submissions--;
    memset(&l->submissions[draw_id], 0, sizeof(l->submissions[draw_id]));
    l->open_submission = FI_DRAW_INVALID;
    return true;
}

#endif

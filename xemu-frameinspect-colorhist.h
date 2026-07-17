/*
 * xemu frame inspector: colour-history codec
 *
 * Colour-history codec: keyframes + RLE deltas for pixel event tracking.
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

#ifndef XEMU_FRAMEINSPECT_COLORHIST_H
#define XEMU_FRAMEINSPECT_COLORHIST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FI_CH_MAX_EVENTS    4096u
#define FI_CH_MAX_KEYFRAMES 64u
#define FI_CH_RUN_CAP       (1u << 24)   /* max run entries total */
#define FI_CH_COLOR_CAP     (1u << 26)   /* max colour entries (before+after) */
#define FI_CH_BYTE_BUDGET   (256ull << 20)

typedef struct FIColorTouch {
    uint32_t event_id;
    uint32_t before;
    uint32_t after;
} FIColorTouch;

typedef struct FIColorRun {
    uint32_t start;      /* first pixel index in the run */
    uint32_t len;        /* contiguous changed pixels */
    uint32_t color_off;  /* offset into colours[]; before at off, after at off+len */
} FIColorRun;

typedef struct FIColorEvent {
    uint32_t event_id;
    uint32_t run_first;  /* index into runs[] */
    uint32_t run_count;
} FIColorEvent;

typedef struct FIKeyframe {
    uint32_t next_event; /* image state before this event index */
    uint32_t *image;     /* w*h snapshot */
} FIKeyframe;

typedef struct FIColorHist {
    uint32_t width, height, npix;
    uint32_t keyframe_interval;
    uint32_t *current;          /* running image */
    FIColorEvent *events;       /* [FI_CH_MAX_EVENTS] */
    uint32_t num_events;
    FIColorRun *runs;           /* growable up to FI_CH_RUN_CAP */
    uint32_t num_runs, cap_runs;
    uint32_t *colors;           /* growable up to FI_CH_COLOR_CAP */
    uint32_t num_colors, cap_colors;
    FIKeyframe keyframes[FI_CH_MAX_KEYFRAMES];
    uint32_t num_keyframes;
    uint64_t bytes_used;
    uint64_t byte_budget;
    bool has_baseline;
    bool keyframes_truncated;
    bool truncated;
} FIColorHist;

static inline bool fi_colorhist_init(FIColorHist *ch, uint32_t width,
                                     uint32_t height, uint32_t keyframe_interval)
{
    if (!ch) {
        return false;
    }
    memset(ch, 0, sizeof(*ch));
    if (!width || !height || width > UINT32_MAX / height) {
        return false;
    }
    ch->width = width;
    ch->height = height;
    ch->npix = width * height;
    ch->keyframe_interval = keyframe_interval ? keyframe_interval : 16;
    ch->byte_budget = FI_CH_BYTE_BUDGET;
    uint64_t current_bytes = (uint64_t)ch->npix * sizeof(uint32_t);
    uint64_t event_bytes = (uint64_t)FI_CH_MAX_EVENTS * sizeof(FIColorEvent);
    uint64_t run_bytes = 4096ull * sizeof(FIColorRun);
    uint64_t color_bytes = 4096ull * sizeof(uint32_t);
    uint64_t initial_bytes = current_bytes + event_bytes + run_bytes +
                             color_bytes;
    if (current_bytes > SIZE_MAX || event_bytes > SIZE_MAX ||
        run_bytes > SIZE_MAX || color_bytes > SIZE_MAX ||
        initial_bytes > ch->byte_budget) {
        memset(ch, 0, sizeof(*ch));
        return false;
    }
    ch->current = (uint32_t *)calloc(ch->npix, sizeof(uint32_t));
    ch->events = (FIColorEvent *)calloc(FI_CH_MAX_EVENTS, sizeof(FIColorEvent));
    ch->cap_runs = 4096; ch->cap_colors = 4096;
    ch->runs = (FIColorRun *)malloc(ch->cap_runs * sizeof(FIColorRun));
    ch->colors = (uint32_t *)malloc(ch->cap_colors * sizeof(uint32_t));
    if (!ch->current || !ch->events || !ch->runs || !ch->colors) {
        free(ch->current); free(ch->events); free(ch->runs); free(ch->colors);
        memset(ch, 0, sizeof(*ch));
        return false;
    }
    ch->bytes_used = initial_bytes;
    return true;
}

static inline void fi_colorhist_free(FIColorHist *ch)
{
    for (uint32_t i = 0; i < ch->num_keyframes; i++) {
        free(ch->keyframes[i].image);
    }
    free(ch->current); free(ch->events); free(ch->runs); free(ch->colors);
    memset(ch, 0, sizeof(*ch));
    ch->events = NULL;
}

static inline bool fi_ch_budget_available(const FIColorHist *ch,
                                          uint64_t bytes)
{
    return ch->bytes_used <= ch->byte_budget &&
           bytes <= ch->byte_budget - ch->bytes_used;
}

static inline bool fi_ch_snapshot_keyframe(FIColorHist *ch,
                                           uint32_t next_event)
{
    if (ch->num_keyframes >= FI_CH_MAX_KEYFRAMES) {
        ch->keyframes_truncated = true;
        return false; /* reconstruction still works without this optimization */
    }
    uint64_t image_bytes = (uint64_t)ch->npix * sizeof(uint32_t);
    if (!fi_ch_budget_available(ch, image_bytes)) {
        ch->keyframes_truncated = true;
        return false;
    }
    uint32_t *image = (uint32_t *)malloc((size_t)image_bytes);
    if (!image) {
        ch->keyframes_truncated = true;
        return false;
    }
    memcpy(image, ch->current, (size_t)image_bytes);
    FIKeyframe *kf = &ch->keyframes[ch->num_keyframes++];
    kf->next_event = next_event;
    kf->image = image;
    ch->bytes_used += image_bytes;
    return true;
}

static inline bool fi_colorhist_set_baseline(FIColorHist *ch,
                                             const uint32_t *rgba)
{
    if (!ch || !rgba || !ch->current || ch->has_baseline || ch->num_events) {
        return false;
    }
    uint64_t image_bytes = (uint64_t)ch->npix * sizeof(uint32_t);
    if (ch->num_keyframes >= FI_CH_MAX_KEYFRAMES ||
        !fi_ch_budget_available(ch, image_bytes)) {
        ch->truncated = true;
        return false;
    }
    uint32_t *image = (uint32_t *)malloc((size_t)image_bytes);
    if (!image) {
        ch->truncated = true;
        return false;
    }
    memcpy(image, rgba, (size_t)image_bytes);
    memcpy(ch->current, rgba, (size_t)image_bytes);
    FIKeyframe *kf = &ch->keyframes[ch->num_keyframes++];
    kf->next_event = 0;
    kf->image = image;
    ch->bytes_used += image_bytes;
    ch->has_baseline = true;
    return true;
}

static inline bool fi_ch_grow_runs(FIColorHist *ch, uint32_t need)
{
    if (need > FI_CH_RUN_CAP - ch->num_runs) {
        ch->truncated = true;
        return false;
    }
    uint32_t required = ch->num_runs + need;
    if (required <= ch->cap_runs) {
        return true;
    }
    uint32_t nc = ch->cap_runs;
    while (nc < required) {
        nc = nc > FI_CH_RUN_CAP / 2 ? FI_CH_RUN_CAP : nc * 2;
    }
    uint64_t old_bytes = (uint64_t)ch->cap_runs * sizeof(FIColorRun);
    uint64_t new_bytes = (uint64_t)nc * sizeof(FIColorRun);
    if (!fi_ch_budget_available(ch, new_bytes - old_bytes)) {
        ch->truncated = true;
        return false;
    }
    FIColorRun *nr = (FIColorRun *)realloc(ch->runs, (size_t)new_bytes);
    if (!nr) { ch->truncated = true; return false; }
    ch->runs = nr;
    ch->cap_runs = nc;
    ch->bytes_used += new_bytes - old_bytes;
    return true;
}

static inline bool fi_ch_grow_colors(FIColorHist *ch, uint32_t need)
{
    if (need > FI_CH_COLOR_CAP - ch->num_colors) {
        ch->truncated = true;
        return false;
    }
    uint32_t required = ch->num_colors + need;
    if (required <= ch->cap_colors) {
        return true;
    }
    uint32_t nc = ch->cap_colors;
    while (nc < required) {
        nc = nc > FI_CH_COLOR_CAP / 2 ? FI_CH_COLOR_CAP : nc * 2;
    }
    uint64_t old_bytes = (uint64_t)ch->cap_colors * sizeof(uint32_t);
    uint64_t new_bytes = (uint64_t)nc * sizeof(uint32_t);
    if (!fi_ch_budget_available(ch, new_bytes - old_bytes)) {
        ch->truncated = true;
        return false;
    }
    uint32_t *nn = (uint32_t *)realloc(ch->colors, (size_t)new_bytes);
    if (!nn) { ch->truncated = true; return false; }
    ch->colors = nn;
    ch->cap_colors = nc;
    ch->bytes_used += new_bytes - old_bytes;
    return true;
}

static inline bool fi_colorhist_add_event(FIColorHist *ch, uint32_t event_id,
                                          const uint32_t *post)
{
    if (!ch || !post || !ch->has_baseline || ch->truncated) {
        return false;
    }
    if (ch->num_events >= FI_CH_MAX_EVENTS) {
        ch->truncated = true;
        return false;
    }
    uint32_t start_runs = ch->num_runs;
    uint32_t start_colors = ch->num_colors;
    FIColorEvent *ev = &ch->events[ch->num_events];
    ev->event_id = event_id;
    ev->run_first = ch->num_runs;
    ev->run_count = 0;
    uint32_t i = 0;
    while (i < ch->npix) {
        if (ch->current[i] == post[i]) { i++; continue; }
        uint32_t start = i;
        while (i < ch->npix && ch->current[i] != post[i]) i++;
        uint32_t len = i - start;
        if (len > FI_CH_COLOR_CAP / 2 || !fi_ch_grow_runs(ch, 1) ||
            !fi_ch_grow_colors(ch, len * 2)) {
            ch->num_runs = start_runs;
            ch->num_colors = start_colors;
            ev->run_count = 0;
            return false;
        }
        FIColorRun *r = &ch->runs[ch->num_runs++];
        r->start = start; r->len = len; r->color_off = ch->num_colors;
        for (uint32_t k = 0; k < len; k++) {
            ch->colors[ch->num_colors + k] = ch->current[start + k];      /* before */
            ch->colors[ch->num_colors + len + k] = post[start + k];       /* after  */
        }
        ch->num_colors += 2 * len;
        ev->run_count++;
    }
    memcpy(ch->current, post, ch->npix * sizeof(uint32_t));
    ch->num_events++;
    if ((ch->num_events % ch->keyframe_interval) == 0) {
        fi_ch_snapshot_keyframe(ch, ch->num_events);
    }
    return true;
}

static inline uint32_t fi_colorhist_num_events(const FIColorHist *ch)
{
    return ch->num_events;
}

static inline bool fi_colorhist_reconstruct(const FIColorHist *ch,
                                            uint32_t event_index, uint32_t *out)
{
    if (!ch || !out || !ch->has_baseline || event_index >= ch->num_events) {
        return false;
    }
    /* nearest image state whose next event is no later than the target */
    const FIKeyframe *best = NULL;
    for (uint32_t i = 0; i < ch->num_keyframes; i++) {
        uint32_t next = ch->keyframes[i].next_event;
        if (next <= event_index + 1 && (!best || next >= best->next_event)) {
            best = &ch->keyframes[i];
        }
    }
    if (!best) {
        return false;
    }
    memcpy(out, best->image, ch->npix * sizeof(uint32_t));
    uint32_t start_ev = best->next_event;
    for (uint32_t e = start_ev; e <= event_index && e < ch->num_events; e++) {
        const FIColorEvent *ev = &ch->events[e];
        for (uint32_t r = 0; r < ev->run_count; r++) {
            const FIColorRun *run = &ch->runs[ev->run_first + r];
            const uint32_t *after = &ch->colors[run->color_off + run->len];
            for (uint32_t k = 0; k < run->len; k++) {
                out[run->start + k] = after[k];
            }
        }
    }
    return true;
}

static inline int fi_colorhist_pixel_history(const FIColorHist *ch,
                                             uint32_t pixel_index,
                                             FIColorTouch *out, int max)
{
    if (!ch || !out || max <= 0 || pixel_index >= ch->npix) {
        return 0;
    }
    int n = 0;
    for (uint32_t e = 0; e < ch->num_events && n < max; e++) {
        const FIColorEvent *ev = &ch->events[e];
        for (uint32_t r = 0; r < ev->run_count; r++) {
            const FIColorRun *run = &ch->runs[ev->run_first + r];
            if (pixel_index >= run->start && pixel_index < run->start + run->len) {
                uint32_t off = pixel_index - run->start;
                out[n].event_id = ev->event_id;
                out[n].before = ch->colors[run->color_off + off];
                out[n].after = ch->colors[run->color_off + run->len + off];
                n++;
                break;
            }
        }
    }
    return n;
}

#endif /* XEMU_FRAMEINSPECT_COLORHIST_H */

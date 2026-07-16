/*
 * xemu-frameinspect-colorhist.h
 *
 * Colour-history codec: keyframes + RLE deltas for pixel event tracking.
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
    uint32_t after_event; /* image state after this event index (or 0 = baseline) */
    uint32_t *image;      /* w*h snapshot */
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
    bool has_baseline;
    bool truncated;
} FIColorHist;

static inline bool fi_colorhist_init(FIColorHist *ch, uint32_t width,
                                     uint32_t height, uint32_t keyframe_interval)
{
    memset(ch, 0, sizeof(*ch));
    ch->width = width; ch->height = height; ch->npix = width * height;
    ch->keyframe_interval = keyframe_interval ? keyframe_interval : 16;
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

static inline void fi_ch_snapshot_keyframe(FIColorHist *ch, uint32_t after_event)
{
    if (ch->num_keyframes >= FI_CH_MAX_KEYFRAMES) {
        return; /* keyframes are an optimization; reconstruction still works */
    }
    FIKeyframe *kf = &ch->keyframes[ch->num_keyframes++];
    kf->after_event = after_event;
    kf->image = (uint32_t *)malloc(ch->npix * sizeof(uint32_t));
    if (kf->image) {
        memcpy(kf->image, ch->current, ch->npix * sizeof(uint32_t));
    } else {
        ch->num_keyframes--;
    }
}

static inline void fi_colorhist_set_baseline(FIColorHist *ch,
                                             const uint32_t *rgba)
{
    memcpy(ch->current, rgba, ch->npix * sizeof(uint32_t));
    ch->has_baseline = true;
    fi_ch_snapshot_keyframe(ch, 0); /* keyframe tagged after_event 0 = baseline */
}

static inline bool fi_ch_grow_runs(FIColorHist *ch, uint32_t need)
{
    if (ch->num_runs + need <= ch->cap_runs) return true;
    uint32_t nc = ch->cap_runs * 2;
    while (nc < ch->num_runs + need) nc *= 2;
    if (nc > FI_CH_RUN_CAP) { ch->truncated = true; return false; }
    FIColorRun *nr = (FIColorRun *)realloc(ch->runs, nc * sizeof(FIColorRun));
    if (!nr) { ch->truncated = true; return false; }
    ch->runs = nr; ch->cap_runs = nc; return true;
}

static inline bool fi_ch_grow_colors(FIColorHist *ch, uint32_t need)
{
    if (ch->num_colors + need <= ch->cap_colors) return true;
    uint32_t nc = ch->cap_colors * 2;
    while (nc < ch->num_colors + need) nc *= 2;
    if (nc > FI_CH_COLOR_CAP) { ch->truncated = true; return false; }
    uint32_t *nn = (uint32_t *)realloc(ch->colors, nc * sizeof(uint32_t));
    if (!nn) { ch->truncated = true; return false; }
    ch->colors = nn; ch->cap_colors = nc; return true;
}

static inline bool fi_colorhist_add_event(FIColorHist *ch, uint32_t event_id,
                                          const uint32_t *post)
{
    if (ch->num_events >= FI_CH_MAX_EVENTS) { ch->truncated = true; return false; }
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
        if (!fi_ch_grow_runs(ch, 1) || !fi_ch_grow_colors(ch, 2 * len)) {
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
        fi_ch_snapshot_keyframe(ch, ch->num_events - 1);
    }
    return true;
}

static inline uint32_t fi_colorhist_num_events(const FIColorHist *ch)
{
    return ch->num_events;
}

static inline void fi_colorhist_reconstruct(const FIColorHist *ch,
                                            uint32_t event_index, uint32_t *out)
{
    /* nearest keyframe with after_event <= event_index (baseline kf = 0) */
    const FIKeyframe *best = NULL;
    for (uint32_t i = 0; i < ch->num_keyframes; i++) {
        uint32_t ae = ch->keyframes[i].after_event;
        if (ae <= event_index && (!best || ae >= best->after_event)) {
            best = &ch->keyframes[i];
        }
    }
    uint32_t start_ev;
    if (best) {
        memcpy(out, best->image, ch->npix * sizeof(uint32_t));
        start_ev = best->after_event ? best->after_event + 1 : 0;
        /* baseline keyframe (after_event 0) already equals pre-event-0 state
         * only if no event 0 applied; distinguish via has_baseline: the
         * baseline kf image is the state BEFORE event 0, so replay from 0. */
        if (best->after_event == 0) start_ev = 0;
    } else {
        memset(out, 0, ch->npix * sizeof(uint32_t));
        start_ev = 0;
    }
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
}

static inline int fi_colorhist_pixel_history(const FIColorHist *ch,
                                             uint32_t pixel_index,
                                             FIColorTouch *out, int max)
{
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

#ifndef XGPU_PLUME_WAIT_STATS_H
#define XGPU_PLUME_WAIT_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lightweight reason-tagged accounting of every main-thread blocking site in
 * the plume layer (fence waits, swap-chain present). Opt-in via
 * XRECOMP_PLUME_WAIT_STATS; when disabled every call is a cached-int test and
 * records nothing, so unlike XRECOMP_PLUME_PERF it does not distort frame
 * time. Counters are single-producer: all instrumented sites run on the
 * cooperative guest thread today (async-present worker calls the same owner
 * bodies on its own thread only when that experiment is enabled, which is
 * incompatible with a stats run).
 */
typedef enum XgpuPlumeWaitSite {
    XGPU_PLUME_WAIT_RING_DRAIN = 0,   /* present preflight drains async slots */
    XGPU_PLUME_WAIT_SLOT_RECLAIM,     /* wait-batch reuses an in-flight slot */
    XGPU_PLUME_WAIT_DOWNLOAD_FENCE,   /* eager fence after a wait-batch submit */
    XGPU_PLUME_WAIT_PRESENT_SWAP,     /* swap-chain present call */
    XGPU_PLUME_WAIT_PRESENT_FENCE,    /* fence after the present submission */
    XGPU_PLUME_WAIT_TEX_UPLOAD,       /* synchronous recorded-texture upload */
    XGPU_PLUME_WAIT_SURFACE_DOWNLOAD, /* color surface GPU->CPU readback */
    XGPU_PLUME_WAIT_ZETA_DOWNLOAD,    /* depth surface GPU->CPU readback */
    XGPU_PLUME_WAIT_SURFACE_RESTORE,  /* surface re-upload/restore submit */
    XGPU_PLUME_WAIT_DESC_FLUSH,       /* descriptor-batch flush mid-replay */
    XGPU_PLUME_WAIT_SITE_COUNT
} XgpuPlumeWaitSite;

/* How each host present synchronized, for the report's fallback breakdown. */
typedef enum XgpuPlumeWaitPresentClass {
    XGPU_PLUME_PRESENT_CLASS_PIPELINED = 0, /* fence wait deferred */
    XGPU_PLUME_PRESENT_CLASS_SYNC_DOWNLOADS,/* surface downloads on the CL */
    XGPU_PLUME_PRESENT_CLASS_SYNC_CAPTURE,  /* F2 source capture on the CL */
    XGPU_PLUME_PRESENT_CLASS_SYNC_OFF,      /* pipeline disabled */
    XGPU_PLUME_WAIT_PRESENT_CLASS_COUNT
} XgpuPlumeWaitPresentClass;

int xgpu_plume_wait_stats_enabled(void);
/* Returns a monotonic-ns start stamp, or 0 when disabled. */
uint64_t xgpu_plume_wait_stats_begin(void);
/* No-op when start_ns is 0. */
void xgpu_plume_wait_stats_end(XgpuPlumeWaitSite site, uint64_t start_ns);
/* Count one host present so the report can normalize per present. */
void xgpu_plume_wait_stats_present(void);
/* Classify how the present just counted synchronized. */
void xgpu_plume_wait_stats_present_class(XgpuPlumeWaitPresentClass cls);
/* Test access: presents counted for one class. */
uint64_t xgpu_plume_wait_stats_present_class_count(
    XgpuPlumeWaitPresentClass cls);
/* One-shot summary to stderr; safe to call when disabled (prints nothing). */
void xgpu_plume_wait_stats_report(void);

/* Test access: totals for one site. Returns count; out params may be NULL. */
uint64_t xgpu_plume_wait_stats_site(XgpuPlumeWaitSite site,
                                    uint64_t *total_ns, uint64_t *max_ns);

#ifdef __cplusplus
}
#endif

#endif

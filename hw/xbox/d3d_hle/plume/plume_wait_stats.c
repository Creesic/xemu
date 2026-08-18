#include "plume_wait_stats.h"

#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_wait_stats_enabled = -1;
static uint64_t g_site_count[XGPU_PLUME_WAIT_SITE_COUNT];
static uint64_t g_site_ns[XGPU_PLUME_WAIT_SITE_COUNT];
static uint64_t g_site_max_ns[XGPU_PLUME_WAIT_SITE_COUNT];
static uint64_t g_presents;
static uint64_t g_present_class[XGPU_PLUME_WAIT_PRESENT_CLASS_COUNT];
static uint64_t g_first_ns;
static uint64_t g_last_ns;
static int g_reported;

static const char *present_class_name(XgpuPlumeWaitPresentClass cls)
{
    switch (cls) {
    case XGPU_PLUME_PRESENT_CLASS_PIPELINED:      return "pipelined";
    case XGPU_PLUME_PRESENT_CLASS_SYNC_DOWNLOADS: return "sync-downloads";
    case XGPU_PLUME_PRESENT_CLASS_SYNC_CAPTURE:   return "sync-capture";
    case XGPU_PLUME_PRESENT_CLASS_SYNC_OFF:       return "sync-off";
    default:                                      return "?";
    }
}

static const char *wait_site_name(XgpuPlumeWaitSite site)
{
    switch (site) {
    case XGPU_PLUME_WAIT_RING_DRAIN:       return "ring-drain";
    case XGPU_PLUME_WAIT_SLOT_RECLAIM:     return "slot-reclaim";
    case XGPU_PLUME_WAIT_DOWNLOAD_FENCE:   return "download-fence";
    case XGPU_PLUME_WAIT_PRESENT_SWAP:     return "present-swap";
    case XGPU_PLUME_WAIT_PRESENT_FENCE:    return "present-fence";
    case XGPU_PLUME_WAIT_TEX_UPLOAD:       return "tex-upload";
    case XGPU_PLUME_WAIT_SURFACE_DOWNLOAD: return "surface-download";
    case XGPU_PLUME_WAIT_ZETA_DOWNLOAD:    return "zeta-download";
    case XGPU_PLUME_WAIT_SURFACE_RESTORE:  return "surface-restore";
    case XGPU_PLUME_WAIT_DESC_FLUSH:       return "desc-flush";
    default:                               return "?";
    }
}

int xgpu_plume_wait_stats_enabled(void)
{
    if (g_wait_stats_enabled < 0) {
        const char *value = getenv("XRECOMP_PLUME_WAIT_STATS");
        g_wait_stats_enabled =
            value && value[0] && strcmp(value, "0") != 0 ? 1 : 0;
        if (g_wait_stats_enabled)
            fprintf(stderr, "[PLUME-WAIT] stats enabled\n");
    }
    return g_wait_stats_enabled;
}

uint64_t xgpu_plume_wait_stats_begin(void)
{
    if (!xgpu_plume_wait_stats_enabled())
        return 0;
    return xrecomp_host_monotonic_ns();
}

void xgpu_plume_wait_stats_end(XgpuPlumeWaitSite site, uint64_t start_ns)
{
    uint64_t now, elapsed;
    if (!start_ns || site >= XGPU_PLUME_WAIT_SITE_COUNT)
        return;
    now = xrecomp_host_monotonic_ns();
    elapsed = now - start_ns;
    if (!g_first_ns)
        g_first_ns = start_ns;
    g_last_ns = now;
    g_site_count[site]++;
    g_site_ns[site] += elapsed;
    if (elapsed > g_site_max_ns[site])
        g_site_max_ns[site] = elapsed;
}

void xgpu_plume_wait_stats_present(void)
{
    if (!xgpu_plume_wait_stats_enabled())
        return;
    g_presents++;
}

void xgpu_plume_wait_stats_present_class(XgpuPlumeWaitPresentClass cls)
{
    if (cls >= XGPU_PLUME_WAIT_PRESENT_CLASS_COUNT ||
        !xgpu_plume_wait_stats_enabled())
        return;
    g_present_class[cls]++;
}

uint64_t xgpu_plume_wait_stats_present_class_count(
    XgpuPlumeWaitPresentClass cls)
{
    if (cls >= XGPU_PLUME_WAIT_PRESENT_CLASS_COUNT)
        return 0;
    return g_present_class[cls];
}

uint64_t xgpu_plume_wait_stats_site(XgpuPlumeWaitSite site,
                                    uint64_t *total_ns, uint64_t *max_ns)
{
    if (site >= XGPU_PLUME_WAIT_SITE_COUNT)
        return 0;
    if (total_ns)
        *total_ns = g_site_ns[site];
    if (max_ns)
        *max_ns = g_site_max_ns[site];
    return g_site_count[site];
}

void xgpu_plume_wait_stats_report(void)
{
    uint64_t total_ns = 0;
    uint64_t total_count = 0;
    double span_ms;
    int site;
    if (g_wait_stats_enabled != 1 || g_reported)
        return;
    g_reported = 1;
    for (site = 0; site < XGPU_PLUME_WAIT_SITE_COUNT; site++) {
        total_ns += g_site_ns[site];
        total_count += g_site_count[site];
    }
    span_ms = (double)(g_last_ns - g_first_ns) * 1.0e-6;
    fprintf(stderr,
            "[PLUME-WAIT] report: %llu presents, %llu waits, %.3f ms blocked "
            "total (%.3f ms/present) over %.1f ms observed\n",
            (unsigned long long)g_presents, (unsigned long long)total_count,
            (double)total_ns * 1.0e-6,
            g_presents ? (double)total_ns * 1.0e-6 / (double)g_presents : 0.0,
            span_ms);
    for (site = 0; site < XGPU_PLUME_WAIT_SITE_COUNT; site++) {
        if (!g_site_count[site])
            continue;
        fprintf(stderr,
                "[PLUME-WAIT]   %-16s count=%-8llu total=%9.3f ms  "
                "per-present=%7.4f ms  max=%7.3f ms\n",
                wait_site_name((XgpuPlumeWaitSite)site),
                (unsigned long long)g_site_count[site],
                (double)g_site_ns[site] * 1.0e-6,
                g_presents
                    ? (double)g_site_ns[site] * 1.0e-6 / (double)g_presents
                    : 0.0,
                (double)g_site_max_ns[site] * 1.0e-6);
    }
    for (site = 0; site < XGPU_PLUME_WAIT_PRESENT_CLASS_COUNT; site++) {
        if (!g_present_class[site])
            continue;
        fprintf(stderr,
                "[PLUME-WAIT]   present class %-14s %llu (%.1f%%)\n",
                present_class_name((XgpuPlumeWaitPresentClass)site),
                (unsigned long long)g_present_class[site],
                g_presents ? (double)g_present_class[site] * 100.0 /
                                 (double)g_presents
                           : 0.0);
    }
}

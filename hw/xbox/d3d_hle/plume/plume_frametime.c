#include "plume_frametime.h"

#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Status refresh period. Long enough that formatting cost is irrelevant, short
 * enough to watch a stall happen. */
#define FT_UPDATE_NS   500000000ull

/* Intervals kept per window for the percentile. At 60 Hz a 500 ms window holds
 * ~30; the cap only matters if presents run far above refresh rate, and then
 * the tail is uninteresting anyway. */
#define FT_MAX_SAMPLES 512

typedef struct {
    uint32_t us[FT_MAX_SAMPLES];
    uint32_t stored;      /* samples in us[] */
    uint32_t counted;     /* samples this window, including any past the cap */
    uint64_t last_ns;
} FtStream;

static FtStream g_present;
static FtStream g_flip;
static uint64_t g_window_start_ns;
static void (*g_sink)(const char *status);
static char g_status[256];
static int g_log_enabled = -1;
static FILE *g_log;

/* Guest work completed this window, differenced from cumulative counters the
 * caller supplies at each flip. */
static uint64_t g_window_methods;
static uint64_t g_window_draws;
static uint32_t g_last_methods_total;
static uint32_t g_last_draws_total;
static int g_counters_valid;

static void ft_sample(FtStream *s, uint64_t now_ns)
{
    if (s->last_ns && now_ns > s->last_ns) {
        const uint64_t delta_us = (now_ns - s->last_ns) / 1000ull;
        if (s->stored < FT_MAX_SAMPLES)
            s->us[s->stored++] = delta_us > 0xFFFFFFFFull
                ? 0xFFFFFFFFu : (uint32_t)delta_us;
        s->counted++;
    }
    s->last_ns = now_ns;
}

static int ft_cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a;
    const uint32_t y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Milliseconds: mean, 95th percentile, max. Sorts in place — the window's
 * samples are discarded straight after. */
static void ft_stats(FtStream *s, double *avg_ms, double *p95_ms,
                     double *max_ms)
{
    uint64_t sum = 0;
    uint32_t i;

    *avg_ms = *p95_ms = *max_ms = 0.0;
    if (!s->stored)
        return;
    qsort(s->us, s->stored, sizeof(s->us[0]), ft_cmp_u32);
    for (i = 0; i < s->stored; i++)
        sum += s->us[i];
    *avg_ms = (double)sum / (double)s->stored / 1000.0;
    *p95_ms = (double)s->us[(s->stored * 95u) / 100u >= s->stored
                            ? s->stored - 1u
                            : (s->stored * 95u) / 100u] / 1000.0;
    *max_ms = (double)s->us[s->stored - 1u] / 1000.0;
}

static int ft_log_enabled(void)
{
    if (g_log_enabled < 0) {
        const char *value = getenv("XRECOMP_PLUME_FRAMETIME_LOG");
        g_log_enabled = value && value[0] && strcmp(value, "0") != 0 ? 1 : 0;
    }
    return g_log_enabled;
}

static void ft_publish(uint64_t now_ns)
{
    const double window_s = (double)(now_ns - g_window_start_ns) / 1.0e9;
    double p_avg, p_p95, p_max, f_avg, f_p95, f_max;
    double present_fps, flip_fps;

    ft_stats(&g_present, &p_avg, &p_p95, &p_max);
    ft_stats(&g_flip, &f_avg, &f_p95, &f_max);
    present_fps = window_s > 0.0 ? (double)g_present.counted / window_s : 0.0;
    flip_fps = window_s > 0.0 ? (double)g_flip.counted / window_s : 0.0;

    {
        /* Per guest frame, and the implied cost of one dispatched method —
         * the ratio that says whether a slow frame is doing more work or the
         * same work more slowly. */
        const double mpf = g_flip.counted
            ? (double)g_window_methods / (double)g_flip.counted : 0.0;
        const double dpf = g_flip.counted
            ? (double)g_window_draws / (double)g_flip.counted : 0.0;
        const double us_per_method = mpf > 0.0 ? f_avg * 1000.0 / mpf : 0.0;

        snprintf(g_status, sizeof(g_status),
                 "present %.1f ms avg / %.1f p95 / %.1f max (%.1f fps) | "
                 "guest %.1f ms (%.1f fps) | %.0f methods + %.0f draws/frame "
                 "(%.2f us/method)",
                 p_avg, p_p95, p_max, present_fps, f_avg, flip_fps,
                 mpf, dpf, us_per_method);
    }

    if (ft_log_enabled()) {
        if (!g_log)
            g_log = fopen("plume_frametime.log", "a");
        if (g_log) {
            fprintf(g_log,
                    "%.3f,%u,%.3f,%.3f,%.3f,%.2f,%u,%.3f,%.3f,%.2f,%llu,%llu\n",
                    (double)now_ns / 1.0e9, g_present.counted, p_avg, p_p95,
                    p_max, present_fps, g_flip.counted, f_avg, f_max,
                    flip_fps, (unsigned long long)g_window_methods,
                    (unsigned long long)g_window_draws);
            fflush(g_log);
        }
    }

    g_present.stored = g_present.counted = 0;
    g_flip.stored = g_flip.counted = 0;
    g_window_methods = g_window_draws = 0;
    g_window_start_ns = now_ns;

    if (g_sink)
        g_sink(g_status);
}

void xgpu_plume_frametime_present(void)
{
    const uint64_t now_ns = xrecomp_host_monotonic_ns();

    ft_sample(&g_present, now_ns);
    if (!g_window_start_ns)
        g_window_start_ns = now_ns;
    else if (now_ns - g_window_start_ns >= FT_UPDATE_NS)
        ft_publish(now_ns);
}

void xgpu_plume_frametime_flip(uint32_t methods_total, uint32_t draws_total)
{
    if (g_counters_valid) {
        g_window_methods += methods_total - g_last_methods_total;
        g_window_draws += draws_total - g_last_draws_total;
    }
    g_last_methods_total = methods_total;
    g_last_draws_total = draws_total;
    g_counters_valid = 1;
    ft_sample(&g_flip, xrecomp_host_monotonic_ns());
}

void xgpu_plume_frametime_set_sink(void (*sink)(const char *status))
{
    g_sink = sink;
}

int xgpu_plume_frametime_status(char *out, unsigned long capacity)
{
    if (!out || !capacity)
        return 0;
    if (!g_status[0]) {
        out[0] = '\0';
        return 0;
    }
    snprintf(out, (size_t)capacity, "%s", g_status);
    return 1;
}

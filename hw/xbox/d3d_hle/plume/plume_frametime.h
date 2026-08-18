/*
 * plume_frametime.h — always-on frame pacing telemetry.
 *
 * Distinct from plume_perf.h: that is opt-in per-phase profiling
 * (XRECOMP_PLUME_PERF) reported to stderr, which is invisible when the nativeish
 * runs without a console. This is the permanent, cheap "how fast is it right
 * now" readout — two monotonic clock reads per frame and a formatted status
 * string at 2 Hz, surfaced through a host-provided sink (window title).
 *
 * Game-agnostic: no title, address, or resolution assumptions.
 */
#ifndef XGPU_PLUME_FRAMETIME_H
#define XGPU_PLUME_FRAMETIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One host present that actually reached the swap chain. Skipped/deferred
 * presents must not be counted — they are not frames the user saw. */
void xgpu_plume_frametime_present(void);

/* One guest flip: the title declared a frame complete. Several host presents
 * can serve one guest frame, so these two rates differ and both matter.
 *
 * The two totals are running cumulative counters (dispatched GPU methods and
 * draw calls); the readout differences them per window. Frame cost here tracks
 * method volume, so ms/frame is only interpretable next to methods/frame — and
 * counters are free, unlike the per-method timestamps that inflated an earlier
 * profile by 3x. */
void xgpu_plume_frametime_flip(uint32_t methods_total, uint32_t draws_total);

/* Host-owned status display. The renderer never touches the native window;
 * the host registers a sink and decides where the text goes. Passing NULL
 * unregisters. The sink is called from the presenting thread. */
void xgpu_plume_frametime_set_sink(void (*sink)(const char *status));

/* Current status line, e.g.
 *   "present 12.3 ms avg / 18.9 p95 / 41.0 max (81 fps) | guest 30.1 fps"
 * Returns 0 when no samples have accumulated yet. */
int xgpu_plume_frametime_status(char *out, unsigned long capacity);

#ifdef __cplusplus
}
#endif

#endif /* XGPU_PLUME_FRAMETIME_H */

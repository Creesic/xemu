/*
 * plume_f2_capture.h — F2-armed bounded draw-stream capture.
 *
 * The user presses F2 while a visual defect is on screen; recording starts
 * immediately and remains active through the next XRECOMP_PLUME_F2_FRAMES
 * (default 3) issued presents. This also captures guest activity during a
 * no-present gap. Replayed draw streams are appended to
 * plume_f2_capture.log with per-draw host
 * resource identity (target surface generations, PS/VS identity, per
 * stage texture guest address + content-version hash, render-state and
 * constant hashes, and every silently skipped draw). Always compiled
 * in; idle cost is one key poll per guest present.
 */
#ifndef XGPU_PLUME_F2_CAPTURE_H
#define XGPU_PLUME_F2_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Edge-triggered F2 poll (Windows GetAsyncKeyState; no-op elsewhere).
 * Called from present requests and the independent VBlank service. */
void xgpu_plume_f2_poll(void);
/* Start a capture manually (what the poll calls on a fresh F2 press). */
void xgpu_plume_f2_request(void);
/* Nonzero while a capture is recording. */
int xgpu_plume_f2_active(void);
/* Retained present hook; captures now begin immediately when requested. */
void xgpu_plume_f2_present_begin(void);
/* Log one present outcome; issued presents count toward the capture's
 * frame budget and the capture closes when the budget is spent. */
void xgpu_plume_f2_present(int issued, const char *reason,
                           uint32_t present_guest, uint32_t queued);
/* Append one "[F2] ..." line (newline added) when a capture is active. */
void xgpu_plume_f2_log(const char *fmt, ...);
/* Flush and close the capture log during orderly host/test shutdown. */
void xgpu_plume_f2_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* XGPU_PLUME_F2_CAPTURE_H */

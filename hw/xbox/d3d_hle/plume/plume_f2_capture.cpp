/*
 * plume_f2_capture.cpp — F2-armed bounded draw-stream capture.
 *
 * See plume_f2_capture.h. Record, present, and guest VBlank service run on
 * the same host thread (proven by the flicker-trace TID audit), so plain ints
 * suffice for the capture state.
 */
#include "plume_f2_capture.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static int g_remaining;
static unsigned g_capture_id;
static unsigned g_frame_in_capture;
static unsigned g_lines;
static FILE *g_log;

/* Per-capture line budget: a runaway draw stream truncates instead of
 * filling the disk (in-game frames are ~1-4k draws; 3 frames plus their
 * WAIT-ring replays fit comfortably). */
static const unsigned kLineCap = 400000u;

static int f2_frames(void)
{
    static int frames = -1;
    if (frames < 0) {
        const char *value = getenv("XRECOMP_PLUME_F2_FRAMES");
        frames = value && *value ? atoi(value) : 0;
        if (frames <= 0)
            frames = 3;
    }
    return frames;
}

static const char *f2_log_path(void)
{
    const char *path = getenv("XRECOMP_PLUME_F2_LOG");
    return path && *path ? path : "plume_f2_capture.log";
}

static int f2_begin_capture(void)
{
    if (g_remaining > 0)
        return 1;
    if (!g_log)
        g_log = fopen(f2_log_path(), "a");
    if (!g_log) {
        fprintf(stderr, "[PLUME-F2] cannot open %s\n", f2_log_path());
        return 0;
    }
    g_capture_id++;
    g_frame_in_capture = 0;
    g_lines = 0;
    g_remaining = f2_frames();
    fprintf(g_log, "[F2] ==== capture %u start frames=%d time=%lld ====\n",
            g_capture_id, g_remaining, (long long)time(NULL));
    fflush(g_log);
    fprintf(stderr, "[PLUME-F2] capture %u recording %d frames\n",
            g_capture_id, g_remaining);
    fflush(stderr);
    return 1;
}

void xgpu_plume_f2_request(void)
{
    if (g_remaining > 0)
        return;
    fprintf(stderr,
            "[PLUME-F2] capture requested (%d frames -> %s)\n",
            f2_frames(), f2_log_path());
    fflush(stderr);
    if (!f2_begin_capture())
        return;
#ifdef _WIN32
#if !defined(XRECOMP_PLUME_F2_TEST_NO_BEEP)
    /* Audible confirmation: the nativeish is often launched without a
     * visible console. */
    MessageBeep(MB_OK);
#endif
#endif
}

void xgpu_plume_f2_poll(void)
{
#ifdef _WIN32
    static SHORT prev;
    const SHORT now = GetAsyncKeyState(VK_F2);
    /* A renderer stall can keep this poll from running until after a normal
     * key tap has already been released.  Bit 0 remembers that F2 was pressed
     * since the previous GetAsyncKeyState call, so accept it as well as the
     * ordinary down-edge. */
    if ((now & 0x0001) || ((now & 0x8000) && !(prev & 0x8000)))
        xgpu_plume_f2_request();
    prev = now;
#endif
    /* Persist no-present diagnostics without flushing once per draw. */
    if (g_log && g_remaining > 0)
        fflush(g_log);
}

int xgpu_plume_f2_active(void)
{
    return g_remaining > 0;
}

void xgpu_plume_f2_log(const char *fmt, ...)
{
    if (g_remaining <= 0 || !g_log)
        return;
    if (g_lines == kLineCap) {
        g_lines++;
        fprintf(g_log, "[F2] LINE CAP %u REACHED - capture truncated\n",
                kLineCap);
        return;
    }
    if (g_lines > kLineCap)
        return;
    g_lines++;
    fprintf(g_log, "[F2] ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
}

void xgpu_plume_f2_present_begin(void)
{
    /* Captures start at the F2 request so no-present intervals are visible. */
}

void xgpu_plume_f2_present(int issued, const char *reason,
                           uint32_t present_guest, uint32_t queued)
{
    if (g_remaining <= 0)
        return;
    xgpu_plume_f2_log("present issued=%d reason=%s guest=%08X queued=%u "
                      "frame=%u",
                      issued, reason ? reason : "?", present_guest, queued,
                      g_frame_in_capture);
    if (g_log)
        fflush(g_log);
    if (!issued)
        return;
    g_frame_in_capture++;
    g_remaining--;
    if (g_remaining == 0) {
        fprintf(g_log, "[F2] ==== capture %u end frames=%u lines=%u ====\n",
                g_capture_id, g_frame_in_capture, g_lines);
        fflush(g_log);
        fprintf(stderr,
                "[PLUME-F2] capture %u complete (%u frames, %u lines) -> %s\n",
                g_capture_id, g_frame_in_capture, g_lines, f2_log_path());
        fflush(stderr);
    }
}

void xgpu_plume_f2_shutdown(void)
{
    if (g_log) {
        fflush(g_log);
        fclose(g_log);
        g_log = NULL;
    }
    g_remaining = 0;
}

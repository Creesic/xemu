#ifndef XGPU_NATIVE_WINDOW_H
#define XGPU_NATIVE_WINDOW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XgpuNativeWindowKind {
    XGPU_NATIVE_WINDOW_NONE = 0,
    XGPU_NATIVE_WINDOW_WIN32,
    XGPU_NATIVE_WINDOW_X11,
    XGPU_NATIVE_WINDOW_SDL,
    XGPU_NATIVE_WINDOW_APPLE,
} XgpuNativeWindowKind;

/* Opaque native-window vocabulary shared by the C guest runtime and the
 * C++ Plume adapter. Platform headers stay on the adapter side.
 *
 * WIN32: window = HWND
 * X11:   display = Display*, window = Window
 * SDL:   view = SDL_Window*
 * Apple: window = NSWindow*, view = CAMetalLayer*
 */
typedef struct XgpuNativeWindow {
    XgpuNativeWindowKind kind;
    void *display;
    uintptr_t window;
    void *view;
} XgpuNativeWindow;

#ifdef __cplusplus
}
#endif

#endif /* XGPU_NATIVE_WINDOW_H */

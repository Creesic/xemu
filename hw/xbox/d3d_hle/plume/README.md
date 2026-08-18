# Plume host adapter

This directory adapts the shared Xbox graphics model to the Plume RHI. It is
part of the runtime library and contains no title profile or title-specific
address, asset, shader, or fallback.

## Components

| File | Responsibility |
|---|---|
| `plume_backend_factory.*` | Parses backend choice, validates compiled capability, creates D3D12/Vulkan/Metal interfaces |
| `plume_context.*` | Device, queue, swapchain, command objects, framebuffers, presentation |
| `plume_shader_compiler.*` | DXC DXIL/SPIR-V and Apple SPIRV-Cross/MSL toolchain |
| `plume_pipeline_layout.*` | Stable descriptor and constant binding contract |
| `plume_render_state.*` | Guest blend/depth/raster state normalization |
| `plume_surface_binding.*` | Guest color/depth identity and hosted-surface binding |
| `plume_draw.*` | Resources, shaders, pipeline cache, draw/copy replay |
| `plume_backend.*` | C ABI orchestration, waits, flips, diagnostics, host-frame present |
| `plume_host.h` | Public C host/renderer boundary |

## Backend selection

Build backends with the CMake cache list:

```powershell
cmake -S . -B build -DXRECOMP_GPU_BACKENDS="d3d12;vulkan"
```

Defaults are Windows `d3d12;vulkan`, Linux `vulkan`, and macOS `metal`.
At runtime:

```text
XRECOMP_GPU_BACKEND=auto
XRECOMP_GPU_BACKEND=d3d12
XRECOMP_GPU_BACKEND=vulkan
XRECOMP_GPU_BACKEND=metal
```

Invalid names and unbuilt/unsupported explicit backends fail clearly; there is
no silent fallback to another project renderer.

## Native windows

The host calls `xgpu_plume_set_native_window()` with one of:

- Windows: `XGPU_NATIVE_WINDOW_WIN32`, `window = HWND`;
- Linux: `XGPU_NATIVE_WINDOW_SDL`, `view = SDL_Window*`;
- macOS: `XGPU_NATIVE_WINDOW_APPLE`, `window` and `view` set to the host
  Cocoa/Metal objects.

The application continues to own the window and event loop.

## Shader tools

DXC must support Shader Model 6 and `-spirv`. Configuration probes candidates
from the official hash-pinned embedded release first, then
`XRECOMP_DXC_EXECUTABLE`, `XRECOMP_DXC`, `VULKAN_SDK`, and `PATH`. Windows
executables embed `dxc.exe`, `dxcompiler.dll`, `dxil.dll`, and their official
notices. Runtime verifies and extracts them under the versioned
`%LOCALAPPDATA%\XboxRecompGame\Runtime\DXC` cache, loads `dxcompiler.dll` in
process, and falls back to `dxc.exe` only if needed. An explicit `XRECOMP_DXC`
remains the highest-priority developer override; the embedded payload otherwise
wins over adjacent or configure-time tools.

Successful shaders use a compiler-aware, checksummed disk cache under
`shader_cache/` next to the executable. Set `XRECOMP_SHADER_CACHE=0` to disable
it or `XRECOMP_SHADER_CACHE_DIR` to choose a different cache directory.

Metal additionally builds the pinned `plume_spirv_cross_msl` converter. Runtime
lookup permits `XRECOMP_SPIRV_CROSS_MSL` or an executable-adjacent tool before
the configured default.

## Presentation and diagnostics

`XRECOMP_PRESENT_MODE=vsync|immediate` controls swapchain pacing. Diagnostics
are opt-in and use only `XRECOMP_*` names; they must never change backend
selection or embed title knowledge. `XRECOMP_PLUME_PERF` provides aggregate
performance telemetry, `XRECOMP_PLUME_REPLAY_SPIKES` isolates detailed replay
spike timing, and `XRECOMP_PLUME_FRAMETIME_LOG` records host-present cadence.
The permanent F2 capture described below owns draw, shader, texture, and
present-surface investigation. Closed one-shot flicker, swap, shader-dump,
geometry, and combiner probes were retired after their investigations ended.

`XRECOMP_INTERNAL_RESOLUTION_SCALE=1..6` selects the initial title-neutral
render-target scale. Guest widths, heights, pitches, addresses, viewport math,
and ordinary textures remain logical/native; hosted color/depth allocations,
their raster viewport/scissor/clear rectangles, and render-target-backed
texture sampling use the physical integer-scaled extent. Guest uploads are
linearly expanded for color, guest readbacks are linearly reduced for color
and nearest-reduced for depth, and the physical scene is filtered into the
swapchain at present. This mirrors Xemu's logical/physical surface split
without exposing host resolution to the lifted title. Hosts can call
`xgpu_plume_set_internal_resolution_scale()` at runtime; Plume establishes a
total-order GPU boundary, rebuilds every scale-dependent cached resource, and
filters existing color contents into the replacement allocations.

The former opt-in `--guest-frame-rate <fps>` present limiter was removed
because it stacked a second timing gate on top of VSync and slept on the
single guest thread, starving cooperative kernel and vblank progress.
Presentation cadence is now controlled only by VSync or immediate mode.

This is deliberately an evolving, game-agnostic boundary model. A title may
advance simulation independently of presentation, submit several GPU flips for
one visible frame, or use another clock entirely. Extend the shared policy from
observed behavior instead of adding title addresses or timing constants.

Pressing **F2** in the nativeish window arms a user-triggered draw-stream capture
(`plume_f2_capture.h`): the next `XRECOMP_PLUME_F2_FRAMES` (default 3) issued
presents append every replayed draw — target surface generations, PS/VS
identity, per-stage texture guest address plus content-version hash,
render-state and constant hashes, and every silently skipped draw — to
`plume_f2_capture.log`. It is always compiled in; idle cost is one key poll
per present, and repeated presses append numbered captures to the same log.

Host diagnostics may register layered
`xgpu_plume_register_debug_overlay_provider()` callbacks. On accepted presents
Plume orders them by layer, validates each straight-alpha RGBA8 panel, gives it
a stable host-texture slot, and restores guest texture, shader, and target
state. Logical-space panels join the Xbox scene; host-space panels alpha-blend
at native resolution after output scaling. Menus therefore remain
above full-screen diagnostic panels without either provider knowing about the
other. The shared compositor contains no title-specific sampling or key
handling.

## Validation boundary

The Windows D3D12/Vulkan sources and contract tests build locally. Linux Vulkan
and macOS Metal still require builds and presentation tests on those hosts. A
second materially different Xbox title is also required before compatibility
is considered empirically game-agnostic.

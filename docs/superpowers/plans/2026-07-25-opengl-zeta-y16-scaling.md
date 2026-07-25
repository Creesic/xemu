# OpenGL Scaled Zeta-to-Y16 Alias Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve packed zeta word-plane parity when a linear Y16 texture aliases a scaled OpenGL zeta surface, removing Beetle Adventure Racing's vertical-line corruption.

**Architecture:** Extract the narrow zeta-to-Y16 layout decision into a pure, unit-tested OpenGL helper. Call it before the existing color surface compatibility checks so only an exact linear, one-level, non-cubemap alias reaches the existing scaled slow surface-to-texture path; every unsupported case keeps the current VRAM fallback.

**Tech Stack:** C11, GLib unit tests, Meson/Ninja, xemu's OpenGL NV2A renderer.

## Global Constraints

- Preserve all unrelated dirty worktree changes.
- Change only the OpenGL renderer; do not alter Vulkan or generated shaders.
- Do not add game-specific checks or relax compatibility beyond the approved layout invariants.
- Keep final in-game visual verification with the user.

---

## File Map

- Create `hw/xbox/nv2a/pgraph/gl/surface-texture-compat.h`: pure compatibility input structure and predicate declaration.
- Create `hw/xbox/nv2a/pgraph/gl/surface-texture-compat.c`: exact zeta-to-Y16 layout predicate.
- Modify `hw/xbox/nv2a/pgraph/gl/surface.c`: translate live surface/texture metadata into the pure predicate before existing compatibility checks.
- Modify `hw/xbox/nv2a/pgraph/gl/meson.build`: compile the helper into the OpenGL renderer.
- Create `tests/unit/test-nv2a-surface-texture-compat.c`: accepted Beetle layout and invariant-rejection coverage.
- Modify `tests/unit/meson.build`: register and link the focused unit test.
- Modify `docs/superpowers/specs/2026-07-25-opengl-zeta-y16-scaling-design.md`: record the approved/implemented status after verification.

## Task 1: Add the Failing Compatibility Regression Test

**Files:**

- Create: `hw/xbox/nv2a/pgraph/gl/surface-texture-compat.h`
- Create: `hw/xbox/nv2a/pgraph/gl/surface-texture-compat.c`
- Create: `tests/unit/test-nv2a-surface-texture-compat.c`
- Modify: `tests/unit/meson.build`

- [ ] **Step 1: Declare the pure compatibility boundary**

Add `PGRAPHGLSurfaceTextureLayout` with only the fields needed by the approved invariants:

```c
typedef struct PGRAPHGLSurfaceTextureLayout {
    bool surface_color;
    bool surface_swizzled;
    uint32_t surface_width;
    uint32_t surface_height;
    uint32_t surface_pitch;
    uint32_t surface_bytes_per_pixel;
    bool texture_linear;
    bool texture_cubemap;
    uint32_t texture_levels;
    uint32_t texture_color_format;
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t texture_pitch;
    uint32_t texture_bytes_per_pixel;
} PGRAPHGLSurfaceTextureLayout;

bool pgraph_gl_zeta_to_y16_compatible(
    const PGRAPHGLSurfaceTextureLayout *layout);
```

The header must include its own `stdbool.h` and `stdint.h` dependencies and use the normal xemu include guard.

- [ ] **Step 2: Write the accepted Beetle layout test**

Create a fixture representing the capture:

```c
static PGRAPHGLSurfaceTextureLayout beetle_layout(void)
{
    return (PGRAPHGLSurfaceTextureLayout) {
        .surface_color = false,
        .surface_swizzled = false,
        .surface_width = 640,
        .surface_height = 480,
        .surface_pitch = 2560,
        .surface_bytes_per_pixel = 4,
        .texture_linear = true,
        .texture_cubemap = false,
        .texture_levels = 1,
        .texture_color_format =
            NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16,
        .texture_width = 1280,
        .texture_height = 480,
        .texture_pitch = 2560,
        .texture_bytes_per_pixel = 2,
    };
}
```

Register `/nv2a/surface-texture-compat/beetle-zeta-y16` and assert that the predicate returns true.

- [ ] **Step 3: Write one rejection test per invariant**

Starting from the fixture, assert false after independently changing:

- `surface_color` to true.
- `surface_swizzled` to true.
- `texture_linear` to false.
- `texture_color_format` to a non-Y16 format.
- `texture_cubemap` to true.
- `texture_levels` to 2.
- `texture_height` to 479.
- `texture_pitch` to 2048.
- `texture_width` to 1279 so the row-byte footprints differ.

Use distinct GLib test paths so failures identify the rejected invariant.

- [ ] **Step 4: Add only enough scaffolding to produce a behavioral failure**

Add the predicate definition with an unconditional `return false;`. This is
temporary compile scaffolding, not the compatibility implementation:

```c
bool pgraph_gl_zeta_to_y16_compatible(
    const PGRAPHGLSurfaceTextureLayout *layout)
{
    return false;
}
```

Register and link the test:

Add this entry to the top-level `tests` map:

```meson
'test-nv2a-surface-texture-compat': [
  '../../hw/xbox/nv2a/pgraph/gl/surface-texture-compat.c',
],
```

- [ ] **Step 5: Run the test target and verify the RED state**

Run:

```powershell
build/pyvenv/bin/meson.exe test -C build test-nv2a-surface-texture-compat --print-errorlogs
```

Expected: the Beetle zeta-to-Y16 case fails because the predicate returns
false, while every rejection case passes. This proves the test exercises the
missing behavior rather than failing on test or build scaffolding.

## Task 2: Implement the Exact Layout Predicate

**Files:**

- Modify: `hw/xbox/nv2a/pgraph/gl/surface-texture-compat.c`

- [ ] **Step 1: Implement the minimum predicate**

Return true only when every approved invariant holds:

```c
bool pgraph_gl_zeta_to_y16_compatible(
    const PGRAPHGLSurfaceTextureLayout *layout)
{
    uint64_t surface_row_bytes =
        (uint64_t)layout->surface_width *
        layout->surface_bytes_per_pixel;
    uint64_t texture_row_bytes =
        (uint64_t)layout->texture_width *
        layout->texture_bytes_per_pixel;

    return !layout->surface_color &&
           !layout->surface_swizzled &&
           layout->texture_linear &&
           !layout->texture_cubemap &&
           layout->texture_levels == 1 &&
           layout->texture_color_format ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16 &&
           layout->surface_height == layout->texture_height &&
           layout->surface_pitch == layout->texture_pitch &&
           surface_row_bytes == texture_row_bytes;
}
```

Use 64-bit intermediates so the row-byte comparison cannot overflow at 32-bit widths.

- [ ] **Step 2: Run the focused test and verify the GREEN state**

Run:

```powershell
build/pyvenv/bin/meson.exe test -C build test-nv2a-surface-texture-compat --print-errorlogs
```

Expected: all accepted/rejected compatibility cases pass.

- [ ] **Step 3: Review the helper for scope creep**

Confirm it contains no OpenGL calls, no surface-scale special case, no Vulkan dependency, and no format conversion.

## Task 3: Route the Alias Through the Existing Scaled Slow Path

**Files:**

- Modify: `hw/xbox/nv2a/pgraph/gl/surface.c`
- Modify: `hw/xbox/nv2a/pgraph/gl/meson.build`

- [ ] **Step 1: Compile the helper into the OpenGL renderer**

Add `surface-texture-compat.c` next to `surface.c` in the OpenGL Meson source list.

- [ ] **Step 2: Build live layout metadata in the compatibility function**

In `pgraph_gl_check_surface_to_texture_compatibility()`, include the helper and create a local layout from:

- `surface->color`, `surface->swizzle`, `surface->width`, `surface->height`, `surface->pitch`, and `surface->fmt.bytes_per_pixel`.
- `kelvin_color_format_info_map[shape->color_format].linear` and `.bytes_per_pixel`.
- `shape->cubemap`, `shape->levels`, `shape->color_format`, `shape->width`, `shape->height`, and `shape->pitch`.

Assert the color-format index before indexing the map, following the renderer's existing format-map conventions.

- [ ] **Step 3: Accept the narrow alias before generic dimension rejection**

Call `pgraph_gl_zeta_to_y16_compatible()` before the existing same-width/same-height and `!surface->color` rejections:

```c
if (pgraph_gl_zeta_to_y16_compatible(&layout)) {
    return true;
}
```

Leave the remainder of the color surface compatibility logic byte-for-byte equivalent. The accepted zeta alias will naturally fail `surface_to_texture_can_fastpath()` and enter `render_surface_to_texture_slow()`, which uploads scaled dimensions and later records `binding->scale = pg->surface_scale_factor`.

- [ ] **Step 4: Re-run the focused test**

Run:

```powershell
build/pyvenv/bin/meson.exe test -C build test-nv2a-surface-texture-compat --print-errorlogs
```

Expected: pass.

- [ ] **Step 5: Build the Windows xemu target**

Run:

```powershell
ninja -C build qemu-system-i386.exe
```

Expected: the new helper and `surface.c` compile and the executable links successfully.

- [ ] **Step 6: Commit the implementation atomically**

Review only the files in this plan, then commit:

```powershell
git add hw/xbox/nv2a/pgraph/gl/surface-texture-compat.h hw/xbox/nv2a/pgraph/gl/surface-texture-compat.c hw/xbox/nv2a/pgraph/gl/surface.c hw/xbox/nv2a/pgraph/gl/meson.build tests/unit/test-nv2a-surface-texture-compat.c tests/unit/meson.build
git commit -m "gpu: preserve scaled zeta-to-Y16 aliases"
```

Do not stage any pre-existing dirty files.

## Task 4: Final Verification and Handoff

**Files:**

- Modify: `docs/superpowers/specs/2026-07-25-opengl-zeta-y16-scaling-design.md`

- [ ] **Step 1: Run fresh focused verification**

Run:

```powershell
build/pyvenv/bin/meson.exe test -C build test-nv2a-surface-texture-compat --print-errorlogs
ninja -C build qemu-system-i386.exe
```

Record the exit codes and test counts before making any completion claim.

- [ ] **Step 2: Inspect the final scoped diff**

Run:

```powershell
git diff HEAD^ -- hw/xbox/nv2a/pgraph/gl/surface-texture-compat.h hw/xbox/nv2a/pgraph/gl/surface-texture-compat.c hw/xbox/nv2a/pgraph/gl/surface.c hw/xbox/nv2a/pgraph/gl/meson.build tests/unit/test-nv2a-surface-texture-compat.c tests/unit/meson.build
git status --short
```

Confirm no existing user changes entered the implementation commit.

- [ ] **Step 3: Update the design status**

Change the spec status to `Implemented; in-game visual verification pending` and commit only the spec:

```powershell
git add docs/superpowers/specs/2026-07-25-opengl-zeta-y16-scaling-design.md
git commit -m "docs: record zeta-to-Y16 implementation status"
```

- [ ] **Step 4: Hand off the visual acceptance check**

Ask the user to run Beetle Adventure Racing at 2x OpenGL scale. In a fresh capture, the expected evidence is:

- The alias upload is `GL_R16` at 2560x960 rather than 1280x480.
- The texture binding scale is 2.
- Adjacent output fragments advance by two physical R16 words and stay on one packed word plane.
- The vertical lines at the equivalent of EID 9934 are absent.

Do not claim the visual defect is fully fixed until that run succeeds.

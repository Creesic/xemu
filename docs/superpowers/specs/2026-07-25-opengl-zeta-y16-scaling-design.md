# OpenGL Scaled Zeta-to-Y16 Alias Fix — Design

**Date:** 2026-07-25
**Status:** Implemented; in-game visual verification pending

## Goal

Remove the vertical-line corruption seen in Beetle Adventure Racing at
surface scale factors above 1 while preserving native OpenGL behavior and
leaving unrelated textures and the Vulkan renderer unchanged.

The reproducing capture is
`C:\Users\Tera\Documents\GitHub\renderdoccaps\xemubeetleroam.rdc`. At EID
9934, texture 112853 is an R16 texture created from a D24S8 zeta surface.
At 2x scaling, adjacent host fragments sample alternating 16-bit halves of
each packed depth/stencil value. The odd-word plane is spatially smooth,
while the even-word plane is discontinuous, producing the visible lines.
At native scale, the guest coordinates advance by two 16-bit words per
fragment and remain on one plane.

## Scope

This change applies only to the OpenGL renderer and only to a linear,
single-level, non-cubemap Y16 texture that aliases a compatible linear zeta
surface at the same VRAM base address.

Out of scope:

- Vulkan renderer behavior.
- General zeta-to-color conversion.
- Shader coordinate snapping or game-specific detection.
- Disabling or reducing the configured surface scale.
- Unrelated surface, texture-cache, or Frame Inspector changes.

## Selected approach

Recognize the packed zeta-to-Y16 layout as a surface-backed texture and
route it through the existing slow surface-to-texture conversion path.

The compatibility decision will require all of these invariants:

- The source is a zeta surface, not a color surface.
- The source surface and destination texture are linear.
- The texture format is `NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16`.
- The texture has one mip level and is not a cubemap.
- Surface and texture heights match.
- Surface and texture pitches match.
- Their logical row byte footprints match:
  `surface width * surface bytes per pixel ==
  texture width * texture bytes per pixel`.

If any invariant fails, xemu retains the current VRAM writeback-and-upload
fallback.

## Data flow

Current failing path:

1. The texture lookup finds a zeta surface at the texture's VRAM address.
2. The existing compatibility check rejects all zeta-to-color aliases.
3. xemu downloads and downscales the host D24S8 surface into guest VRAM.
4. The texture cache uploads those bytes as an unscaled 1280x480 R16
   texture and records scale 1.
5. A 2x draw advances one R16 word per host fragment and alternates packed
   word planes.

Corrected path:

1. The compatibility check accepts only the layout described above.
2. `pgraph_gl_render_surface_to_texture()` rejects the direct color
   fastpath for the zeta source and uses the existing slow path.
3. The slow path reads the full scaled D24S8 host surface without
   downscaling.
4. It uploads the same packed byte stream into an R16 texture whose
   dimensions are scaled from the guest Y16 shape.
5. The texture binding records the active surface scale factor. Existing
   shader normalization then advances two physical R16 words per host
   fragment, preserving word-plane parity.

No shader generation or sampling-state changes are required.

## Error handling and safety

The new case is opt-in through exact layout checks. Unsupported or
ambiguous aliases use the existing fallback rather than asserting or
guessing. Existing color surface-to-texture compatibility remains
unchanged.

The implementation must preserve all unrelated dirty worktree changes.

## Testing

Implementation will follow test-first development:

1. Add a focused compatibility regression test that initially fails for
   the D24S8 640x480/pitch-2560 to Y16 1280x480/pitch-2560 alias.
2. Cover rejection cases for mismatched pitch, height, row byte footprint,
   nonlinear/swizzled input, mip levels, cubemaps, and non-Y16 formats.
3. Implement the smallest compatibility change that passes those tests.
4. Run the focused test and the relevant xemu build/test target.
5. Inspect the resulting OpenGL calls at 2x scale: the alias texture should
   be 2560x960 R16 and its binding scale should be 2.

The user will perform final in-game visual verification because the title
and interactive scene are outside the automated test environment. A fresh
capture should show that adjacent output fragments remain on a consistent
16-bit word plane and that the vertical lines are gone.

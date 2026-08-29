from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "hw/xbox/d3d_hle/d3d8_device.c").read_text()
COMBINERS = (ROOT / "hw/xbox/d3d_hle/d3d8_combiners.c").read_text()
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()
DRAW = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text()
HEADER = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.h").read_text()
TRACKER = (ROOT / "hw/xbox/d3d_hle/plume/plume_surface_binding.cpp").read_text()

# Color and zeta generations share one counter, so the recorded surfaceStage
# identity can select either cache without a second per-draw namespace.
assert TRACKER.count("uint64_t next_generation = 1;") == 1
assert "findOrCreate(m_impl->colors, colorKey)" in TRACKER
assert "findOrCreate(m_impl->zetas, zetaKey)" in TRACKER

bind_start = DEVICE.index("int d3d8_PgraphBindSurfaceTextureStage(")
bind_end = DEVICE.index("\nint d3d8_PgraphDownloadSurfaceRange", bind_start)
bind = DEVICE[bind_start:bind_end]
assert "NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_X8_Y24_FIXED" in bind
assert "zeta->format == XGPU_ZETA_Z24S8 && !zeta->floating" in bind
assert "stage, offset, unnormalized_coords, texture_format" in bind

set_start = DRAW.index("bool PlumeDraw::setSurfaceTexture(")
set_end = DRAW.index("\nvoid PlumeDraw::setPresentSurface", set_start)
set_surface = DRAW[set_start:set_end]
assert "m_guestLatestZetaGeneration.find(guest)" in set_surface
assert "m_guestLatestSurfaceGeneration.find(guest)" in set_surface

view_start = DRAW.index("bool PlumeDraw::ensureZetaSampleView(")
view_end = DRAW.index("\nbool PlumeDraw::ensureBackbufferMirror", view_start)
view = DRAW[view_start:view_end]
assert "RenderTextureViewDesc::Texture2D(zeta.format)" in view
assert "RenderSwizzle::R, RenderSwizzle::ONE" in view
assert "RenderSwizzle::ZERO, RenderSwizzle::ZERO" in view
assert "RenderTextureLayout::SHADER_READ" in view
assert "sampledView" in HEADER and "sampledDescSet" in HEADER

# The native driver promotes authored PROJECT2D according to the live binding:
# raw 0x8421 + X8Y24 at stage 1 + a cube at stage 3 => [1, 2, 1, 3].
raw_modes = 0x8421
assert [(raw_modes >> (stage * 5)) & 0x1F for stage in range(4)] == [1, 1, 1, 1]
effective = COMBINERS[
    COMBINERS.index("static uint32_t effective_texture_shader_mode"):
    COMBINERS.index("static int texture_binding_is_3d")
]
assert "state->texture_shadow[stage] && mode == 1u" in effective
assert "state->texture_cube[stage]" in effective
assert "return 2u;" in effective and "return 3u;" in effective
assert "normalized_shadow = format == 0x2Eu || format == 0x2Fu;" in COMBINERS

# X8Y24 PROJECT3D returns the configured depth comparison in every channel;
# this is what makes combiner reads such as r_t1.b meaningful.
assert "shadow_depth%d" in COMBINERS
assert "input.tc%d.z / input.tc%d.w" in COMBINERS
assert "16777215.0" in COMBINERS
assert "shadow_comparison[state->shadow_func]" in COMBINERS
assert 'if ((uint32_t)state == 311u)' in GUEST
assert "value < 0x200u || value > 0x207u" in GUEST
assert "d3d8_combiners_set_shadow_func(value - 0x200u);" in GUEST

# Replay must make the sampled depth readable and reject unsupported feedback
# against the currently bound depth attachment.
assert DRAW.count("transitionZeta(zeta->second,") == 2
assert DRAW.count('SKIP zeta-feedback stage=%u zg=%llu') == 2
assert "views[s] = zeta->second.sampledView.get();" in DRAW
assert "set = zeta->second.sampledDescSet.get();" in DRAW

print("d3d_hle_z24_surface_texture_contract: OK")

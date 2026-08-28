from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "hw/xbox/d3d_hle/d3d8_device.c").read_text()
BACKEND = (ROOT / "hw/xbox/d3d_hle/plume/plume_backend.cpp").read_text()
DRAW = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text()
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()

start = SOURCE.index("int d3d8_PgraphPresentSurface(uint32_t offset)")
end = SOURCE.index("\n}", start)
body = SOURCE[start:end]
set_rt_start = SOURCE.index("int d3d8_PgraphSetRenderTarget(")
set_rt_end = SOURCE.index("\n}", set_rt_start)
set_rt_body = SOURCE[set_rt_start:set_rt_end]
helper_start = SOURCE.index("static PgraphHostSurface *pgraph_fallback_surface(")
helper_end = SOURCE.index("\n}", helper_start)
helper_body = SOURCE[helper_start:helper_end]

# A directly presented panel surface is the authoritative fallback when the
# hardware PCRTC address does not identify an HLE-managed allocation. Merely
# binding another full-panel render target must not replace it.
promote = body.index("g_pgraph_present_surface = surface;")
fallback = body.index("pgraph_fallback_surface()")
assert promote < fallback
assert "g_pgraph_present_surface = surface;" not in set_rt_body
assert helper_body.index("g_pgraph_present_surface") < helper_body.index(
    "g_pgraph_current_surface")

# A COPY swap chain may move a long-lived writable CPU backbuffer to another
# panel allocation. It must inherit the published frame before LockRect reads
# it back, then become the surface watched by vblank refresh.
lock_start = SOURCE.index("void d3d8_PgraphMarkCpuSurfaceLock(")
lock_end = SOURCE.index("\n}", lock_start)
lock_body = SOURCE[lock_start:lock_end]
inherit = lock_body.index("g_pgraph_present_surface->offset, pixels")
promote_lock = lock_body.index("g_pgraph_present_surface = surface;", inherit)
assert inherit < promote_lock
assert "xgpu_plume_blit_surface(" not in lock_body
assert "g_hle_present_snapshot.SwapEffect == D3DSWAPEFFECT_COPY" in GUEST

# CPU surface locks can complete between draw batches without another
# SetRenderTarget. Their upload must happen before the next GPU draw, not at a
# later present where it would overwrite every draw recorded in between.
prepare_start = SOURCE.index("static BOOL prepare_draw(BOOL immediate)")
prepare_end = SOURCE.index("\n}", prepare_start)
prepare_body = SOURCE[prepare_start:prepare_end]
sync = prepare_body.index("sync_cpu_surface(g_pgraph_current_surface,")
vsh = prepare_body.index("d3d8_vsh_prepare_draw")
combiner = prepare_body.index("d3d8_combiners_prepare_draw")
assert sync < vsh < combiner
assert "color_write_mask != 0xFu" in prepare_body
assert SOURCE.count("prepare_draw(immediate)") == 1
assert SOURCE.count("prepare_draw(FALSE)") == 2

# Xbox Swap resolves a multisampled backbuffer whose physical surface extent
# is encoded in MultiSampleType's low nibbles. The HLE entry hook bypasses that
# XDK implementation, so the shared memory-target binding must render the
# resolved logical extent and Swap must classify the enlarged guest resource
# as scanout rather than deferring the live frame forever.
extent_start = GUEST.index(
    "static int d3d_hle_guest_multisample_logical_extent(")
extent_end = GUEST.index("\n}", extent_start)
extent_body = GUEST[extent_start:extent_end]
assert "(multisample >> 4) & 0xFu" in extent_body
assert "multisample & 0xFu" in extent_body
assert "(multisample & 0x3000u) == 0u" in extent_body
assert "g_hle_present_snapshot.SwapEffect != D3DSWAPEFFECT_COPY" in extent_body
assert "(uint64_t)backbuffer_width * sample_width" in extent_body
assert "(uint64_t)backbuffer_height * sample_height" in extent_body

bind_start = GUEST.index(
    "static HRESULT d3d_hle_guest_bind_memory_render_target(")
bind_end = GUEST.index("\n}", bind_start)
bind_body = GUEST[bind_start:bind_end]
normalize_color = bind_body.index(
    "d3d_hle_guest_multisample_logical_extent(")
bind_color = bind_body.index("binding.width = color_width;")
assert normalize_color < bind_color
assert "binding.image_width = color_width;" in bind_body
assert "binding.zeta_width = zeta_width;" in bind_body

swap_start = GUEST.index("HRESULT d3d_hle_guest_swap(uint32_t flags)")
swap_end = GUEST.index("\n}", swap_start)
swap_body = GUEST[swap_start:swap_end]
classify_msaa = swap_body.index("target_is_multisampled_scanout")
present_msaa = swap_body.index("d3d8_PgraphPresentSurfaceForSwap")
assert classify_msaa < present_msaa
assert "d3d_hle_guest_multisample_logical_extent(" in swap_body

present_scale_start = DRAW.index("auto copyPresentTarget = [&]()")
present_scale_end = DRAW.index("auto submitDescriptorBatch", present_scale_start)
present_scale = DRAW[present_scale_start:present_scale_end]
exact_copy = present_scale.index("cl->copyTexture(screenTexture")
filtered_scale = present_scale.index("recordOutputScale(")
assert exact_copy < filtered_scale
assert "physicalOutputWidth()" in present_scale
assert "physicalOutputHeight()" in present_scale

# A partial clear cannot start from a zeroed host target: channels/pixels not
# selected by the clear retain their guest-memory contents. MM3's lighting
# pass depends on RGB clears preserving a pre-existing alpha mask.
clear_start = SOURCE.index("static void clear_rects(")
clear_end = SOURCE.index("\n}", clear_start)
clear_body = SOURCE[clear_start:clear_end]
seed = clear_body.index("sync_cpu_surface(g_pgraph_current_surface, 1)")
record_clear = clear_body.index("xgpu_plume_clear_target")
assert seed < record_clear
assert "color_write_mask != 0xFu || (count && rects)" in clear_body

# Merely binding a fresh render target establishes GPU ownership; stale
# nonzero bytes in a recycled guest allocation must not seed it. Untouched
# surfaces may still seed from CPU memory when actually presented or sampled.
assert "sync_cpu_surface(" not in set_rt_body
sync_start = SOURCE.index("static int sync_cpu_surface(")
sync_end = SOURCE.index("\n}", sync_start)
sync_body = SOURCE[sync_start:sync_end]
assert "!allow_initial_upload" in sync_body
assert sync_body.index("!allow_initial_upload") < sync_body.index(
    "d3d8_cpu_surface_needs_upload"
)

# Rebinding one guest VRAM address with different render-target geometry must
# first resolve the old host generation. Otherwise a partial first write seeds
# its preserved channels from stale guest RAM instead of the rendered image.
preserve_start = SOURCE.index("static int download_color_surface_to_guest(")
preserve_end = SOURCE.index("\n}", preserve_start)
preserve_body = SOURCE[preserve_start:preserve_end]
resolve_old = set_rt_body.index("download_color_surface_to_guest(surface)")
overwrite_metadata = set_rt_body.index("surface->offset = offset;")
assert resolve_old < overwrite_metadata
assert "metadata_changed && surface->offset == offset" in set_rt_body
assert "surface->image_width ? surface->image_width" in preserve_body
assert preserve_body.index("xgpu_plume_download_color_surface") < (
    preserve_body.index("rebaseline_cpu_surfaces_after_gpu_download")
)

# Surface bindings are recorded separately from draws. Direct readback/restore
# must materialize a pending binding before resolving the owner-side latest
# generation, or a geometry change makes the restore target the old surface.
for function, operation in (
    ("xgpu_plume_download_color_surface", "g_draw.downloadColorSurface"),
    ("xgpu_plume_upload_color_surface", "g_draw.uploadColorSurface"),
):
    start = BACKEND.index(f'extern "C" int {function}(')
    end = BACKEND.index("\n}", start)
    body = BACKEND[start:end]
    assert body.index("g_draw.materializeRecordedSurfaces(g_ctx)") < body.index(
        operation
    )

print("d3d_hle_scanout_surface_contract: OK")

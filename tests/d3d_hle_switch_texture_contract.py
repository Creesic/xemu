from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WRAPPER = (ROOT / "hw/xbox/d3d_hle/d3d_hle_device_switch_texture.c").read_text()
FRONTEND = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()
HEADER = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.h").read_text()
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()
DEVICE = (ROOT / "hw/xbox/d3d_hle/d3d8_device.c").read_text()
RENDERER = (ROOT / "hw/xbox/d3d_hle/xgpu_renderer.h").read_text()
PLUME_DRAW = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text()
SURFACE_BINDING = (
    ROOT / "hw/xbox/d3d_hle/plume/plume_surface_binding.cpp"
).read_text()

assert "extern uint32_t g_esi;" in WRAPPER
assert "d3d_hle_device_switch_texture_gen_unused();" in WRAPPER
assert "d3d_hle_guest_adopt_switch_texture(g_esi, g_edx, format);" in WRAPPER
assert "d3d_hle_guest_switch_texture(g_ecx, g_edx, format);" in WRAPPER
assert "d3d_hle_guest_adopt_switch_texture(" not in FRONTEND
assert "d3d_hle_guest_adopt_switch_texture(" in HEADER

start = GUEST.index("int d3d_hle_guest_adopt_switch_texture(")
end = GUEST.index("static void d3d_hle_guest_resource_add_bind_ref", start)
adopter = GUEST[start:end]
assert "d3d_hle_guest_adopt_resource(texture_va)" in adopter
assert "d3d_hle_guest_try_read_u32(texture_va, &common)" in adopter
assert "d3d_hle_guest_try_read_u32(texture_va + 16u, &live_size)" in adopter
assert "texture->data_va" in adopter
assert "texture_va + 12u" in adopter
assert "g_hle_texture_data_index" in adopter

# SetTexture has already recorded the authoritative object for each stage.
# Prefer it over a global Data-address lookup, which may name an older live
# PixelContainer after a streaming allocator reuses the same storage.
switch_start = GUEST.index("void d3d_hle_guest_switch_texture(")
switch_end = GUEST.index("\n}", switch_start)
switch_texture = GUEST[switch_start:switch_end]
bound_lookup = switch_texture.index("g_hle_bindings.texture_resource[stage]")
bound_validation = switch_texture.index(
    "d3d_hle_guest_texture_data_candidate(texture, data)"
)
global_fallback = switch_texture.index("d3d_hle_guest_find_texture_data(data)")
assert bound_lookup < bound_validation < global_fallback

# Direct guest writes do not necessarily pass through LockRect/Register, so a
# matching object/version is not enough to reuse a hosted texture. Validate the
# bytes with QEMU's existing fast hash before accepting the cache hit, and bump
# the shared generation when the published payload changed.
content_hash = switch_texture.index("content_hash = fast_hash(")
cache_bind = switch_texture.index(
    "xgpu_plume_bind_texture_if_cached(&cache_binding)"
)
assert content_hash < cache_bind
assert "texture->uploaded_content_hash != content_hash" in switch_texture
assert "texture->version = ++g_hle_texture_version" in switch_texture
assert "texture->uploaded_content_hash = content_hash" in switch_texture
assert "texture->uploaded_content_hash == content_hash" in switch_texture
assert "binding.version = texture->uploaded_version" in switch_texture
assert "!xgpu_plume_f2_active()" not in switch_texture
assert '#include "qemu/fast-hash.h"' in GUEST

# Resource_Register is the publication boundary for caller-owned texture
# bytes. Streaming may reuse every piece of metadata while replacing the
# payload, so registration must invalidate the hosted texture cache even when
# refresh_external_resource sees no descriptor change.
register_start = GUEST.index("HRESULT d3d_hle_guest_resource_register(")
register_end = GUEST.index("\n}", register_start)
resource_register = GUEST[register_start:register_end]
assert "resource->version = ++g_hle_texture_version" in resource_register

# Compressed textures cannot be render targets. Treating a stale surface at
# the same guest address as a live alias replaced MM3's BC3 light texture with
# an unrelated 32x32 render target in the later lighting passes.
surface_key_start = GUEST.index("static uint32_t d3d_hle_guest_texture_surface_key(")
surface_key_end = GUEST.index("\n}", surface_key_start)
surface_key = GUEST[surface_key_start:surface_key_end]
assert "d3d_hle_guest_format_is_compressed(texture->format)" in surface_key

# A Y16 view of packed Z24S8 may overlap the active color target in guest
# memory. Resolving color there first clobbers the packed depth bytes and also
# leaves a stale color snapshot that CPU-surface sync later uploads over the
# completed frame. SwitchTexture therefore requests depth only; CopyRects
# retains the generic color+depth resolve.
assert "d3d8_PgraphDownloadSurfaceRange(texture->data_va, bytes, 1)" in GUEST
assert "source->data_va, source->data_bytes, 0" in GUEST

# A complete hosted CopyRects is an ordered GPU surface copy. Updating guest
# RAM alone leaves the live destination stale when it is sampled immediately
# (MM3's alpha-only light mask); partial copies keep the generic CPU fallback.
copy_start = GUEST.index("HRESULT d3d_hle_guest_copy_rects(")
copy_end = GUEST.index("\n}", copy_start)
copy_rects = GUEST[copy_start:copy_end]
host_blit = copy_rects.index("xgpu_plume_blit_surface(")
cpu_download = copy_rects.index("d3d8_PgraphDownloadSurfaceRange(")
assert "if (!count && bpp == 4u" in copy_rects
assert host_blit < cpu_download
assert "destination->uploaded_version = destination->version" in copy_rects

# Normal render targets keep guest-storage identity because MM3 reuses its
# transient surface headers. Only an aliased CopyRects destination needs a
# distinct object generation so the source and destination do not collapse.
assert "color_guest_address" not in RENDERER
assert "binding.color_resource = color->data_va;" in GUEST
assert """: (destination->data_va == source->data_va
            ? d3d_hle_guest_surface_resource(destination)
            : destination->data_va);""" in copy_rects
assert """&destination_binding, destination->data_va,
            source->host_handle ? source->host_handle : source->data_va,
            source->data_va""" in copy_rects
assert "ColorKey colorKey(binding.color_resource" in SURFACE_BINDING
assert "color_guest_address" not in SURFACE_BINDING
assert "m_guestLatestSurfaceGeneration.find(srcResource)" in PLUME_DRAW
assert "ids.color_generation == srcGeneration" in PLUME_DRAW
assert "dstGuest = dstGuest ? dstGuest : dstResource;" in PLUME_DRAW
assert "m_latestSurfaceGeneration[dstResource]" in PLUME_DRAW
assert "m_latestSurfaceGeneration[dstGuest]" in PLUME_DRAW

download_start = DEVICE.index("int d3d8_PgraphDownloadSurfaceRange(")
download_end = DEVICE.index("\n}", download_start)
download = DEVICE[download_start:download_end]
assert "!zeta_only && i < g_pgraph_surface_count" in download
assert download.index("!zeta_only && i < g_pgraph_surface_count") < download.index(
    "i < g_pgraph_zeta_count"
)
color_helper_start = DEVICE.index("static int download_color_surface_to_guest(")
color_helper_end = DEVICE.index("\n}", color_helper_start)
color_helper = DEVICE[color_helper_start:color_helper_end]
color_download = color_helper.index("xgpu_plume_download_color_surface")
color_rebaseline = color_helper.index(
    "rebaseline_cpu_surfaces_after_gpu_download", color_download
)
assert color_download < color_rebaseline
assert download.index("download_color_surface_to_guest(surface)") < download.index(
    "i < g_pgraph_zeta_count"
), "GPU color readback must not look like a later CPU write"
zeta_download = download.index("xgpu_plume_download_zeta_surface")
rebaseline = download.index("rebaseline_cpu_surfaces_after_gpu_download", zeta_download)
refreshed = download.index("refreshed++", zeta_download)
assert zeta_download < rebaseline < refreshed, (
    "renderer-owned depth readback must re-baseline overlapping color "
    "fingerprints before they can be mistaken for CPU writes"
)

print("d3d_hle_switch_texture_contract: OK")

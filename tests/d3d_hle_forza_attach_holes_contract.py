"""Only reviewed semantics may close Forza's post-push attach holes."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text(
    encoding="utf-8"
)
API = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest_api.c").read_text(encoding="utf-8")
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text(encoding="utf-8")
MM3 = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_mm3.c").read_text(encoding="utf-8")
DEVICE = (ROOT / "hw/xbox/d3d_hle/d3d8_device.c").read_text(encoding="utf-8")
HOST = (ROOT / "hw/xbox/d3d_hle/plume/plume_host.h").read_text(encoding="utf-8")
DRAW = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text(encoding="utf-8")
SHADERS = "\n".join(
    (ROOT / path).read_text(encoding="utf-8")
    for path in (
        "hw/xbox/d3d_hle/d3d8_combiners.c",
        "hw/xbox/d3d_hle/xps_translate.cpp",
        "hw/xbox/d3d_hle/plume/plume_fixed_function.h",
    )
)

assert "B1(D3DDevice_CreatePalette2, d3d_hle_device_create_palette2_std)" in DISCOVERY
assert "B4(D3DDevice_CreateImageSurface" in DISCOVERY
assert "N2(D3D_AllocContiguousMemory)" in DISCOVERY
assert "#define N2(api)" in DISCOVERY

assert "d3d_hle_device_create_palette2_std" in API
assert "d3d_hle_device_create_image_surface_std" in API
assert "d3d_hle_guest_create_palette2" in GUEST
assert "d3d_hle_guest_create_image_surface" in GUEST

# PersistDisplay retains native allocation/AV state, while its nested GPU work
# continues through the already-hooked CopyRects and idle calls.
assert "N0(D3DDevice_PersistDisplay)" in DISCOVERY
assert "d3d_hle_device_persist_display_std" not in API
assert "d3d_hle_guest_persist_display" not in GUEST

# SetStipple is renderer-owned and snapshots all 32 rows into per-draw state.
assert "B1(D3DDevice_SetStipple, d3d_hle_device_set_stipple_std)" in DISCOVERY
assert "d3d_hle_guest_set_stipple(a[0])" in API
assert "32u * sizeof(*pattern)" in GUEST
assert "d3d8_SetStipple(pattern)" in GUEST
assert "stipple_pattern[32]" in DEVICE
assert "stipple_pattern[32]" in HOST
assert "renderState.stipple_pattern" in DRAW
assert "kProgramConstantFloatCount = 136u" in (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.h").read_text(encoding="utf-8")
assert SHADERS.count("uint4 stipple_pattern[8]") == 3
assert SHADERS.count("stipple_control.x != 0u") == 3

# VBlank is a kernel-event wait in the native XDK body, not a GPU-idle wait.
assert "N0(D3DDevice_BlockUntilVerticalBlank)" in DISCOVERY
assert 'PASS(0x003407D0u, "BlockUntilVerticalBlank")' in MM3
assert "d3d_hle_device_block_until_vertical_blank" not in API
assert "d3d_hle_guest_block_until_vertical_blank" not in GUEST

# The XDK setter stores the callback on its device and the native VBlank ISR
# dispatches it. Replacing the setter records a pointer that Plume never fires.
assert "#define N1(api)" in DISCOVERY
assert "N1(D3DDevice_SetVerticalBlankCallback)" in DISCOVERY
assert "B1(D3DDevice_SetVerticalBlankCallback" not in DISCOVERY

print("d3d_hle_forza_attach_holes_contract: OK")

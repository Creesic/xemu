"""Only reviewed semantics may close Forza's post-push attach holes."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text(
    encoding="utf-8"
)
API = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest_api.c").read_text(encoding="utf-8")
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text(encoding="utf-8")
MM3 = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_mm3.c").read_text(encoding="utf-8")

assert "B1(D3DDevice_CreatePalette2, d3d_hle_device_create_palette2_std)" in DISCOVERY
assert "B4(D3DDevice_CreateImageSurface" in DISCOVERY
assert "N2(D3D_AllocContiguousMemory)" in DISCOVERY
assert "#define N2(api)" in DISCOVERY

assert "d3d_hle_device_create_palette2_std" in API
assert "d3d_hle_device_create_image_surface_std" in API
assert "d3d_hle_guest_create_palette2" in GUEST
assert "d3d_hle_guest_create_image_surface" in GUEST

# Placeholders are blockers, not implementations.
for symbol in ("PersistDisplay", "SetStipple"):
    assert f"B0(D3DDevice_{symbol}" not in DISCOVERY
    assert f"B1(D3DDevice_{symbol}" not in DISCOVERY
assert "d3d_hle_device_persist_display_std" not in API
assert "d3d_hle_device_set_stipple_std" not in API
assert "d3d_hle_guest_persist_display" not in GUEST
assert "d3d_hle_guest_set_stipple" not in GUEST

# VBlank is a kernel-event wait in the native XDK body, not a GPU-idle wait.
assert "N0(D3DDevice_BlockUntilVerticalBlank)" in DISCOVERY
assert 'PASS(0x003407D0u, "BlockUntilVerticalBlank")' in MM3
assert "d3d_hle_device_block_until_vertical_blank" not in API
assert "d3d_hle_guest_block_until_vertical_blank" not in GUEST

print("d3d_hle_forza_attach_holes_contract: OK")

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WRAPPER = (ROOT / "hw/xbox/d3d_hle/d3d_hle_device_switch_texture.c").read_text()
HEADER = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.h").read_text()
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()

assert "extern uint32_t g_esi;" in WRAPPER
assert "d3d_hle_device_switch_texture_gen_unused();" in WRAPPER
assert "d3d_hle_guest_adopt_switch_texture(g_esi, g_edx, format);" in WRAPPER
assert "d3d_hle_guest_switch_texture(g_ecx, g_edx, format);" in WRAPPER
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

print("d3d_hle_switch_texture_contract: OK")

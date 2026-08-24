from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HLE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()
LAYOUT = (ROOT / "hw/xbox/d3d_hle/kernel/xbox_memory_layout.h").read_text()
MESON = (ROOT / "hw/xbox/meson.build").read_text()
D3D8 = (ROOT / "hw/xbox/d3d_hle/d3d8_device.c").read_text()
PLUME_DRAW = (
    ROOT / "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text()

assert "xemu_d3d_hle_guest_heap.c" in MESON
assert "XEMU_D3D_HLE_SYNTHETIC_PHYS_BASE" in HLE
assert "memory_region_init_ram" in HLE
assert "xemu_d3d_hle_map_synthetic_guest_heap" in HLE
assert "xemu_d3d_hle_unmap_synthetic_guest_heap" in HLE
assert "xbox_HeapSyntheticAvailable" in HLE
assert "xbox_HeapSyntheticReset" in HLE
assert "tlb_flush" in HLE
assert "XBOX_HEAP_EXT_SIZE XEMU_D3D_HLE_SYNTHETIC_SIZE" in LAYOUT
assert "return xbox_HeapSyntheticAvailable();" in GUEST
assert "xbox_guest_phys_ptr" in D3D8
assert "xbox_guest_host_to_phys" in D3D8
assert "g_xbox_mem_offset" not in D3D8
assert "xbox_guest_phys_ptr" in PLUME_DRAW
assert "g_xbox_mem_offset" not in PLUME_DRAW

select_start = HLE.index(
    "static const XemuD3DHleProfile *xemu_d3d_hle_select_profile")
select_end = HLE.index("static bool xemu_d3d_hle_read_identity", select_start)
select_body = HLE[select_start:select_end]
match_at = select_body.index("xemu_d3d_hle_validate_profile(profiles[i])")
capability_at = select_body.index("synthetic_allocator_available")
assert match_at < capability_at

reset_start = HLE.index("void xemu_d3d_hle_session_reset")
reset_end = HLE.index("static void xemu_d3d_hle_load_registers", reset_start)
assert "xbox_HeapSyntheticReset();" in HLE[reset_start:reset_end]

registry_start = GUEST.index("void d3d_hle_guest_reset_registry")
registry_end = GUEST.index(
    "static void d3d_hle_guest_release_device", registry_start)
registry_body = GUEST[registry_start:registry_end]
assert "xbox_HeapFree(g_hle_push_scratch_va)" in registry_body
assert "g_hle_push_scratch_va = 0" in registry_body

print("d3d_hle_synthetic_mapping_contract: OK")

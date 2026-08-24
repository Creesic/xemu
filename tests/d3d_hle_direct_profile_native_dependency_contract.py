from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PGR2 = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_pgr2.c").read_text()
DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()
API = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest_api.c").read_text()
HEADER = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.h").read_text()

assert "d3d_hle_get_2d_surface_desc" in HEADER
assert "HLE(0x001C8910u, d3d_hle_get_2d_surface_desc)" in PGR2
assert "B3(Get2DSurfaceDesc, d3d_hle_get_2d_surface_desc)" in DISCOVERY

start = API.index("void d3d_hle_get_2d_surface_desc(void)")
end = API.index("\n}", start)
body = API[start:end]
assert "d3d_hle_guest_stack_u32(0)" in body
assert "d3d_hle_guest_stack_u32(1)" in body
assert "d3d_hle_guest_stack_u32(2)" in body
assert "d3d_hle_guest_stdcall_return(12)" in body
assert "d3d_hle_guest_surface_desc" in body

print("d3d_hle_direct_profile_native_dependency_contract: OK")

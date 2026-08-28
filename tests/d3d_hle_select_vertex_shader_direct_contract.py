from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()
API = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest_api.c").read_text()
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()


assert "A2(D3DDevice_SelectVertexShaderDirect,\n       d3d_hle_device_select_vertex_shader_direct, AX, BX)" in DISCOVERY
assert "d3d_hle_guest_select_vertex_shader_direct(declaration, address);" in API
assert "loaded = &g_hle_loaded_vertex_programs[address];" in GUEST
assert "d3d8_vsh_create_shader(\n        loaded->microcode" in GUEST
assert "d3d_hle_guest_set_vertex_shader(declaration_va);" in GUEST

print("SelectVertexShaderDirect contract OK")

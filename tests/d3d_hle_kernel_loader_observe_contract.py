from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

assert "xemu_d3d_hle_try_resolve_kernel_loader" in SOURCE
assert "XeLoadSection" in SOURCE
assert "XeUnloadSection" in SOURCE
assert "functions_rva" in SOURCE
assert "names_rva" in SOURCE
assert "ordinals_rva" in SOURCE
assert "exec_loader_return_pc" in SOURCE
assert "loader_entry_span_mapped" in SOURCE

vblank_start = SOURCE.index("void xemu_d3d_hle_vblank")
vblank_end = SOURCE.index("void xemu_d3d_hle_publish_overlay", vblank_start)
vblank_body = SOURCE[vblank_start:vblank_end]
assert "xemu_d3d_hle_try_resolve_kernel_loader" in vblank_body

exec_start = SOURCE.index("static bool xemu_d3d_hle_exec")
exec_end = SOURCE.index("static bool xemu_d3d_hle_is_entry", exec_start)
exec_body = SOURCE[exec_start:exec_end]
assert "exec_loader_pc[0]" in exec_body
assert "exec_loader_pc[1]" in exec_body
assert "exec_loader_return_pc" in exec_body
assert "loader_entry_span_mapped" in exec_body

print("d3d_hle_kernel_loader_observe_contract: OK")

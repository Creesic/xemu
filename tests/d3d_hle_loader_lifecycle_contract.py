from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

for token in (
    "XemuD3DHleLoaderKind",
    "XEMU_D3D_LOADER_LOAD",
    "XEMU_D3D_LOADER_UNLOAD",
    "s_loader_call_kind",
):
    assert token in SOURCE

exec_start = SOURCE.index("static bool xemu_d3d_hle_exec")
exec_end = SOURCE.index("static void xemu_d3d_hle_discovery_on_cpu", exec_start)
body = SOURCE[exec_start:exec_end]

# Entry must remember which discrete kernel export was called; a mapped span on
# XeUnloadSection is not a load refcount event.
assert "pc == s_cpu->exec_loader_pc[0]" in body
assert "pc == s_cpu->exec_loader_pc[1]" in body
assert "s_loader_call_kind = XEMU_D3D_LOADER_LOAD" in body
assert "s_loader_call_kind = XEMU_D3D_LOADER_UNLOAD" in body

# Return handling checks the post-call mapping, resets any frozen/active profile
# when coverage changed, and lets pre-profile discovery resume from fresh data.
assert "loader_span_mapped_after" in body
assert "completed_kind == XEMU_D3D_LOADER_UNLOAD" in body
assert "s_profile_checked || s_host_ready" in body
assert "xemu_d3d_hle_queue_session_reset" in body
assert "s_loader_call_kind = XEMU_D3D_LOADER_NONE" in body

reset_start = SOURCE.index("void xemu_d3d_hle_session_reset")
reset_end = SOURCE.index("static void xemu_d3d_hle_load_registers", reset_start)
reset_body = SOURCE[reset_start:reset_end]
assert "s_loader_call_kind = XEMU_D3D_LOADER_NONE" in reset_body

print("d3d_hle_loader_lifecycle_contract: OK")

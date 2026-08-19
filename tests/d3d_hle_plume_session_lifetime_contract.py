from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUEST_H = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.h").read_text()
GUEST_C = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()
HLE_C = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()
PLUME_C = (ROOT / "hw/xbox/d3d_hle/plume/plume_backend.cpp").read_text()

assert "d3d_hle_guest_reset_session" in GUEST_H
assert "xgpu_plume_reset_session" in GUEST_C
assert "void d3d_hle_guest_reset_session" in GUEST_C

session_start = GUEST_C.index("void d3d_hle_guest_reset_session")
session_end = GUEST_C.index("void d3d_hle_guest_teardown_host_device", session_start)
session_body = GUEST_C[session_start:session_end]
assert "d3d_hle_guest_release_device" in session_body
assert "xgpu_plume_reset_session" in session_body
assert "xgpu_plume_teardown_output" not in session_body

reset_start = HLE_C.index("void xemu_d3d_hle_session_reset")
reset_end = HLE_C.index("static void xemu_d3d_hle_load_registers", reset_start)
reset_body = HLE_C[reset_start:reset_end]
assert "d3d_hle_guest_reset_session" in reset_body
assert "d3d_hle_guest_teardown_host_device" not in reset_body

plume_start = PLUME_C.index("static void plume_reset_guest_session_state")
plume_end = PLUME_C.index('extern "C" void xgpu_plume_teardown_output', plume_start)
plume_body = PLUME_C[plume_start:plume_end]
assert "g_draw.reset" in plume_body
assert "g_ctx.reset" not in plume_body
assert "g_ctx.ready" not in plume_body

print("d3d_hle_plume_session_lifetime_contract: OK")

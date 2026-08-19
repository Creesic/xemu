from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GUEST_H = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.h").read_text()
GUEST_C = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()
HOST_H = (ROOT / "hw/xbox/d3d_hle/plume/plume_host.h").read_text()
BACKEND = (ROOT / "hw/xbox/d3d_hle/plume/plume_backend.cpp").read_text()
CONTEXT_H = (ROOT / "hw/xbox/d3d_hle/plume/plume_context.h").read_text()
CONTEXT_CPP = (ROOT / "hw/xbox/d3d_hle/plume/plume_context.cpp").read_text()
DRAW_H = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.h").read_text()
DRAW_CPP = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text()

assert "d3d_hle_guest_reset_registry" in GUEST_H
assert "d3d_hle_guest_teardown_host_device" in GUEST_H
assert "void d3d_hle_guest_reset_registry" in GUEST_C
assert "void d3d_hle_guest_teardown_host_device" in GUEST_C
assert "xgpu_plume_teardown_output" in HOST_H
assert "xgpu_plume_teardown_output" in BACKEND
assert "void reset();" in CONTEXT_H
assert "void PlumeContext::reset" in CONTEXT_CPP
assert "void reset();" in DRAW_H
assert "void PlumeDraw::reset" in DRAW_CPP

print("d3d_hle_session_teardown_contract: OK")

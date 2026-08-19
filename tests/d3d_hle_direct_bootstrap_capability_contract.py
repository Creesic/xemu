from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUEST_H = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.h").read_text()
GUEST_C = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()
HLE_C = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

assert "d3d_hle_guest_synthetic_allocator_available" in GUEST_H
assert "d3d_hle_guest_synthetic_allocator_available" in GUEST_C
assert "BOOTSTRAP_DIRECT" in HLE_C
assert "synthetic_allocator_available" in HLE_C

select_start = HLE_C.index("static const XemuD3DHleProfile *xemu_d3d_hle_select_profile")
select_end = HLE_C.index("static bool xemu_d3d_hle_read_identity", select_start)
select_body = HLE_C[select_start:select_end]
assert "d3d_hle_guest_synthetic_allocator_available" in select_body
assert "skipping that override" in select_body

print("d3d_hle_direct_bootstrap_capability_contract: OK")

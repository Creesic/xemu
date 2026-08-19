from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROFILES = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_profiles.c").read_text()
HEADER = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_profile.h").read_text()
HLE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

assert "bool xemu_d3d_hle_profile_validate(" in PROFILES
assert "bool xemu_d3d_hle_profile_validate(" in HEADER

select_start = HLE.index("static const XemuD3DHleProfile *xemu_d3d_hle_select_profile")
select_end = HLE.index("static void xemu_d3d_hle_load_registers", select_start)
select_body = HLE[select_start:select_end]
assert "xemu_d3d_hle_profile_validate(" in select_body
assert "skipping that override" in select_body

install_start = HLE.index("void xemu_d3d_hle_install")
install_body = HLE[install_start:]
assert "xemu_d3d_hle_profiles_validate(" not in install_body

print("d3d_hle_profile_isolation_contract: OK")

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.h").read_text()
SOURCE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

assert "xemu_d3d_hle_session_reset" in HEADER
assert "static bool xemu_d3d_hle_read_identity" in SOURCE
assert "void xemu_d3d_hle_session_reset" in SOURCE
assert "s_session_reset_queued" in SOURCE

provider_start = SOURCE.index("static int xemu_d3d_hle_overlay_provider")
provider_end = SOURCE.index("static void xemu_d3d_hle_load_registers", provider_start)
provider_body = SOURCE[provider_start:provider_end]
assert "!s_host_ready" in provider_body

reset_start = SOURCE.index("void xemu_d3d_hle_session_reset")
reset_end = SOURCE.index("static void xemu_d3d_hle_load_registers", reset_start)
reset_body = SOURCE[reset_start:reset_end]
assert "d3d_hle_guest_reset_session" in reset_body
assert "s_profile_checked = false" in reset_body
assert "s_profile = NULL" in reset_body
assert "s_pending" in reset_body
assert "s_device_pending" in reset_body
assert reset_body.index("s_host_ready = false") < reset_body.index(
    "d3d_hle_guest_reset_session")

entry_start = SOURCE.index("static bool xemu_d3d_hle_is_entry")
entry_end = SOURCE.index("void xemu_d3d_hle_install", entry_start)
entry_body = SOURCE[entry_start:entry_end]
assert "xemu_d3d_hle_read_identity" in entry_body
assert "xemu_d3d_hle_session_reset" in entry_body

vblank_start = SOURCE.index("void xemu_d3d_hle_vblank")
vblank_body = SOURCE[vblank_start:]
assert "xemu_d3d_hle_read_identity" in vblank_body
assert "async_run_on_cpu" in vblank_body
assert "s_host_ready" in vblank_body

print("d3d_hle_session_identity_contract: OK")

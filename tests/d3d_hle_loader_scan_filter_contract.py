from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HLE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()
DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()
HEADER = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.h").read_text()

# Shared classifier is exported for the loader lifecycle.
assert "bool xemu_d3d_hle_discovery_is_scan_target(" in HEADER
assert "bool xemu_d3d_hle_discovery_is_scan_target(" in DISCOVERY

# Loader entry must record whether the section can affect discovery at all.
assert "s_loader_section_scan_target" in HLE
entry_start = HLE.index("if (!s_loader_call_active) {")
entry_end = HLE.index("return false;", entry_start)
entry_body = HLE[entry_start:entry_end]
assert "xemu_d3d_hle_discovery_is_scan_target" in entry_body

# Loader return handling: streamed data sections (audio/video/track paging)
# must never queue a session reset or coverage recompute. Only sections the
# detector actually scans participate in the lifecycle.
ret_start = HLE.index("const bool success = (int32_t)env->regs[R_EAX] >= 0;")
ret_end = HLE.index("if (!s_loader_call_active) {", ret_start)
ret_body = HLE[ret_start:ret_end]
assert "s_loader_section_scan_target" in ret_body
assert ret_body.count("s_loader_section_scan_target") >= 2

# Once a verdict exists (verified, active, or refused), the per-TB entry
# check must not keep re-reading the XBE identity from guest RAM; vblank
# owns identity-change detection from that point.
is_entry_start = HLE.index("static bool xemu_d3d_hle_is_entry")
is_entry_end = HLE.index("\n}", is_entry_start)
is_entry_body = HLE[is_entry_start:is_entry_end]
assert "if (!s_profile_checked" in is_entry_body.split("read_identity")[0], (
    "identity read must be gated behind !s_profile_checked"
)

# vblank must detect identity changes for refused/verified titles too, not
# only while a Plume host session is live.
vblank_start = HLE.index("void xemu_d3d_hle_vblank(uint32_t pcrtc_start)")
vblank_end = HLE.index("\n}", vblank_start)
vblank_body = HLE[vblank_start:vblank_end]
assert "s_profile_checked" in vblank_body

print("d3d_hle_loader_scan_filter_contract: OK")

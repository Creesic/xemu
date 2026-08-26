from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILE_H = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_profile.h").read_text()
DISCOVERY_C = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()
HLE_C = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

assert "discovery_unsupported_count" in PROFILE_H
assert "automatic_profile.discovery_unsupported_count" in DISCOVERY_C
assert "bootstrap_only_functions" not in DISCOVERY_C
assert "XEMU_D3D_HLE_HOOK_BOOTSTRAP_ONLY, api" not in DISCOVERY_C
assert "automatic_profile.reviewed_blocker_count = scan.unsupported_functions" in DISCOVERY_C

resolve_start = HLE_C.index("static bool xemu_d3d_hle_resolve_loaded_xbe")
resolve_end = HLE_C.index("static const XemuD3DHleHook *xemu_d3d_hle_find_any_hook", resolve_start)
resolve_body = HLE_C[resolve_start:resolve_end]
assert "discovery_mutating_uncovered_count" in resolve_body
assert "discovery_uncovered_abi_count" in resolve_body
assert "XEMU_D3D_HLE_STATUS_PROFILE_REJECTED" in resolve_body
assert "leaving title on NV2A" in resolve_body

print("d3d_hle_automatic_refuse_split_contract: OK")

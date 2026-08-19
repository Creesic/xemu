from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILE_H = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_profile.h").read_text()
DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()
HLE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

assert "discovery_mutating_uncovered_count" in PROFILE_H
assert "discovery_uncovered_abi_count" in PROFILE_H
assert "discovery_name_is_native_safe" in DISCOVERY
assert "discovery_note_unsupported" in DISCOVERY
assert "param_count > XEMU_D3D_HLE_MAX_ABI_ARGS" in DISCOVERY

start = DISCOVERY.index("static void register_symbol")
end = DISCOVERY.index("static int compare_hooks", start)
body = DISCOVERY[start:end]
assert "unsupported_mutating_functions" in DISCOVERY
assert "uncovered_abi_functions" in DISCOVERY

resolve_start = HLE.index("static bool xemu_d3d_hle_resolve_loaded_xbe")
resolve_end = HLE.index("static const XemuD3DHleHook *xemu_d3d_hle_find_any_hook", resolve_start)
resolve_body = HLE[resolve_start:resolve_end]
assert "discovery_mutating_uncovered_count" in resolve_body
assert "discovery_uncovered_abi_count" in resolve_body
assert "leaving title on NV2A" in resolve_body

print("d3d_hle_abi_classification_contract: OK")

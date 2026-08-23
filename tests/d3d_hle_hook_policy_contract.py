from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_profile.h").read_text()

for policy in (
    "XEMU_D3D_HLE_HOOK_REPLACE",
    "XEMU_D3D_HLE_HOOK_NATIVE_THEN_MIRROR",
    "XEMU_D3D_HLE_HOOK_NATIVE_SAFE",
    "XEMU_D3D_HLE_HOOK_BOOTSTRAP_ONLY",
    "XEMU_D3D_HLE_HOOK_OBSERVE",
):
    assert policy in PROFILE

assert "XemuD3DHleHookPolicy policy" in PROFILE
assert "uint8_t observe_class" in PROFILE
assert "XEMU_D3D_HLE_OBSERVE_NONE" in PROFILE
assert "XEMU_D3D_HLE_OBSERVE_SAFE" in PROFILE
assert "XEMU_D3D_HLE_OBSERVE_MUTATING" in PROFILE
assert "XEMU_D3D_HLE_OBSERVE_ABI_HOLE" in PROFILE

print("d3d_hle_hook_policy_contract: OK")

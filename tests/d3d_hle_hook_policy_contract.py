from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROFILE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_profile.h").read_text()
DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()
HLE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

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
assert "hook->policy = binding->policy" in DISCOVERY

# Native-safe automatic entries may execute the original body. Every other
# automatic policy needs an explicit replacement or mirror path.
assert "N0(D3DDevice_MakeSpace)" in DISCOVERY
assert "M6(Direct3D_CreateDevice" in DISCOVERY
assert "M3(Direct3D_CreateDevice" in DISCOVERY
assert "M1(D3DDevice_GetBackBuffer2" in DISCOVERY
assert "M7(D3DDevice_CreateTexture2" in DISCOVERY
assert "MA1(D3DDevice_CreateIndexBuffer2" in DISCOVERY
assert "O0(D3D_CommonSetDebugRegisters)" in DISCOVERY
assert "O2(CDevice_InitializeFrameBuffers)" in DISCOVERY
assert "O6(CMiniport_CreateCtxDmaObject)" in DISCOVERY

# A discovered replacement must never fall through to the native XDK when ABI
# marshalling fails after Plume has claimed ownership.
exec_start = HLE.index("static bool xemu_d3d_hle_exec")
exec_end = HLE.index("static void xemu_d3d_hle_discovery_on_cpu", exec_start)
exec_body = HLE[exec_start:exec_end]
marshal_start = exec_body.index("xemu_d3d_hle_invoke_discovered")
marshal_body = exec_body[marshal_start:]
assert "xemu_d3d_hle_fail_closed_call" in marshal_body
assert "executing the native XDK body" not in marshal_body
assert "XEMU_D3D_HLE_HOOK_BOOTSTRAP_ONLY" in exec_body
assert "bootstrap-only hook executed after activation" in exec_body

# Duplicate addresses are accepted only when the complete binding/ABI policy is
# identical; ambiguous aliases are a refusal input.
assert "discovery_ambiguous_count" in PROFILE
assert "ambiguous_functions" in DISCOVERY
assert "duplicate binding ambiguity" in DISCOVERY

print("d3d_hle_hook_policy_contract: OK")

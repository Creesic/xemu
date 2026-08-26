import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
cc = os.environ.get("CC", "cc")

with tempfile.TemporaryDirectory(prefix="d3d-hle-texture-state-") as tmp:
    exe = Path(tmp) / "d3d_hle_texture_state_semantic_test.exe"
    subprocess.run(
        [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I" + str(ROOT / "hw/xbox/d3d_hle"),
            str(ROOT / "tests/d3d_hle_texture_state_semantic_test.c"),
            "-o",
            str(exe),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(exe)], check=True, cwd=ROOT)

DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()
API = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest_api.c").read_text()
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()
COMBINERS = (ROOT / "hw/xbox/d3d_hle/d3d8_combiners.c").read_text()
DEVICE = (ROOT / "hw/xbox/d3d_hle/d3d8_device.c").read_text()
RENDER_STATE = (
    ROOT / "hw/xbox/d3d_hle/plume/plume_render_state.cpp"
).read_text()
DRAW = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text()
PLUME_INTERFACE = (
    ROOT / "thirdparty/plume/plume_render_interface.h"
).read_text()
PLUME_D3D12 = (ROOT / "thirdparty/plume/plume_d3d12.cpp").read_text()
PLUME_VULKAN = (ROOT / "thirdparty/plume/plume_vulkan.cpp").read_text()
PLUME_METAL = (ROOT / "thirdparty/plume/plume_metal.cpp").read_text()

assert "B3(D3DDevice_SetTextureState_BumpEnv" in DISCOVERY
assert "B2(D3DDevice_SetTextureState_ColorKeyColor" in DISCOVERY
assert "B1(D3D_BlockOnResource" in DISCOVERY
assert "M6(D3DDevice_CreateTexture2" in DISCOVERY
assert "B6(Lock2DSurface" in DISCOVERY
assert "B1(D3D_DestroyResource, d3d_hle_destroy_resource_std)" in DISCOVERY
assert "B1(CDevice_KickOff, d3d_hle_cdevice_kickoff_std)" in DISCOVERY
assert "B0(D3DDevice_EndPushBuffer, d3d_hle_device_end_push_buffer_std)" in DISCOVERY
assert "B4(D3D_CreateStandAloneSurface" in DISCOVERY
for symbol in (
    "D3D_CommonSetDebugRegisters",
    "D3D_UpdateProjectionViewportTransform",
    "CDevice_InitializeFrameBuffers",
    "CDevice_SetStateUP",
    "CDevice_SetStateVB",
    "CMiniport_CreateCtxDmaObject",
    "CMiniport_InitHardware",
):
    assert f"O0({symbol})" not in DISCOVERY
    assert f"O1({symbol})" not in DISCOVERY
    assert f"O2({symbol})" not in DISCOVERY
    assert f"O6({symbol})" not in DISCOVERY
# D3D_MakeRequestedSpace was bootstrap-only (NULL entry, fail-closed after
# attach) until the push slice gave it a real destination. It now grows the
# scratch window and retargets the guest push cursor onto it.
assert "B2(D3D_MakeRequestedSpace, d3d_hle_make_requested_space_std)" in DISCOVERY
assert "O2(D3D_MakeRequestedSpace)" not in DISCOVERY
assert "A3(D3DDevice_SetTextureStageStateNotInline2" in DISCOVERY
assert "A2(D3DDevice_SelectVertexShaderDirect" in DISCOVERY
assert "B5(D3DDevice_SetVertexData4ub" in DISCOVERY
assert "d3d_hle_device_set_texture_state_bump_env_std" in API
assert "d3d_hle_block_on_resource_std" in API
assert "d3d_hle_lock_2d_surface_std" in API
assert "d3d_hle_guest_block_on_resource" in GUEST
lock_2d_start = GUEST.index("void d3d_hle_guest_lock_2d_surface(")
lock_2d_end = GUEST.index("\n}", lock_2d_start)
lock_2d = GUEST[lock_2d_start:lock_2d_end]
assert "d3d_hle_guest_adopt_resource(pixel_container_va)" in lock_2d
assert "XRECOMP_XBOX_D3DRTYPE_SURFACE" in lock_2d
assert "surface_va = pixel_container_va;" in lock_2d
assert "if (surface_va != pixel_container_va)" in lock_2d
assert "d3d_hle_guest_set_xbox_texture_stage_state" in GUEST
assert "case XBOX_D3DTSS_TEXTURETRANSFORMFLAGS:" in GUEST
assert "host_state = XRECOMP_D3DTSS_TEXTURETRANSFORMFLAGS;" in GUEST
assert "d3d_hle_guest_set_vertex_data4ub" in GUEST
assert "d3d8_combiners_set_bump_env" in COMBINERS
assert "d3d8_combiners_set_color_key" in COMBINERS
assert "color_key_mode" in COMBINERS
assert "discard;" in COMBINERS
for value, factor in (
    ("0x8001u", "D3DBLEND_CONSTANTCOLOR"),
    ("0x8002u", "D3DBLEND_INVCONSTANTCOLOR"),
    ("0x8003u", "D3DBLEND_CONSTANTALPHA"),
    ("0x8004u", "D3DBLEND_INVCONSTANTALPHA"),
):
    assert f"case {value}: return {factor};" in GUEST
assert "case NV097_SET_BLEND_COLOR:" in GUEST
assert "XRECOMP_D3DRS_BLEND_COLOR, value" in GUEST
assert "state.blend_color = rs[XRECOMP_D3DRS_BLEND_COLOR];" in DEVICE
assert "case 12: return RenderBlend::BLEND_FACTOR;" in RENDER_STATE
assert "case 14: return RenderBlend::BLEND_FACTOR_ALPHA;" in RENDER_STATE
assert "plume_blend_factor_from_xgpu" in DRAW
assert "virtual void setBlendFactor(RenderColor blendFactor)" in PLUME_INTERFACE
assert "OMSetBlendFactor(blendFactor.rgba)" in PLUME_D3D12
assert "VK_DYNAMIC_STATE_BLEND_CONSTANTS" in PLUME_VULKAN
assert "vkCmdSetBlendConstants(vk, blendFactor.rgba)" in PLUME_VULKAN
assert "setBlendColor(" in PLUME_METAL

print("d3d_hle_shared_blocker_coverage_contract: OK")

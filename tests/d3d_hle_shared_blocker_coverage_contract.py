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

assert "B3(D3DDevice_SetTextureState_BumpEnv" in DISCOVERY
assert "B2(D3DDevice_SetTextureState_ColorKeyColor" in DISCOVERY
assert "B1(D3D_BlockOnResource" in DISCOVERY
assert "M6(D3DDevice_CreateTexture2" in DISCOVERY
assert "B6(Lock2DSurface" in DISCOVERY
assert "O1(D3D_DestroyResource)" in DISCOVERY
assert "O1(CDevice_KickOff)" in DISCOVERY
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
assert "d3d_hle_guest_set_xbox_texture_stage_state" in GUEST
assert "d3d_hle_guest_set_vertex_data4ub" in GUEST
assert "d3d8_combiners_set_bump_env" in COMBINERS
assert "d3d8_combiners_set_color_key" in COMBINERS
assert "color_key_mode" in COMBINERS
assert "discard;" in COMBINERS

print("d3d_hle_shared_blocker_coverage_contract: OK")

import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
cc = os.environ.get("CC", "cc")

with tempfile.TemporaryDirectory(prefix="d3d-hle-tile-") as tmp:
    exe = Path(tmp) / "d3d_hle_tile_semantic_test.exe"
    subprocess.run(
        [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I" + str(ROOT / "hw/xbox/d3d_hle"),
            str(ROOT / "tests/d3d_hle_tile_semantic_test.c"),
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
assert "B2(D3D_SetTileNoWait" in DISCOVERY
assert "B2(D3DDevice_SetTile" in DISCOVERY
assert "B2(D3DDevice_GetTile" in DISCOVERY
assert "d3d_hle_device_set_tile_std" in API
assert "d3d_hle_device_get_tile_std" in API
assert "d3d_hle_guest_set_tile" in GUEST
assert "d3d_hle_guest_get_tile" in GUEST
assert "xbox_d3d8_tile_contains" in GUEST
print("d3d_hle_tile_state_contract: OK")

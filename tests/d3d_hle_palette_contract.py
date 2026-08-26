import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
cc = os.environ.get("CC", "cc")

with tempfile.TemporaryDirectory(prefix="d3d-hle-palette-") as tmp:
    exe = Path(tmp) / "d3d_hle_palette_semantic_test.exe"
    subprocess.run(
        [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I" + str(ROOT / "hw/xbox/d3d_hle"),
            str(ROOT / "tests/d3d_hle_palette_semantic_test.c"),
            "-o",
            str(exe),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(exe)], check=True, cwd=ROOT)

DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text()
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()
assert "B2(D3DDevice_SetPalette" in DISCOVERY
assert "d3d_hle_guest_set_palette" in GUEST
assert "D3DFMT_P8" in GUEST
assert "xbox_d3d8_palette_lookup" in GUEST
assert "P8 texture without bound palette" not in GUEST
palette_guard = GUEST.index(
    "if (texture->format == D3DFMT_P8) {",
    GUEST.index("void d3d_hle_guest_switch_texture"),
)
upload_begin = GUEST.index("D3D HLE Texture Upload Miss", palette_guard)
guard_body = GUEST[palette_guard:upload_begin]
assert "D3D_HLE_RESOURCE_PALETTE" in guard_body
assert "XRECOMP_CPU_RECORDER_ZONE_END" in guard_body
assert "return;" in guard_body
create_palette = GUEST[GUEST.index("uint32_t d3d_hle_guest_create_palette2"):]
create_palette = create_palette[:create_palette.index("\n}")]
assert "xbox_d3d8_palette_entry_count(size << 30)" in create_palette
assert "32u << size" not in create_palette
print("d3d_hle_palette_contract: OK")

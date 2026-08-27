import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRAW = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.cpp").read_text()
HEADER = (ROOT / "hw/xbox/d3d_hle/plume/plume_draw.h").read_text()

# The render-target binding's guest layout must survive until readback.
assert "surface.guestLayout = old.guestLayout;" in DRAW
assert "dstIt->second.guestLayout = destination.layout;" in DRAW
assert "colorIt->second.guestLayout = binding.layout;" in DRAW
assert "uint32_t guestLayout = XGPU_SURFACE_PITCH;" in HEADER

start = DRAW.index("void PlumeDraw::completeDownloadsFrom(")
end = DRAW.index("\n}", start)
download = DRAW[start:end]
pack = download.index("xgpu_plume_pack_guest_color_surface(")
swizzle = download.index("xbox_swizzle_rect(")
assert pack < swizzle
assert "s.guestLayout == XGPU_SURFACE_SWIZZLE" in download

# Exercise the actual helpers: Plume BGRA alpha must become tight A8 bytes,
# then publish and recover byte-exactly through Xbox Morton storage.
source = r'''#include "plume/plume_surface_download.h"
#include "d3d8_swizzle.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    uint8_t bgra[4u * 4u * 4u] = {0};
    uint8_t linear[16] = {0};
    uint8_t swizzled[16] = {0};
    uint8_t recovered[16] = {0};
    for (uint32_t i = 0; i < 16; ++i)
        bgra[i * 4u + 3u] = (uint8_t)(i + 1u);

    assert(xgpu_plume_guest_color_row_bytes(0x19u, 4u) == 4u);
    assert(xgpu_plume_pack_guest_color_surface(
        0x19u, bgra, 16u, linear, 4u, 4u, 4u));
    for (uint32_t i = 0; i < 16; ++i)
        assert(linear[i] == (uint8_t)(i + 1u));

    xbox_swizzle_rect(swizzled, linear, 4u, 4u, 1u);
    xbox_unswizzle_rect(recovered, swizzled, 4u, 4u, 1u);
    assert(memcmp(recovered, linear, sizeof(linear)) == 0);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="d3d-hle-surface-download-") as tmp:
    tmp = Path(tmp)
    src = tmp / "surface_download_test.c"
    exe = tmp / "surface_download_test.exe"
    src.write_text(source)
    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I" + str(ROOT / "hw/xbox/d3d_hle"),
            str(src),
            "-o",
            str(exe),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(exe)], check=True, cwd=ROOT)

print("d3d_hle_surface_download_contract: OK")

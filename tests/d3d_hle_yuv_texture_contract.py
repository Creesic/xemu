"""Contract: YUY2/UYVY (0x24/0x25) video textures must render, not vanish.

Runtime evidence (RSC2 F2 capture, frame-by-frame):

    [F2] hle texture-upload stage=0 ... 640x480 fmt=24 pitch=1280:1280
         bytes=614400 head=82127F12,...      <- live Bink FMV frame, YUY2
    [F2] g 1 route=prog tgt=F0000001 ... tex=-,-,-,-   <- binding DROPPED
    [F2] s4-final avg_bgra=0,0,0,0                     <- black framebuffer
    [F2] present issued=1 reason=device_swap           <- faithfully presented

plume_map_texfmt has no case for 0x24, so setTexture clear_stage()s the
binding and the fullscreen video quad draws texture*diffuse with a null
texture: the black screen. The pipeline presents a correctly rendered
picture of nothing.

Oracle: xemu's NV2A renders these formats via convert_yuy2_to_rgb /
convert_uyvy_to_rgb (hw/xbox/nv2a/pgraph/util.h, BT.601 integer math
298/409/100/208/516). Plume must reuse THOSE functions — not a local
re-derivation — convert to B8G8R8A8 at upload (like the existing 0x11
LIN_R5G6B5 branch), and map both formats to B8G8R8A8_UNORM.
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / \
    "hw/xbox/d3d_hle/plume/plume_draw.cpp"


def extract_function(text, marker):
    start = text.index(marker)
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
    raise AssertionError(f"unbalanced braces after {marker}")


def main():
    text = SRC.read_text(encoding="utf-8", errors="replace")

    map_body = extract_function(text, "static bool plume_map_texfmt")
    for fmt, name in ((0x24, "YUY2"), (0x25, "UYVY")):
        m = re.search(
            r"case\s+0x%02X[uU]?\s*:\s*out\s*=\s*RenderFormat::B8G8R8A8_UNORM"
            % fmt, map_body)
        assert m, (
            f"plume_map_texfmt must map 0x{fmt:02X} ({name}) to "
            "B8G8R8A8_UNORM (converted at upload); unmapped, setTexture "
            "clears the stage and RSC2's Bink FMV quad draws black"
        )

    assert re.search(r'#include\s+".*nv2a/pgraph/util\.h"', text), (
        "plume_draw.cpp must include the NV2A pgraph util.h oracle; the "
        "YUV conversion must be xemu's own (BT.601 integer math), not a "
        "local re-derivation"
    )

    upload = extract_function(text, "void PlumeDraw::uploadRecordedTexture")
    branch = re.search(r"format\s*==\s*0x24[uU]?\s*\|\|\s*format\s*==\s*0x25",
                       upload) or \
             re.search(r"format\s*==\s*0x25[uU]?\s*\|\|\s*format\s*==\s*0x24",
                       upload)
    assert branch, (
        "uploadRecordedTexture needs a conversion branch covering BOTH "
        "0x24 and 0x25"
    )
    assert "convert_yuy2_to_rgb" in upload, (
        "YUY2 conversion must call the pgraph oracle convert_yuy2_to_rgb"
    )
    assert "convert_uyvy_to_rgb" in upload, (
        "UYVY conversion must call the pgraph oracle convert_uyvy_to_rgb"
    )
    print("d3d_hle_yuv_texture_contract: OK")


if __name__ == "__main__":
    sys.exit(main())

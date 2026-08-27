"""Contract: LIN_A8 (0x1F) must upload like A8 (0x19), not fall back to white.

Oracle: XDK pixeljar.hpp g_TextureFormat — 0x1F D3DFMT_LIN_A8 is FMT_8BPP |
FMT_LINEAR; d3d8types.h names it D3DFMT_LIN_A8. Identical texel semantics to
swizzled A8 (alpha-only, RGB read as 1.0), stored row-major with a pitch —
strictly SIMPLER to upload than swizzled A8, which is already supported.

Runtime evidence: RSC2 (build 5849) creates a 192x125 LIN_A8 texture with
row pitch 192 immediately after device creation (non-pow2 dimensions are only
legal for linear formats, so this cannot be satisfied by the swizzled path).
An unmapped format makes every draw sampling it use the white fallback.
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / \
    "hw/xbox/d3d_hle/plume/plume_draw.cpp"


def extract_function(text, name):
    start = text.index(f"{name}(")
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
    raise AssertionError(f"unbalanced braces in {name}")


def main():
    text = SRC.read_text(encoding="utf-8", errors="replace")
    body = extract_function(text, "static bool plume_map_texfmt")
    m = re.search(r"case\s+0x1F[uU]?\s*:\s*out\s*=\s*RenderFormat::R8_UNORM",
                  body)
    assert m, (
        "plume_map_texfmt must map 0x1F (D3DFMT_LIN_A8) to R8_UNORM; "
        "unmapped it silently samples the white fallback (RSC2 creates a "
        "192x125 pitch-192 LIN_A8 texture at boot)"
    )
    # The ordinary 2D alpha-replication view swizzle (RGB=1, A=R) must pair
    # LIN_A8 with A8. Cube-only A8 handling is a separate legal path.
    paired = re.search(
        r"format\s*==\s*0x19[uU]?\s*\|\|\s*format\s*==\s*0x1F[uU]?",
        text,
    )
    assert paired, "expected the 2D A8 component mapping to include LIN_A8"
    window = text[paired.start() : paired.start() + 350]
    assert re.search(
        r"RenderSwizzle::ONE,\s*RenderSwizzle::ONE,\s*"
        r"RenderSwizzle::ONE,\s*RenderSwizzle::R",
        window,
    ), "A8/LIN_A8 must sample as RGB=ONE, A=R"
    print("d3d_hle_lin_a8_texture_contract: OK")


if __name__ == "__main__":
    sys.exit(main())

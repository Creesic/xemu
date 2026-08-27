"""Contract: bytes-per-pixel must match the XDK PixelJar format table.

Oracle: D:/Emulation/xboxsystem/windows/directx/dxg/d3d8/se/pixeljar.hpp
g_TextureFormat[] (FMT_32BPP/FMT_16BPP/FMT_8BPP/FMT_4BPP flags), cross-checked
with d3d8types.h D3DFORMAT values.

Runtime evidence (RSC2 smoke): the title created a 192x125 D3DFMT_LIN_A8
(0x1F) texture with row pitch 192. LIN_A8 is FMT_8BPP (1 byte/px), so pitch
192 is exact; the HLE table's 4-byte default computed tight-pitch 768 and the
fail-closed linear-pitch gate fatally rejected a valid texture:

    [D3D-HLE] rejected linear storage: 192x125x1 fmt=1F row-pitch=192
              tight-pitch=768 tight-bytes=96000
    [D3D-HLE] fatal: linear texture pitch failed (HRESULT=0x80070057)

Fail-closed is only sound when the ground truth it checks against is right.
Every format the XDK table defines must map to the XDK's storage width;
formats the XDK leaves undefined (table entry 0) are unconstrained here.
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "hw/xbox/d3d_hle/d3d_hle_guest.c"

# pixeljar.hpp g_TextureFormat, transcribed: format -> bytes per pixel.
# FMT_4BPP DXT1 and FMT_8BPP DXT2-5 are block-compressed and handled by
# d3d_hle_guest_format_is_compressed, not the per-pixel table.
ORACLE_BPP = {
    0x00: 1,  # L8
    0x01: 1,  # AL8
    0x02: 2,  # A1R5G5B5
    0x03: 2,  # X1R5G5B5
    0x04: 2,  # A4R4G4B4
    0x05: 2,  # R5G6B5
    0x06: 4,  # A8R8G8B8
    0x07: 4,  # X8R8G8B8 / X8L8V8U8
    0x0B: 1,  # P8
    0x10: 2,  # LIN_A1R5G5B5
    0x11: 2,  # LIN_R5G6B5
    0x12: 4,  # LIN_A8R8G8B8 / LIN_Q8W8V8U8
    0x13: 1,  # LIN_L8
    0x16: 2,  # LIN_R8B8
    0x17: 2,  # LIN_G8B8 / LIN_V8U8
    0x19: 1,  # A8
    0x1A: 2,  # A8L8
    0x1B: 1,  # LIN_AL8
    0x1C: 2,  # LIN_X1R5G5B5
    0x1D: 2,  # LIN_A4R4G4B4
    0x1E: 4,  # LIN_X8R8G8B8 / LIN_X8L8V8U8
    0x1F: 1,  # LIN_A8  <- RSC2 crash format
    0x20: 2,  # LIN_A8L8
    0x24: 2,  # UYVY
    0x25: 2,  # YUY2
    0x27: 2,  # R6G5B5 / L6V5U5
    0x28: 2,  # G8B8 / V8U8
    0x29: 2,  # R8B8
    0x2A: 4,  # D24S8
    0x2B: 4,  # F24S8
    0x2C: 2,  # D16_LOCKABLE / D16
    0x2D: 2,  # F16
    0x2E: 4,  # LIN_D24S8
    0x2F: 4,  # LIN_F24S8
    0x30: 2,  # LIN_D16
    0x31: 2,  # LIN_F16
    0x32: 2,  # L16
    0x33: 4,  # V16U16
    0x35: 2,  # LIN_L16
    0x37: 2,  # LIN_R6G5B5 / LIN_L6V5U5
    0x38: 2,  # R5G5B5A1
    0x39: 2,  # R4G4B4A4
    0x3A: 4,  # A8B8G8R8 / Q8W8V8U8
    0x3B: 4,  # B8G8R8A8
    0x3C: 4,  # R8G8B8A8
    0x3D: 2,  # LIN_R5G5B5A1
    0x3E: 2,  # LIN_R4G4B4A4
    0x3F: 4,  # LIN_A8B8G8R8
    0x40: 4,  # LIN_B8G8R8A8
    0x41: 4,  # LIN_R8G8B8A8
}
COMPRESSED = {0x0C, 0x0E, 0x0F}
# Formats absent from the XDK table (entry 0); the HLE table must not claim
# a specific width for them beyond its default bucket.
UNDEFINED = {0x08, 0x09, 0x0A, 0x0D, 0x14, 0x15, 0x18,
             0x21, 0x22, 0x23, 0x26, 0x34, 0x36}


def extract_function(text, name):
    match = re.search(
        rf"(?m)^static\s+[^\n(]+\b{re.escape(name)}\s*"
        rf"\([^;{{}}]*\)\s*{{",
        text,
    )
    assert match, f"function definition not found: {name}"
    start = match.start()
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


def parse_switch(body):
    """Map case value -> returned bytes for the format switch."""
    mapping = {}
    default = None
    pending = []
    for line in body.splitlines():
        for case in re.findall(r"case\s+0x([0-9A-Fa-f]+)", line):
            pending.append(int(case, 16))
        if "default" in line:
            pending.append("default")
        m = re.search(r"return\s+(\d+)", line)
        if m:
            value = int(m.group(1))
            for item in pending:
                if item == "default":
                    default = value
                else:
                    mapping[item] = value
            pending = []
    assert default is not None, "switch must have a default return"
    return mapping, default


def main():
    text = SRC.read_text(encoding="utf-8", errors="replace")
    body = extract_function(text, "d3d_hle_guest_format_bytes_per_pixel")
    mapping, default = parse_switch(body)

    errors = []
    for fmt, expected in sorted(ORACLE_BPP.items()):
        actual = mapping.get(fmt, default)
        if actual != expected:
            errors.append(
                f"fmt 0x{fmt:02X}: HLE says {actual} bytes, XDK PixelJar "
                f"says {expected}"
            )
    for fmt in sorted(mapping):
        if fmt in COMPRESSED:
            errors.append(
                f"fmt 0x{fmt:02X} is block-compressed; it belongs to "
                "d3d_hle_guest_format_is_compressed, not the per-pixel table"
            )
        elif fmt not in ORACLE_BPP and fmt not in UNDEFINED:
            errors.append(f"fmt 0x{fmt:02X} is not a defined D3DFORMAT")
        elif fmt in UNDEFINED:
            errors.append(
                f"fmt 0x{fmt:02X} is undefined in the XDK table; listing it "
                "explicitly claims storage knowledge the oracle lacks"
            )
    assert not errors, "\n" + "\n".join(errors)

    comp = extract_function(text, "d3d_hle_guest_format_is_compressed")
    for fmt in sorted(COMPRESSED):
        assert f"0x{fmt:02X}" in comp or f"0x{fmt:02x}" in comp, (
            f"compressed fmt 0x{fmt:02X} missing from "
            "d3d_hle_guest_format_is_compressed"
        )
    print("d3d_hle_format_bpp_oracle_contract: OK")


if __name__ == "__main__":
    sys.exit(main())

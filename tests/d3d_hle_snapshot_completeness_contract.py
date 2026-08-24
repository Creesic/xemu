"""Contract: no discovery verdict from a partial XBE section snapshot.

Runtime evidence (RSC2 smoke, one run, same title, same disk):

  gen=1: "XBE snapshot: retained 4229152 mapped bytes; 61896 unavailable
          bytes across 1 preload section left zero-filled"
         scan: recognized=86 unsupported=2 (both native-safe)  -> ACTIVATED
  gen=4: full section paged in
         scan: recognized=145 unsupported=20 (17 MUTATING:
         BeginPush/EndPush/KickPushBuffer/InsertFence/BlockOnFence/...)
                                                              -> REFUSED

Symbols living in the zero-filled pages are invisible to OOVPA matching, so
the completeness denominator self-certifies: the fewer pages mapped, the
cleaner the title looks, and activation proceeds with GPU-mutating symbols
unhooked — exactly the split renderer the refuse gate exists to prevent
(black screen; guest crash).

A verdict — activation OR refusal — is only sound on a complete snapshot.
xemu_d3d_hle_discover must treat ANY zero-filled scan-target bytes as
"not scan-ready": set *retryable, emit no scan, return NULL. The armed
resolve path then waits for coverage growth, and the existing 5-second
"D3D section never fully mapped" backstop still refuses titles that never
page their D3D section in.
"""

import sys
from pathlib import Path

SRC = (Path(__file__).resolve().parents[1] /
       "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c")


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
    body = extract_function(text, "const XemuD3DHleProfile *xemu_d3d_hle_discover")

    assert "incomplete_sections" in body, (
        "discover no longer tracks incomplete sections; the completeness "
        "gate needs that evidence"
    )
    gate = body.index("if (incomplete_sections)")
    scan = body.index("XbSDB_GenerateLibraryFilter")
    assert gate < scan, (
        "the incomplete-section gate must run BEFORE any XbSDB scan: a "
        "partial snapshot must produce no verdict at all"
    )
    open_brace = body.index("{", gate)
    depth = 0
    block_end = open_brace
    for i in range(open_brace, len(body)):
        if body[i] == "{":
            depth += 1
        elif body[i] == "}":
            depth -= 1
            if depth == 0:
                block_end = i
                break
    block = body[gate:block_end]
    assert "*retryable = true" in block, (
        "a partial snapshot must be retryable (stay armed, wait for "
        "coverage growth), not a terminal refusal"
    )
    assert "goto out" in block, (
        "a partial snapshot must abort discovery (goto out) instead of "
        "scanning zero-filled pages: symbols in unavailable bytes are "
        "invisible to OOVPA matching, so activation would proceed with "
        "GPU-mutating symbols unhooked (split renderer)"
    )

    # Sliver rule: XBE sections are unaligned; an uncommitted section whose
    # only mapped bytes are edge slivers on pages shared with committed
    # neighbours is effectively ABSENT (those bytes are zero-fill, not code)
    # and must be skipped like copied==0, not treated as partial. Without
    # this, RSC2's va=0055FBA0 section (head sliver 1120 bytes, section
    # committed only minutes later) defers discovery forever.
    loop = body[body.index("for (i = 0; i < header.dwSections"):gate]
    assert "if (!copied)" in loop and "continue" in loop, (
        "fully-absent scan-target sections must be skipped, not deferred: "
        "absent code cannot execute, and the loader commit hook rescans "
        "when it appears"
    )
    assert "head + tail" in loop and "0x1000u" in loop, (
        "sliver-only mapped sections (no interior page, copied <= shared "
        "edge slivers) must be treated as absent, or unaligned sections "
        "that commit late defer discovery forever"
    )
    print("d3d_hle_snapshot_completeness_contract: OK")


if __name__ == "__main__":
    sys.exit(main())

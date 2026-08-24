"""Contract: a transiently unreadable XBE header must never tear down a live
host session.

The vblank identity check is the only identity-change detector once a profile
is active. Guest loader churn (BINKDATA section paging on RSC2) can leave the
XBE header or certificate page briefly unmapped; reading identity then fails
without any title change. The host-ready branch must only queue a session
reset when it DECODED a DIFFERENT identity — never on a failed read alone.
The refused/verified-idle branch already documents this rule; the host-ready
branch must follow it too, because a spurious full-wipe reset destroys the
guest D3D registry while the title still holds live handles (SelectVertexShader
HRESULT=0x80070057 fail-fast).
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "hw/xbox/d3d_hle/xemu_d3d_hle.c"


def extract_function(text, name):
    start = text.index(f"void {name}(")
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
    body = extract_function(text, "xemu_d3d_hle_vblank")

    host_ready = body.index("qatomic_read(&s_host_ready)")
    idle = body.index("else if (s_profile_checked", host_ready)
    branch = body[host_ready:idle]

    assert "!xemu_d3d_hle_read_identity" not in branch, (
        "host-ready vblank branch treats a transiently unreadable XBE header "
        "as an identity change; a failed read alone must not queue a session "
        "reset (loader section churn unmaps the header page briefly)"
    )
    assert "xemu_d3d_hle_read_identity(" in branch, (
        "host-ready vblank branch must still read identity to detect real "
        "title swaps"
    )
    assert "xemu_d3d_hle_queue_session_reset()" in branch, (
        "a decoded different identity must still fully reset the session"
    )
    # The reset must be gated on a successful read AND a decoded difference.
    gate = branch.index("xemu_d3d_hle_read_identity(")
    reset = branch.index("xemu_d3d_hle_queue_session_reset()")
    assert gate < reset, "identity read must precede the reset decision"
    compare = branch.index("s_identity_title_id != title_id")
    assert gate < compare < reset, (
        "reset must require a decoded identity difference, not read failure"
    )
    print("d3d_hle_vblank_identity_contract: OK")


if __name__ == "__main__":
    sys.exit(main())

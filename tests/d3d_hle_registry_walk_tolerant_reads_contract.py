"""Contract: registry-walk revalidation must tolerate unmapped guest pages.

Proven by crash dump qemu-system-i386w.exe.143748.dmp (FAST_FAIL_FATAL_APP_EXIT,
process uptime 23s):

    ucrtbase!abort
    xbox_guest_ptr            (unmapped guest pointer -> abort)
    d3d_hle_guest_read_u32
    d3d_hle_guest_find_texture_data
    d3d_hle_guest_resource_release_bind_ref
    d3d_hle_device_set_texture_std

Guest-state-preserving session resets keep registry slots alive across loader
section churn (RSC2 BINKDATA paging), so a historical slot's object_va can
reference a page the loader has since unmapped. Any function that SPECULATIVELY
revalidates registry slots while searching (candidate checks, live-type checks,
descriptor refreshes) must use d3d_hle_guest_try_read_u32 and treat an
unreadable header as "not this resource" — never the aborting
d3d_hle_guest_read_u32/xbox_guest_ptr fail-fast, which is reserved for
dereferencing state the guest just handed us.
"""

import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "hw/xbox/d3d_hle/d3d_hle_guest.c"

SPECULATIVE_FUNCS = [
    "d3d_hle_guest_texture_data_candidate",
    "d3d_hle_guest_resource_record_matches_live_type",
    "d3d_hle_guest_refresh_external_resource",
]


def extract_function(text, name):
    key = f" {name}("
    start = text.index(key)
    # Skip the forward declaration if present (ends with ';' before any '{').
    while True:
        brace = text.index("{", start)
        semi = text.index(";", start)
        if semi < brace:
            start = text.index(key, start + len(key))
            continue
        break
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
    for name in SPECULATIVE_FUNCS:
        body = extract_function(text, name)
        assert "d3d_hle_guest_read_u32(" not in body, (
            f"{name} blind-reads guest memory with the aborting "
            "d3d_hle_guest_read_u32; stale registry slots can reference "
            "unmapped pages after loader section churn "
            "(crash dump 143748: SetTexture -> find_texture_data -> abort)"
        )
        assert "xbox_guest_ptr(" not in body, (
            f"{name} must not call the aborting xbox_guest_ptr directly"
        )
        assert "d3d_hle_guest_try_read_u32(" in body, (
            f"{name} must revalidate via the tolerant "
            "d3d_hle_guest_try_read_u32 and reject unreadable slots"
        )
    print("d3d_hle_registry_walk_tolerant_reads_contract: OK")


if __name__ == "__main__":
    sys.exit(main())

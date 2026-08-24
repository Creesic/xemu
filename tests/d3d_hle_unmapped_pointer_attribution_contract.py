"""Contract: the unmapped-guest-pointer abort must name its guest caller.

Runtime evidence: RSC2 runs ~2 minutes of FMV (367 vertex-shader adoption
loops), then dies with exactly:

    [D3D-HLE] unmapped guest pointer 000001C0

and a 0xC0000409 abort. No hook name, no history — unattributable. The
xbox_guest_ptr fail-fast is correct (continuing with a wild host pointer
would corrupt guest RAM), but a fail-fast that hides WHICH of 85 hooks
passed the bad pointer converts a 5-minute fix into an archaeology dig.

The abort path must report the active hook (s_active_hook_name), the most
recent hook (s_last_hook_name/s_last_hook_pc), and the hook entry count,
and must invoke the same fatal diagnostic used by d3d_hle_guest_fatal
(trace ring dump) before aborting.
"""

import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "hw/xbox/d3d_hle/xemu_d3d_hle.c"


def extract_function(text, name):
    start = text.index(f"{name}(uint32_t va)")
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
    body = extract_function(text, "uint8_t *xbox_guest_ptr")
    assert "abort()" in body, "fail-fast must remain: no wild host pointers"
    for needed, why in [
        ("s_active_hook_name", "the hook currently executing"),
        ("s_last_hook_name", "the most recent hook before the fault"),
        ("s_last_hook_pc", "the guest PC of that hook"),
        ("s_hook_entry_count", "how deep into the session the fault is"),
        ("xemu_d3d_hle_dump_trace_ring", "the trace ring dump diagnostic"),
    ]:
        assert needed in body, (
            f"unmapped-guest-pointer abort must report {needed} ({why}); "
            "an unattributed abort turned RSC2's 000001C0 fault into "
            "guesswork"
        )
    print("d3d_hle_unmapped_pointer_attribution_contract: OK")


if __name__ == "__main__":
    sys.exit(main())

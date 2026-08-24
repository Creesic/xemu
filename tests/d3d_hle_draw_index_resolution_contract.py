"""Contract: draw paths must not abort on unresolvable guest index data.

Runtime evidence (RSC2, trace-armed run): after ~533K hooks of live menu/FMV
rendering through Plume, the title issued DrawIndexedVertices with an index
argument of 000001C0 — a byte offset whose bound-index-buffer recovery found
no usable record — and the fall-through treated it as an absolute pointer:

    [D3D-HLE] unmapped guest pointer 000001C0
        (active-hook=D3DDevice_DrawIndexedVertices ... hooks=533554)
    -> abort() (0xC0000409), whole process dies mid-race-transition

Hardware oracle: the XDK pushes whatever pointer the caller supplies and the
GPU DMA-reads it; a bad pointer draws garbage for a frame, it does not halt
the console. The HLE cannot read unmapped memory, so the faithful floor is:
probe the resolved index range with tolerant reads; when unreadable, emit a
diagnostic carrying the full binding state (for root-causing WHY the binding
was unresolvable) and drop the draw — matching this function's existing
out-of-range drop precedent — never abort on guest-controlled data.
"""

import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "hw/xbox/d3d_hle/d3d_hle_guest.c"


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
    body = extract_function(text, "d3d_hle_guest_draw_indexed_vertices")

    probe = body.find("d3d_hle_guest_try_read_u32(resolved_indices_va")
    deref = body.find("xbox_guest_ptr(resolved_indices_va)")
    assert probe != -1, (
        "draw_indexed_vertices must probe the resolved index pointer with "
        "d3d_hle_guest_try_read_u32 before dereferencing: an unresolved "
        "offset (RSC2: 000001C0) reaches xbox_guest_ptr and aborts the "
        "whole process on guest-controlled data"
    )
    assert deref != -1 and probe < deref, (
        "the tolerant probe must precede the xbox_guest_ptr dereference"
    )
    guard = body[probe:deref]
    assert "dropped indexed draw" in guard, (
        "the unreadable-index path must log a distinctive diagnostic "
        "('dropped indexed draw') carrying binding state so the "
        "unresolvable binding can be root-caused from the log"
    )
    for field in ("index_resource", "data_va", "indices_va",
                  "resolved_indices_va", "index_count"):
        assert field in guard, (
            f"the drop diagnostic must include {field}: without binding "
            "state the next occurrence is unattributable"
        )
    assert "return;" in guard, (
        "an unreadable index range must DROP the draw (return), matching "
        "the existing out-of-range drop precedent in this function"
    )
    # Probe must cover the END of the index range too: a first-word hit with
    # an unmapped tail still aborts inside the sanitize/copy loop. The guard
    # slice starts at the first tolerant read, so the evidence is the
    # last-word address being probed within it.
    assert "last_va" in guard, (
        "the probe must validate the LAST index word as well; a partially "
        "mapped range still aborts in the index walk"
    )
    before_probe = body[: probe]
    assert "index_count - 1u" in before_probe or "index_bytes" in before_probe, (
        "the last-word address must be derived from index_count so the "
        "probe scales with the draw"
    )
    print("d3d_hle_draw_index_resolution_contract: OK")


if __name__ == "__main__":
    sys.exit(main())

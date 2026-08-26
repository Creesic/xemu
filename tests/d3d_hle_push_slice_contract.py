"""Contract: the push slice bounds its drain and arms a method work list.

The scratch decoder used to walk the whole 16 KB buffer and stop on a zero
header. BeginPush/EndPush know exactly how far the caller wrote, so the walk
must bound on min(explicit end, buffer end) instead of trusting zero-fill.

BeginStateBig and MakeRequestedSpace return nothing: the caller writes its
Count dwords through g_pDevice->pPut ([device+0], limit [device+4]).
Returning a scratch VA they never read would leave those writes on the
native cursor and split the renderer, so both bodies must retarget the
guest cursor. BeginPush does hand back a pointer, so it does not.

Unmodeled methods must log once per distinct method id, not once per
process, so a first attach enumerates the whole work list.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text(encoding="utf-8")
DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text(
    encoding="utf-8"
)


def body(name):
    start = GUEST.index(name)
    brace = GUEST.index("{", start)
    depth = 0
    for i in range(brace, len(GUEST)):
        if GUEST[i] == "{":
            depth += 1
        elif GUEST[i] == "}":
            depth -= 1
            if depth == 0:
                return GUEST[brace:i + 1]
    raise AssertionError(f"unbalanced braces in {name}")


drain = body("static void d3d_hle_guest_drain_push_range(uint32_t end_va)")
# Bounded on the explicit end, not only on a zero header.
assert "end_va" in drain and "end_va < end" in drain
# Still honours the buffer end.
assert "g_hle_push_scratch_bytes" in drain
# The two modeled methods stay; everything else feeds the work list.
assert "0x1EA4u" in drain and "0x0B80u" in drain
assert "d3d_hle_guest_log_unmodeled_push_method" in drain

logger = body("static void d3d_hle_guest_log_unmodeled_push_method(")
# Per-method-id dedupe, not a single process-wide latch.
assert "seen" in logger and "seen_count" in logger
assert "static int logged" not in logger

reserve_bytes = body(
    "static uint32_t d3d_hle_guest_reserve_push_bytes(uint64_t needed)"
)
reserve = body("static uint32_t d3d_hle_guest_reserve_push(uint32_t count)")
# Dword callers convert once; MakeRequestedSpace already supplies bytes.
assert "count * 4u + 0x204u" in reserve
assert "d3d_hle_guest_reserve_push_bytes" in reserve
make_requested = body("void d3d_hle_guest_make_requested_space(")
assert "d3d_hle_guest_reserve_push_bytes(requested)" in make_requested
assert "d3d_hle_guest_reserve_push(requested)" not in make_requested
assert "* 4u" not in reserve_bytes

begin_state_big = body("void d3d_hle_guest_begin_state_big(uint32_t count)")
# Once Plume owns the cursor, preserve native's capacity fast path before any
# drain/clear/rewind. The first call still has to retarget away from NV2A.
capacity = begin_state_big.index("put + count * 4u")
reserve_call = begin_state_big.index("d3d_hle_guest_reserve_push(count)")
assert capacity < reserve_call
assert "put >= g_hle_push_scratch_va" in begin_state_big
assert "limit + 0x200u" in begin_state_big
assert "return;" in begin_state_big[:reserve_call]

# Cursor retarget: required for the two that return nothing, absent from the
# one that hands the caller a pointer.
for name in ("void d3d_hle_guest_begin_state_big(uint32_t count)",
             "void d3d_hle_guest_make_requested_space("):
    assert "d3d_hle_guest_retarget_push_cursor" in body(name), name
assert "retarget" not in body("uint32_t d3d_hle_guest_begin_push(uint32_t count)")

retarget = body(
    "static void d3d_hle_guest_retarget_push_cursor(uint32_t va, uint32_t bytes)"
)
# Writes pPut and pPushLimit through the discovered device global, tolerantly.
assert "xrecomp_d3d_hle_device_global_va" in retarget
assert "d3d_hle_guest_try_read_u32" in retarget
assert "device + 4u" in retarget
assert "va + bytes - 0x200u" in retarget

# Both BeginPush ABIs are bound, and the family is REPLACE.
assert "B1(D3DDevice_BeginPush, d3d_hle_device_begin_push_std)" in DISCOVERY
assert "B2(D3DDevice_BeginPush, d3d_hle_device_begin_push2_std)" in DISCOVERY
assert "B1(D3DDevice_EndPush" in DISCOVERY
assert "B0(D3DDevice_KickPushBuffer" in DISCOVERY
assert "B1(D3DDevice_BeginStateBig" in DISCOVERY
assert "B0(D3DDevice_MakeSpace" in DISCOVERY
assert "N0(D3DDevice_MakeSpace)" not in DISCOVERY

print("d3d_hle_push_slice_contract: OK")

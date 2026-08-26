from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HLE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

# A session reset after activation (same-title executable-flagged section
# churn, e.g. RSC2 BINKDATA) re-verifies the profile but the title never
# calls CreateDevice again, stranding the session at PROFILE_VERIFIED on
# NV2A. The last successful activation parameters must be remembered with
# the identity they belonged to.
assert "s_reactivate_params_va" in HLE
assert "s_reactivate_title_id" in HLE
assert "s_reactivate_timedate" in HLE
assert "s_reactivate_image_size" in HLE

# Saved on successful activation only (shared activation path).
activate_start = HLE.index(
    "static HRESULT xemu_d3d_hle_activate_host_device_common("
)
activate_end = HLE.index("\n}", activate_start)
activate_body = HLE[activate_start:activate_end]
assert "s_reactivate_params_va = parameters_va" in activate_body

# Reactivation must NOT reread the recorded guest parameters pointer: it
# referenced the original CreateDevice call's stack and is dead after a
# session reset. A host-side present-parameter snapshot is required.
react_start = HLE.index(
    "static HRESULT xemu_d3d_hle_reactivate_host_device(void)\n{"
)
react_end = HLE.index("\n}", react_start)
react_body = HLE[react_start:react_end]
assert "d3d_hle_guest_restart_host_device(" in react_body
assert "d3d_hle_guest_start_host_device(" not in react_body
GUEST = (ROOT / "hw/xbox/d3d_hle/d3d_hle_guest.c").read_text()
assert "g_hle_present_snapshot" in GUEST
restart_start = GUEST.index("HRESULT d3d_hle_guest_restart_host_device")
restart_end = GUEST.index("\n}", restart_start)
restart_body = GUEST[restart_start:restart_end]
assert "read_present_parameters" not in restart_body
assert "g_hle_present_snapshot_valid" in restart_body

# On re-verification of the SAME identity, activation is retried directly
# instead of waiting for a CreateDevice hook that already fired in a prior
# generation. Different identities must never reuse saved parameters.
resolve_start = HLE.index("static bool xemu_d3d_hle_resolve_loaded_xbe")
resolve_end = HLE.index("\n}", resolve_start)
resolve_body = HLE[resolve_start:resolve_end]
assert "s_reactivate_params_va" in resolve_body
assert "s_reactivate_title_id == s_identity_title_id" in resolve_body
assert "s_reactivate_timedate == s_identity_timedate" in resolve_body
assert "s_reactivate_image_size == s_identity_image_size" in resolve_body
assert "xemu_d3d_hle_reactivate_host_device()" in resolve_body

# The verdict path must capture identity itself: a stale pre-reset TB can
# reach it without is_entry having re-read identity for the generation,
# and the reactivation gate depends on a valid identity.
assert "xemu_d3d_hle_read_identity(" in resolve_body
assert resolve_body.index("xemu_d3d_hle_read_identity(") < resolve_body.index(
    "s_profile_checked = true"
)

# Completing native bootstrap can re-enter the same automatic CreateDevice
# hook once at the same guest call boundary. Only that exact one-shot replay
# may return the already-completed S_OK; a later/different call stays on the
# normal native-mirror fail-closed path.
assert "s_create_device_reentry_expected" in HLE
assert "s_create_device_reentry_return_pc" in HLE
assert "s_create_device_reentry_parameters_va" in HLE
assert "s_create_device_reentry_output_va" in HLE
assert "s_create_device_reentry_hook_count" in HLE
assert "bool exact_reentry" in HLE
assert "s_create_device_reentry_hook_count <= 64u" in HLE
assert "s_create_device_reentry_expected = false" in HLE
assert "suppressed duplicate automatic" in HLE
assert "xemu_d3d_hle_return_hook(" in HLE

print("d3d_hle_reactivation_contract: OK")

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HLE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()

# The TB-entry gate is evaluated at TRANSLATION time. Any TB translated
# while no profile was live (initial discovery, or the session-reset ->
# re-verify gap) direct-chains into D3D function bodies ungated. If those
# TBs survive a profile transition, hooks silently never fire for the
# affected call paths: Plume owns scanout but receives no draws (black
# screen after same-title reactivation). Every transition to a valid
# profile must therefore flush the translation cache.
resolve_start = HLE.index("static bool xemu_d3d_hle_resolve_loaded_xbe")
resolve_end = HLE.index("\n}", resolve_start)
resolve_body = HLE[resolve_start:resolve_end]
verified_at = resolve_body.index("XEMU_D3D_HLE_STATUS_PROFILE_VERIFIED")
assert "queue_tb_flush(s_cpu)" in resolve_body[verified_at - 600:], (
    "profile verification must queue a TB flush to invalidate ungated "
    "direct chains translated while no profile was live"
)

print("d3d_hle_tb_flush_contract: OK")

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPY_H = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_spy.h").read_text(encoding="utf-8")
SPY_C = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_spy.c").read_text(encoding="utf-8")
MESON = (ROOT / "hw/xbox/meson.build").read_text(encoding="utf-8")
HEADER = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.h").read_text(encoding="utf-8")
DISABLED = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_disabled.c").read_text(encoding="utf-8")

for name in (
    "xemu_d3d_hle_spy_init",
    "xemu_d3d_hle_spy_enabled",
    "xemu_d3d_hle_spy_intern_name",
    "xemu_d3d_hle_spy_bind",
    "xemu_d3d_hle_spy_note",
    "xemu_d3d_hle_spy_dump",
    "xemu_d3d_hle_spy_reset",
    "xemu_d3d_hle_spy_symbol_count",
    "xemu_d3d_hle_spy_called_holes",
    "xemu_d3d_hle_spy_class_name",
    "xemu_d3d_hle_spy_on_f2_poll",
    "xemu_d3d_hle_spy_capture_seen_swap",
):
    assert name in SPY_H
    assert name in SPY_C

assert "XEMU_D3D_HLE_SPY" in SPY_C
assert "XEMU_D3D_HLE_SPY_LOG" in SPY_C
assert "plume_d3d8_census.log" in SPY_C
assert "[D3D-SPY] ignored: Plume frontend is not armed" in SPY_C
assert "[D3D-SPY] ==== census" in SPY_C
assert "called_holes=" in SPY_C
assert "never_called_holes=" in SPY_C
assert "class=unbound-mutating" in SPY_C or 'unbound-mutating' in SPY_C
assert "d3d_hle/xemu_d3d_hle_spy.c" in MESON
assert "xemu_d3d_hle_spy_" not in HEADER
assert "xemu_d3d_hle_spy_" not in DISABLED
assert "atexit" in SPY_C

DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text(encoding="utf-8")
assert '#include "xemu_d3d_hle_spy.h"' in DISCOVERY
assert "XEMU_D3D_HLE_HOOK_OBSERVE" in DISCOVERY
assert "xemu_d3d_hle_spy_enabled" in DISCOVERY
assert "xemu_d3d_hle_spy_intern_name" in DISCOVERY
assert "XEMU_D3D_HLE_OBSERVE_MUTATING" in DISCOVERY
assert "XEMU_D3D_HLE_OBSERVE_SAFE" in DISCOVERY
assert "XEMU_D3D_HLE_OBSERVE_ABI_HOLE" in DISCOVERY
# Unbound hooks are spy-only.
unsupported = DISCOVERY.index("no reviewed canonical binding")
assert "xemu_d3d_hle_spy_enabled" in DISCOVERY[unsupported:unsupported + 800]

HLE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text(encoding="utf-8")
assert '#include "xemu_d3d_hle_spy.h"' in HLE

install_start = HLE.index("void xemu_d3d_hle_install")
install_end = HLE.index("bool xemu_d3d_hle_owns_window", install_start)
assert "xemu_d3d_hle_spy_init" in HLE[install_start:install_end]

resolve_start = HLE.index("static bool xemu_d3d_hle_resolve_loaded_xbe")
resolve_end = HLE.index("static const XemuD3DHleHook *xemu_d3d_hle_find_any_hook", resolve_start)
resolve_body = HLE[resolve_start:resolve_end]
assert "xemu_d3d_hle_spy_enabled" in resolve_body
assert "xemu_d3d_hle_select_profile" in resolve_body
# Exact profile is skipped only under spy; the call must remain for non-spy.
assert resolve_body.index("xemu_d3d_hle_spy_enabled") < resolve_body.index("xemu_d3d_hle_select_profile")
assert "xemu_d3d_hle_spy_bind" in resolve_body
assert "D3D8 spy on NV2A:" in resolve_body
assert "leaving title on NV2A" in resolve_body

exec_start = HLE.index("static bool xemu_d3d_hle_exec")
exec_end = HLE.index("static void xemu_d3d_hle_discovery_on_cpu", exec_start)
exec_body = HLE[exec_start:exec_end]
assert "xemu_d3d_hle_spy_note" in exec_body
assert "xemu_d3d_hle_activate_host_device" in exec_body
assert exec_body.index("xemu_d3d_hle_spy_note") < exec_body.index("xemu_d3d_hle_activate_host_device")
assert "[F2] call" not in exec_body or "xgpu_plume_f2_log" in exec_body
assert "spy-swap" in exec_body
assert "call %s class=%s" in exec_body

vblank_start = HLE.index("void xemu_d3d_hle_vblank")
# next public function
vblank_end = HLE.index("void xemu_d3d_hle_publish_overlay", vblank_start)
vblank_body = HLE[vblank_start:vblank_end]
assert "xgpu_plume_f2_poll" in vblank_body
assert "xemu_d3d_hle_spy_on_f2_poll" in vblank_body
assert "spy-vblank" in vblank_body
# Poll happens even when the host is not ready.
host_ready_returns = list(__import__("re").finditer(r"if \(!qatomic_read\(&s_host_ready\)\)\s*return;", vblank_body))
assert host_ready_returns, "vblank must still refuse Plume present when !host_ready"
assert vblank_body.index("xgpu_plume_f2_poll") < host_ready_returns[-1].start()

reset_start = HLE.index("void xemu_d3d_hle_session_reset")
reset_end = HLE.index("static void xemu_d3d_hle_load_registers", reset_start)
reset_body = HLE[reset_start:reset_end]
assert "xemu_d3d_hle_spy_dump" in reset_body
assert "xemu_d3d_hle_spy_reset" in reset_body
assert reset_body.index("xemu_d3d_hle_spy_dump") < reset_body.index("s_profile = NULL")
assert reset_body.index("xemu_d3d_hle_spy_dump") < reset_body.index("xemu_d3d_hle_spy_reset")

README = (ROOT / "hw/xbox/d3d_hle/README.md").read_text(encoding="utf-8")
assert "XEMU_D3D_HLE_SPY" in README
assert "SPY-CENSUS.md" in README

print("d3d_hle_spy_census_contract: OK")

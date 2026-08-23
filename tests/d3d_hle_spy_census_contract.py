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

print("d3d_hle_spy_census_contract: OK")

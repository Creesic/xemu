from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
XEMU = (ROOT / "ui/xemu.c").read_text()
HUD = (ROOT / "ui/xui/xemu-hud.h").read_text()
HLE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text()
CONTEXT = (ROOT / "hw/xbox/d3d_hle/plume/plume_context.cpp").read_text()

# A dedicated D3D output child window must exist so the DXGI flip-model
# swapchain never shares an HWND with SDL's WGL presentation (mixing the
# two is undefined; windowed DWM keeps compositing the stale GL
# redirection surface, which shows up as a frozen window that only
# unfreezes in borderless fullscreen independent-flip).
assert "xemu_get_d3d_output_window_handle" in HUD
assert "xemu_get_d3d_output_window_handle" in XEMU
assert "d3d_output_window_sync" in XEMU
assert "HTTRANSPARENT" in XEMU
assert "WS_CHILD" in XEMU
assert "SW_SHOWNA" in XEMU

# The GL path must never present to the output child, and the child is
# shown only while Plume owns presentation.
assert "xemu_d3d_hle_owns_window()" in XEMU

# Activation must target the dedicated output window, not the SDL HWND.
assert "xemu_get_d3d_output_window_handle()" in HLE
activate_start = HLE.index(
    "static HRESULT xemu_d3d_hle_activate_host_device(uint32_t parameters_va)\n{"
)
activate_end = HLE.index("\n}", activate_start)
activate_body = HLE[activate_start:activate_end]
assert "xemu_get_d3d_output_window_handle()" in activate_body
assert "xemu_get_native_window_handle()" not in activate_body

# The swapchain must proactively follow output-window resizes; relying on
# acquire failure alone leaves stale extents (and DXGI scaling blur) when
# the child is resized by windowed<->fullscreen transitions.
acquire_start = CONTEXT.index("bool PlumeContext::acquire")
acquire_end = CONTEXT.index("\n}", acquire_start)
acquire_body = CONTEXT[acquire_start:acquire_end]
assert "needsResize()" in acquire_body

print("d3d_hle_output_window_contract: OK")

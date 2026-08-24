"""Forza: a partial non-D3D executable section must not stall discovery.

Forza reports ~5.7 MiB of scan-target pages mapped with 143360 bytes still
unavailable in one preload executable section. Coverage of the named D3D
section is already 100%, so the 5 s 'D3D section never fully mapped'
backstop never fires. The slow-path TCG callback then re-runs discover
and logs the incomplete snapshot on every TB.

UNIVERSAL-SETUP omit-until-100%: XbSDB may only see complete sections.
A 1-99% non-core section is omitted from this job (same as fully absent),
not used to poison the whole scan. Core (named D3D, or .text if no D3D)
partials stay retryable.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DISCOVERY = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c").read_text(
    encoding="utf-8"
)
HLE = (ROOT / "hw/xbox/d3d_hle/xemu_d3d_hle.c").read_text(encoding="utf-8")


def extract(text, start_token, end_token):
    start = text.index(start_token)
    end = text.index(end_token, start)
    return text[start:end]


def main():
    discover = extract(
        DISCOVERY,
        "const XemuD3DHleProfile *xemu_d3d_hle_discover",
        "static uint32_t register_value",
    )
    # Non-core partials are omitted, not counted as incomplete_sections.
    assert "omit-until-100%" in discover or "omit until 100%" in discover
    assert 'discovery_section_name_is(read_guest, &sections[i], "D3D")' in discover
    gate = discover.index("if (incomplete_sections)")
    omit = discover.index("is_core")
    assert omit < gate

    exec_start = HLE.index("static bool xemu_d3d_hle_exec")
    exec_end = HLE.index("static void xemu_d3d_hle_discovery_on_cpu", exec_start)
    exec_body = HLE[exec_start:exec_end]
    pending_end = exec_body.index(
        "if (!xemu_d3d_hle_resolve_loaded_xbe"
    )
    before_resolve = exec_body[:pending_end]
    assert "if (!s_profile_checked)" in before_resolve, (
        "exec must not re-run discover on every slow-path TB while "
        "discovery is still undecided"
    )

    print("d3d_hle_forza_omit_partial_contract: OK")


if __name__ == "__main__":
    raise SystemExit(main())

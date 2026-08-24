# Xbox D3D HLE / Plume frontend provenance

The compatibility frontend in this directory was imported from
`Creesic/xrecomp` at commit
`5b893fb2d57018c023d3238c8e562c09168a6c72` on 2026-08-16.

The MM3 runtime plan and addresses were generated for XBE SHA-256
`bd5a0376a6b678327d1f4a66b90c99d3d936f21a5f3bb02fee22a3afdb52efde`.
Its detector readiness is 90 required / 90 implemented / zero blockers.

The PGR2 profile and additional reviewed ABI wrappers were translated from the
coverage-complete generated dispatch in the local `xrecomp814-PGR2` checkout
at revision `2193751fa1b2ea05f53315cc3574e388c7669af9`. Its source XBE SHA-256
is `217e5856d646920968f69b063a242c300de42927ecd1de5ec191c4a88e1de8de`;
the detector runtime plan reports 94 required / 94 implemented / zero blockers,
and the generated dispatch contains 99 D3D HLE entries. Xemu's first 99 PGR2
entries were checked against those address/wrapper pairs. The xemu full-system
profile adds one reviewed shared wrapper at `0x001C8910` for
`D3D8_Get2DSurfaceDesc`: original disassembly shows that helper dereferencing
the native `D3D__pDevice`, which is intentionally absent under direct
bootstrap, while the shared `d3d_hle_guest_surface_desc` path implements the
same three-argument descriptor contract without native-device coupling.

MM3 and PGR2 addresses and title identity remain in exact compatibility
profiles. Other games are detected at runtime with Cxbx-Reloaded's
`XbSymbolDatabase`, tracked at `thirdparty/xbsymbol-database` from the official
repository at revision `20eced544726f5558c5a408458f38a086cc4e543`.
The library code is MIT licensed. Its D3D8 and D3D8LTCG OOVPA data are
ODbL-1.0; only those two signature databases are compiled into this frontend.

The automatic bridge copies the loaded XBE into a scanner snapshot, restricts
matching to its declared D3D8 libraries, and consumes the detector's public
symbol, call-convention, and parameter-location metadata. Shared interception,
ABI normalization, guest-memory, D3D8 state, and Plume code remain
title-neutral. Original-Xbox D3D8 behavior was additionally cross-checked
against the XDK source snapshot under
`D:\Emulation\xboxsystem\windows\directx\dxg\d3d8\se`; that source is research
evidence and is not copied or distributed here.

Plume is tracked separately at `thirdparty/plume`, pinned to public revision
`eeea520b4b6f9c80cfaad9c82d6eb3e707c93100` on branch
`xrecomp/game-agnostic-rhi`. The donor checkout uses local revision
`e032023847802a3b400f664f7b7092bc3e3e709d`, whose only additional renderer
change raises the D3D12 sampler descriptor heap from 1024 to 2048. xemu applies
that reviewed one-line overlay to a generated build source, leaving the pinned
submodule clean.

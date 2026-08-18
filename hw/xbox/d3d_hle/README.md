# xemu Xbox D3D8 HLE to Plume

This is an experimental, opt-in graphics frontend which discovers the Xbox
D3D8 runtime linked into the loaded XBE and routes recognized calls through
the shared Xbox D3D8 compatibility layer and Plume's D3D12 backend. Those
calls bypass PFIFO/PGRAPH.

Normal xemu NV2A emulation remains the default and is unchanged.

## Activation

Initialize the Plume and XbSymbolDatabase dependencies, build xemu on Windows,
and choose **View > Backend > Plume (D3D12 HLE)**. The choice is applied after
restarting xemu. It can also be requested from PowerShell:

```powershell
$env:XEMU_D3D_FRONTEND = 'hle'
& .\qemu-system-i386w.exe
```

Unset the variable, or set it to `nv2a`, to use normal xemu rendering. The
general `Auto` backend setting does not opt into this experimental path.

Shader compilation needs a recent `dxc.exe`. Put it beside the executable, on
`PATH`, or point `XEMU_DXC` at it. `XEMU_D3D_SHADER_CACHE=0` disables the shader
cache. Only D3D12 is built here; `XEMU_PLUME_BACKEND` therefore normally stays
unset or is set to `d3d12`.

## Automatic game discovery

Once Plume is selected, no per-game profile has to be authored for the first
attempt. When a retail XBE is loaded, xemu:

1. snapshots its loaded headers and sections;
2. reads its linked XDK library records;
3. scans only the D3D8 and D3D8LTCG signature databases;
4. converts the detector's stack/register parameter metadata into the reviewed
   HLE wrapper ABI; and
5. enables Plume only if it finds `Direct3D_CreateDevice`, `D3DDevice_Swap`,
   and at least one supported draw path.

Unknown symbols, unsupported parameter forms, and ambiguous duplicate matches
are not intercepted. They keep executing their native XDK bodies. This is
deliberately fail-closed per call: automatic discovery broadens title coverage,
but it does not claim that every Xbox D3D helper already has equivalent Plume
semantics.

MM3 and PGR2 retain exact fingerprinted profiles as compatibility overrides
because those paths contain title-tested bootstrap and object-lifetime policy.
All other XBE images use runtime signature and ABI discovery.

Offline scans currently recover a CreateDevice-capable dispatch for RalliSport
Challenge 2, Forza Motorsport, Halo 2, Sega GT 2002, and Sega GT Online. That
is detector evidence, not live intro/menu/gameplay validation.

## Positive runtime proof

Selecting Plume only arms the detector. It is active for the running game only
after both of these are visible:

- the UI changes to `Active: Plume (D3D12), <profile or automatic title>`; and
- the log prints `automatic scan`, `selected ... with ... D3D entry hooks`, and
  finally `profile active: guest D3D calls now target Plume`.

If discovery rejects the XBE, the UI reports that rejection and xemu does not
claim Plume is active. A build or a successful signature scan alone is not
runtime proof.

## Runtime boundary

The static recompiler's generated dispatch cannot be reused directly in xemu.
The xemu bridge instead:

1. sees prospective D3D entries before TCG selects a translated block;
2. checks the exact MM3/PGR2 profiles, then falls back to runtime discovery;
3. normalizes ordinary stdcall and full-register LTCG variants into a common
   logical argument list;
4. preserves native XDK CreateDevice long enough to create a valid guest
   device, then mirrors it into Plume;
5. lazily adopts guest textures, buffers, and surfaces into the shared D3D8
   state model; and
6. completes bound state, draw, synchronization, and present calls through
   Plume while maintaining the detected guest calling convention.

The generic interception and memory bridge lives in `xemu_d3d_hle.c`.
`xemu_d3d_hle_discovery.c` owns runtime detection and ABI marshalling. The
MM3/PGR2 compatibility overrides remain isolated in their profile translation
units. Backend-neutral D3D8/Plume code remains in the other files in this
directory.

## Extending automatic coverage

Supporting another detector-recognized API is a shared change: add its
canonical API and reviewed wrapper ABI to the binding table. A per-title
profile is reserved for a confirmed compatibility policy that cannot safely be
expressed by the shared runtime bridge.

The compiled exact-profile registry is still validated at startup for complete
detector results, unique sorted hook addresses, required special hooks, and a
valid bootstrap policy. A broken exact profile disables HLE for the run.

## Current limitations

- Windows/D3D12 only.
- Automatic discovery depends on a known XDK D3D8 signature and a reviewed
  shared wrapper. Unsupported or previously unseen compiler variants stay
  native and can still expose missing state or rendering behavior.
- Plume presents directly to xemu's SDL Win32 window. The existing OpenGL HUD
  is rendered into a transparent native-size offscreen target and composited
  after Plume's guest output scale, keeping SDL input and xemu's menus active.
- MM3 has live intro/menu/gameplay validation in this branch. The cross-title
  results above are offline detector validation and still need real runtime QA.
- A successful build proves integration, not gameplay parity or the expected
  frametime improvement. Runtime capture is required for performance claims.

See `PROVENANCE.md` for dependency revisions and licensing.

# D3D8 Spy Census

| Field | Value |
|---|---|
| Title | D3D8 Spy Census |
| Author | XemuPlume / D3D HLE |
| Date | 2026-08-23 |
| Status | Draft |
| Companion | `hw/xbox/d3d_hle/UNIVERSAL-SETUP.md`, `hw/xbox/d3d_hle/README.md` |

Observe-only intercept of every XbSDB-recognized D3D8 symbol while NV2A
still owns the GPU. The output is a per-symbol hit census: what this title
actually calls, which of those calls the shared automatic table already
wraps, and which mutating holes are the next shared-wrapper work.

This is the microscope for growing one automatic Plume setup across MM3,
FM1, and PGR2. It is not a per-title profile format. It does not make
Plume claim Active. A successful census is not gameplay proof.

A senior engineer in this tree should be able to implement from this
document without inventing policy.

## Goals

- With `XEMU_D3D_HLE_SPY` armed and Plume requested, a loaded XBE that
  passes the existing automatic **core gate** (CreateDevice + Swap + a
  draw path) gets a TCG hook on **every** recognized D3D8 symbol,
  including symbols with no `bindings[]` row.
- Native XDK bodies always run. Plume never starts. `s_host_ready` stays
  false. Status never becomes `XEMU_D3D_HLE_STATUS_ACTIVE`.
- Exact MM3/PGR2 profiles are skipped while spy is on, so the census
  describes the shared automatic path.
- Always-on hit counters. F2 records a bounded argument stream of the
  same intercepts. A census file lists wrapped / mutating-unbound /
  called holes / never-called holes.
- Spy off is bit-identical to today’s path: exact profiles, refuse
  mutating-uncovered / ambiguous / ABI holes, Plume Active.

## Non-goals

- Implementing the wrappers the census names.
- Changing refuse-split to “called-only”.
- Removing the MM3 or PGR2 exact profiles.
- Spy-while-Plume-Active (same hooks, a later slice).
- A new HUD surface, binary capture format, or per-title saved config.
- Relaxing the core gate. Spy still needs CreateDevice, Swap, and a
  draw path before it will install hooks.
- KVM. The TCG intercept remains TCG-only.

## Arming

Read once in `xemu_d3d_hle_install`, stored in `xemu_d3d_hle_spy.c`.

| Env | Meaning |
|---|---|
| `XEMU_D3D_HLE_SPY` | Armed when the value is present and `atoi` is greater than zero. `"1"` is the documented spelling. `"0"`, empty, and unset are off. |
| `XEMU_D3D_HLE_SPY_LOG` | Census path. Default `plume_d3d8_census.log` in the process working directory. |

Spy is a modifier on an already-requested Plume session
(`XEMU_D3D_FRONTEND=hle` or View > Backend > Plume). If spy is set and
HLE is not requested, log one line
`[D3D-SPY] ignored: Plume frontend is not armed` and do nothing. There
is no intercept without `xemu_d3d_hle_install`’s TCG gate.

## Session shape

```
install (spy on, HLE requested)
  → Armed (same as today)
  → automatic discover only (skip xemu_d3d_hle_select_profile)
  → core gate unchanged (CreateDevice + Swap + draw path)
  → mutating-uncovered / ambiguous / ABI-hole do NOT refuse
  → hook table = every recognized symbol
  → PROFILE_VERIFIED, s_host_ready = false
  → status_detail = "D3D8 spy on NV2A: <N> symbols, <K> called holes"
  → every hooked PC: hits++, maybe F2 line, return false
  → NV2A renders
```

Window title and `xemu_d3d_hle_owns_window()` stay on the NV2A / not-Plume
side because they key off `STATUS_ACTIVE` / `s_host_ready`.

Do not add a `STATUS_SPYING` enum value.

## Discovery

`xemu_d3d_hle_discover` keeps its signature. It asks
`xemu_d3d_hle_spy_enabled()` (false when the spy object was never
initialized).

For each XbSDB-recognized symbol, after the existing `param_void`
normalization and `param_count > 8` / `param_psh2` checks:

| Condition | Hook | `hook->policy` | `hook->entry` | Census class |
|---|---|---|---|---|
| `find_binding` hits | existing row | binding policy | binding entry | `replace` / `mirror` / `native-safe` / `bootstrap-only` from that policy |
| no binding, `discovery_name_is_native_safe` | new | `XEMU_D3D_HLE_HOOK_OBSERVE` | `NULL` | `unbound-safe` |
| no binding, mutating | new | `XEMU_D3D_HLE_HOOK_OBSERVE` | `NULL` | `unbound-mutating` |
| ABI cannot marshal | new | `XEMU_D3D_HLE_HOOK_OBSERVE` | `NULL` | `abi-hole` |

`discovery_note_unsupported` still runs for the last three rows so the
profile counters (`discovery_mutating_uncovered_count`,
`discovery_uncovered_abi_count`, `discovery_unsupported_count`) remain
honest. Spy does not pretend those symbols are reviewed wrappers.

Hook names for OBSERVE rows are interned by
`xemu_d3d_hle_spy_intern_name(str, len)` into a process-lifetime arena.
XbSDB strings are not retained. Binding rows keep pointing at the
static `bindings[].name` literals.

When spy is off, `register_symbol` does not add a hook for unsupported
symbols. That is today’s control case; the refuse-split contract
depends on it.

Duplicate-address handling is unchanged: identical bindings stay one
hook; ambiguous aliases increment `ambiguous_functions` and do not add
a second hook. Spy cannot see two names at one PC.

OBSERVE hooks still fill `source_param_count` / `source_params` when
`convert_param` succeeded for that index. ABI-hole rows may hold a
prefix. F2 then uses `xemu_d3d_hle_discovered_argument` the same way
as bound automatic hooks.

Cap remains `XEMU_D3D_HLE_MAX_DISCOVERED_HOOKS` (384). Overflow logs
`[D3D-SPY] hook table full, dropping <name>` and continues.

After a successful scan, `xemu_d3d_hle.c` calls
`xemu_d3d_hle_spy_bind(s_profile)` so the sidecar has one slot per
hook (address, interned name, class, hits=0). `reviewed_*_hook_count`
on the automatic profile includes OBSERVE rows (they are installed
hooks). They are not “implemented” wrappers; the census class says so.

## Exec

In `xemu_d3d_hle_exec`, after `xemu_d3d_hle_profile_find_hook` succeeds
and after `xemu_d3d_hle_load_registers`, when spy is enabled:

1. `xemu_d3d_hle_spy_note(hook)` — `hits++`; sets `capture_seen_swap`
   when the name is `D3DDevice_Swap` or `D3DDevice_Present`.
2. If F2 is active: one `[F2] call` line from `xemu_d3d_hle.c` using
   `xemu_d3d_hle_discovered_argument` (see F2). Spy `.c` does not
   include F2 or `CPUX86State`.
3. If F2 is active and the name is `D3DDevice_Swap` or
   `D3DDevice_Present`, `xgpu_plume_f2_present(1, "spy-swap", 0, 0)`.
   That spends the existing frame budget.
4. `return false`.

This return is **before** the `!s_host_ready` CreateDevice activation
block, before pending-mirror begins, and before `hook->entry()` /
`xemu_d3d_hle_invoke_discovered`. Loader intercepts (`exec_loader_pc`)
are unchanged and still run first.

`XEMU_D3D_HLE_HOOK_OBSERVE` is also a safety net: if a future edit
misses the spy early-out, `entry==NULL` already falls through later in
`exec`. OBSERVE must never invoke a wrapper.

## F2 clock

`xemu_d3d_hle_service_vblank` currently returns on `!s_host_ready`, so
its `xgpu_plume_f2_poll()` never runs in spy. Spy must not change that
early-out (it guards Plume present/scanout).

In `xemu_d3d_hle_vblank`, after the existing identity peek and before
return, when spy is enabled:

1. `xgpu_plume_f2_poll()`.
2. Falling edge of `xgpu_plume_f2_active()`: `xemu_d3d_hle_spy_dump("f2")`.
3. Track a `capture_seen_swap` flag in the spy object. Rising edge of
   `xgpu_plume_f2_active()` clears it. Swap/Present `note` sets it.
   If F2 is active and the flag is still clear,
   `xgpu_plume_f2_present(1, "spy-vblank", 0, 0)`. Three vblanks close
   a no-Swap capture. Hit counters are session-lifetime and are not
   reset by F2; a short F2 still dumps the full census.

Non-spy vblank behavior is unchanged.

## Census file

Append-only. Open lazily on first dump. `fopen` failure: one stderr
line, no abort.

Header then one row per bound hook, sorted by the hook table order
(address order after discover’s `qsort`):

```
[D3D-SPY] ==== census title=0x4D53007D symbols=145 wrapped=128 unbound_mutating=17 called_holes=4 never_called_holes=13 reason=<why> ====
[D3D-SPY] hole D3DDevice_Present va=0034A100 hits=842 class=unbound-mutating
[D3D-SPY] ok   D3DDevice_Swap    va=00343E60 hits=842 class=replace
[D3D-SPY] skip D3DDevice_SetLight va=0034B200 hits=0 class=unbound-mutating
```

| Row tag | When |
|---|---|
| `hole` | class is `unbound-mutating` or `abi-hole`, and `hits > 0` |
| `ok` | wrapped (`replace` / `mirror` / `native-safe` / `bootstrap-only`) |
| `skip` | `unbound-safe`, or a mutating/abi hole with `hits == 0` |

`wrapped` in the header counts `ok` rows. `called_holes` counts `hole`
rows. `never_called_holes` counts mutating/abi rows with `hits == 0`.
Those last rows are **not** the work list.

F2 stream line, only while a capture is recording:

```
[F2] call D3DDevice_DrawIndexedVertices class=replace a0=... a1=... a2=...
```

Arguments: up to `hook->source_param_count` public values via
`xemu_d3d_hle_discovered_argument` for automatic hooks that stored
source params. OBSERVE rows store whatever `convert_param` produced
before the ABI failure; if none, omit `aN`. No guest-memory dumps.

## Dump points and resets

| Event | Dump | Zero hits |
|---|---|---|
| F2 capture falling edge | yes, reason=`f2` | no |
| `xemu_d3d_hle_session_reset` | yes, reason=`reset`, **before** `s_profile = NULL` | yes, after dump |
| Process exit | `atexit` registered once from spy init, reason=`exit` | n/a |

`status_detail` is rewritten on each vblank while spy is bound:
`D3D8 spy on NV2A: <N> symbols, <K> called holes`.

## Files

| File | Change |
|---|---|
| `hw/xbox/d3d_hle/xemu_d3d_hle_spy.h` | **New.** `enabled`, `init`, `intern_name`, `bind`, `note`, `dump`, `reset`. `extern "C"`. |
| `hw/xbox/d3d_hle/xemu_d3d_hle_spy.c` | **New.** Env, arena, sidecar[384], dump writer, atexit. |
| `hw/xbox/d3d_hle/xemu_d3d_hle_profile.h` | Add `XEMU_D3D_HLE_HOOK_OBSERVE`. |
| `hw/xbox/d3d_hle/xemu_d3d_hle_discovery.c` | OBSERVE hooks when spy enabled; intern names. |
| `hw/xbox/d3d_hle/xemu_d3d_hle.c` | Skip exact profile and refuse-split when spy; exec early-out; vblank F2 poll / dump edge; reset dump+`spy_reset`; `spy_init` from install. |
| `hw/xbox/d3d_hle/plume/plume_f2_capture.*` | Unchanged API. Spy is a new caller of `poll` / `present` / `log` / `active`. |
| `hw/xbox/meson.build` | Add `d3d_hle/xemu_d3d_hle_spy.c` next to discovery. |
| `hw/xbox/d3d_hle/xemu_d3d_hle.h` | **No change.** Disabled stubs stay as they are. |
| `hw/xbox/d3d_hle/README.md` | One short “D3D8 spy census” paragraph under Current limitations, pointing here. |

Public spy functions are not part of `xemu_d3d_hle.h`. Only
`xemu_d3d_hle.c` and `xemu_d3d_hle_discovery.c` include `xemu_d3d_hle_spy.h`.

## Spy C API (normative)

```c
void xemu_d3d_hle_spy_init(bool hle_requested);
bool xemu_d3d_hle_spy_enabled(void);
const char *xemu_d3d_hle_spy_intern_name(const char *name, size_t length);
void xemu_d3d_hle_spy_bind(const XemuD3DHleProfile *profile);
void xemu_d3d_hle_spy_note(const XemuD3DHleHook *hook);
void xemu_d3d_hle_spy_dump(const char *reason);
void xemu_d3d_hle_spy_reset(void);
unsigned xemu_d3d_hle_spy_symbol_count(void);
unsigned xemu_d3d_hle_spy_called_holes(void);
```

`init` is called from `xemu_d3d_hle_install` after `s_requested` is
known. `bind` copies hook addresses/names/policies/`observe_class`
into the sidecar; it does not retain `profile` after return. `note`
is a no-op on a NULL hook or when spy has no bound profile. `reset` zeroes hits and
drops the bind; interned names stay allocated for the process.

Class mapping from `XemuD3DHleHookPolicy`:

| Policy | Class string |
|---|---|
| `REPLACE` | `replace` |
| `NATIVE_THEN_MIRROR` | `mirror` |
| `NATIVE_SAFE` | `native-safe` |
| `BOOTSTRAP_ONLY` | `bootstrap-only` |
| `OBSERVE` | from `hook->observe_class` |

Add `uint8_t observe_class` on `XemuD3DHleHook`, zero for non-OBSERVE.
Values: 0=none, 1=unbound-safe, 2=unbound-mutating, 3=abi-hole. Discovery
sets it when creating an OBSERVE row. `bind` copies it into the sidecar.

## Errors

| Failure | Behavior |
|---|---|
| Spy set, HLE not requested | One stderr line; `enabled()==false` |
| Census `fopen` fails | One stderr line; counters keep running |
| Hook table full | Drop extra symbols; log each name once |
| `intern_name` OOM | `g_malloc` abort is existing QEMU policy; do not add a silent NULL name |
| Title fails core gate | Same reject as today; no spy bind |
| Incomplete XBE snapshot | Same retryable Armed path as today |

## Testing

Add `tests/d3d_hle_spy_census_contract.py` to `d3d_hle_contracts` in
`tests/meson.build`. Source assertions:

- `XEMU_D3D_HLE_SPY` and `XEMU_D3D_HLE_SPY_LOG` appear only as documented.
- `xemu_d3d_hle_select_profile` is skipped when `xemu_d3d_hle_spy_enabled()`.
- `discovery_mutating_uncovered_count` refuse block in
  `xemu_d3d_hle_resolve_loaded_xbe` is gated with spy-enabled skip; the
  `leaving title on NV2A` string remains for the non-spy branch.
- `XEMU_D3D_HLE_HOOK_OBSERVE` is in the profile header and assigned in
  discovery only under `xemu_d3d_hle_spy_enabled()`.
- `xemu_d3d_hle_exec` spy path calls `xemu_d3d_hle_spy_note` and
  `return false` before `xemu_d3d_hle_activate_host_device`.
- `xemu_d3d_hle_vblank` calls `xgpu_plume_f2_poll` when spy is enabled,
  including on the `!s_host_ready` path.
- `xemu_d3d_hle_session_reset` calls `xemu_d3d_hle_spy_dump` then
  `xemu_d3d_hle_spy_reset`.
- `xemu_d3d_hle_spy.c` is in `hw/xbox/meson.build`.
- `xemu_d3d_hle.h` / `xemu_d3d_hle_disabled.c` gain no new exports.

Existing `d3d_hle_automatic_refuse_split_contract` and
`d3d_hle_hook_policy_contract` must still pass. Extend the hook-policy
contract to include `XEMU_D3D_HLE_HOOK_OBSERVE` in the enum list.

Runtime proof is manual and not CI: FM1 with Plume requested and
`XEMU_D3D_HLE_SPY=1` still shows NV2A frames; F2 or a clean exit writes
`plume_d3d8_census.log` with a header and per-symbol rows. Called
`hole` rows are the FM1 shared-wrapper work list.

## Follow-up slices (not this spec)

1. Implement called mutating holes from an FM1 census in `bindings[]`
   and guest wrappers. No FM1 profile file.
2. Same census on PGR2 with spy (exact profile skipped) until automatic
   covers it; then drop the PGR2 override only if gameplay still holds.
3. Spy-while-Active: same `note`/`dump` on REPLACE hooks after Plume
   owns the GPU, still no unbound fall-through after attach.

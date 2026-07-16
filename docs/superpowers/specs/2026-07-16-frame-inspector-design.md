# Frame Inspector ("Inspect Element" for a Frame) — Design

**Date:** 2026-07-16
**Status:** Approved (pending spec review). Revised same day to incorporate
`2026-07-16-frame-inspector-design-review.md` (all findings accepted except
where noted in "Out of scope").
**Builds on:** the call-trace recorder (`xemu-calltrace.*`, TCG CALL hook in
`target/i386/tcg/`) and its Data-mode argument-set interning.

## Goal

A debugging break for rendering: arm via hotkey/menu, xemu captures the next
frame completely — every PGRAPH method word, per-batch GPU state, immutable
snapshots of the resources each draw consumed, the color-change history of
every tracked surface, and the guest call chain that wrote every command
word — then pauses. An ImGui overlay lets the user hover the frozen frame
like Chrome's "inspect element": the draw under the cursor highlights, and
selecting it shows the tree of how it came to be, from guest function
(IDA-ready addresses) down to NV097 methods and per-pixel color history. A
follow-up "watch" capture records deep data (registers, args, return value,
memory writes) for specific calls discovered in the first capture.

Primary workflows, equally weighted:
1. **RE attribution** — point at an on-screen element, get the guest call
   chain that drew it.
2. **Rendering debugging** — an element draws incorrectly; inspect the
   draw's full GPU state, method sequence, resources, and color history to
   find the bad state.

## Decisions (from brainstorming + design review)

| Question | Decision |
|---|---|
| Tree content | GPU state tree **and** guest code origin per draw |
| Inspector UI | In-emulator ImGui overlay; VM pauses after capture is finalized |
| Pixel attribution | Synchronous readback + diff per writer event → exact **color-change history** (see Guarantee model; not fragment coverage) |
| Guest linkage | Per-write: TCG store instrumentation tags written dwords with the live call-path node — explicitly best-effort with confidence states |
| Resources | Immutable draw-time snapshots (dedup by hash); a finalized capture never depends on live guest RAM or live GL objects |
| Storage | RAM only — no file format; capture lives until replaced/discarded |
| Backend | OpenGL renderer only in v1 (menu disabled otherwise) |
| Follow-up captures | Watched calls: per-invocation regs/args/return value + subtree write log |
| Rejected approaches | Deferred GPU-side snapshots (memory-heavy, same result); record+replay à la RenderDoc (huge lift, divergence risk); integer writer-ID sidecar (deferred, see Out of scope) |

## Guarantee model

The UI and docs expose distinct kinds of evidence; nothing is labeled
"exact" beyond what its mechanism can prove.

- **Color history (exact, within caps):** the stored color before and after
  every observed color-changing writer event on tracked surfaces, and exact
  reconstruction of any tracked surface generation at any writer event.
  Makes **no claim** about fragments that ran without changing stored color
  (same-color writes, color-masked draws, depth/stencil-only draws,
  rejected fragments, individual blended contributors).
- **Resource dependency (exact identity, not texel ancestry):** the
  textures, geometry, constants, shaders, and surfaces a draw consumed, as
  immutable snapshots, plus upstream producing events for render-target-
  backed resources. No claim about which source texel produced which output
  pixel.
- **Guest origin attribution (best-effort, never fabricated):** last CPU
  writer call path for command and resource dwords, with explicit states:
  `attributed`, `partial-dword`, `lost-sync`, `pre-arm/unattributed`.
  No guarantee for stores predating arming or bypassing instrumented paths.

## Architecture

| Component | Location | Role |
|---|---|---|
| Inspector core | `xemu-frameinspect.c/h` (repo root) | Capture state machine, in-RAM capture store, arm/disarm API, tag map, shadow call tree, budgets |
| Guest instrumentation | `target/i386/tcg/` | Reuses calltrace CALL hook; adds RET hook and store hook, gated on `xemu_frameinspect_armed` with the calltrace TB-flush pattern |
| PFIFO/PGRAPH taps | `hw/xbox/nv2a/` | Pusher logs method words pre-lookahead with source address + writer tag; pgraph groups them into guest batches with state snapshots |
| GL capture | `hw/xbox/nv2a/pgraph/gl/` | Per-writer-event surface readback + diff; resource snapshotting; scanout capture |
| Inspector UI | `ui/xui/frame-inspector.cc/hh` | ImGui overlay: hover/highlight, detail tabs, timelines, address lookup, watch management |

## Event vocabulary

Used consistently in the data model, UI, and this spec:

- **Method event** — one decoded NV097 method parameter word, logged in the
  PFIFO pusher *before* PGRAPH lookahead can squash BEGIN/DRAW/END patterns
  (`pgraph.c:783-803`); how PGRAPH grouped/consumed them is recorded too.
- **Guest batch** — one logical `SET_BEGIN_END` bracket ("a draw" in UI
  terms).
- **Host draw** — one actual GL draw command (a guest batch may span
  several, e.g. `glMultiDrawArrays`; several inline ranges may combine).
- **Writer event** — one operation after which a target surface's history
  is sampled: guest batch, clear, image blit, CPU upload, download, or
  scanout. Pixel histories reference writer events; the UI groups writer
  events under guest batches where applicable.

## Capture lifecycle

```
User arms (Ctrl+Alt+I or menu)
  → alloc tag map + budgets; enable CALL/RET/store instrumentation (TB flush)
  → lead-in interval: guest stores tagged with call-path nodes
first NV097_FLIP_STALL (pgraph.c:901-908) after instrumentation active
  → GPU frame capture ON
  → record method events (+origin), guest batches (+state snapshots,
    +resource snapshots), writer events (+surface deltas)
second NV097_FLIP_STALL
  → capture final scanout state + displayed image
  → finalize: build immutable capture object (indices, per-pixel CSR, blobs)
  → disarm instrumentation (TB flush)
  → publish capture to UI via acquire/release handoff
  → request VM pause asynchronously, outside PFIFO/PGRAPH critical sections
inspect (VM paused)
  → optionally mark watches
  → "Re-capture with watches": re-arm + resume → cycle repeats
```

- The lead-in interval **cannot** recover origin tags for command words or
  resources written before arming; those read `pre-arm/unattributed`.
- The pause is a user-experience feature, **not** the synchronization that
  makes capture data safe — the immutable publish is. Normal
  `RUN_STATE_PAUSED` does not halt PFIFO (`nv2a.c:383-411` halts it only for
  save/restore), so the UI only ever sees the finalized capture object and
  never live PGRAPH, renderer, texture-cache, or surface-list pointers.
- The capture is RAM-resident, remains valid and browsable after resume
  (it depends on nothing live), and persists until the next capture,
  explicit discard, or exit.

## Data captured

### Shadow call tree (vCPU thread)

- **Path nodes** interned by `(parent_id, call_site, callee)`; **argument
  sets** (6 dwords at call time) interned separately per node with a
  per-node cap, reusing calltrace Data mode's argset pattern — varying
  arguments never duplicate path structure. Vtable `this` pointers are
  visible via arg dword 0/ECX conventions.
- Maintained by CALL/RET instrumentation while armed. On RET to target `T`,
  pop frames until the pushed expected-return address matches `T`, bounded
  (max 64 pops); on failure, reset to an explicit **unknown root** and mark
  the affected span `lost-sync`.
- **Guest-thread discontinuities:** shadow stacks are keyed by the current
  guest thread — the kernel's current-KTHREAD pointer read via the KPCR
  (fixed Xbox kernel ABI, title-independent), with ESP-discontinuity
  detection at CALL/RET as a backstop. When continuity cannot be
  established, attach under the unknown root — attribution degrades to
  missing, never fabricated. Hardware interrupt entry is not a CALL; ISR
  bodies attribute to the interrupted context (documented noise) unless the
  thread key changes, which resets to unknown root.
- Tail calls (`jmp func`) extend the current node's attribution (accepted
  imprecision).
- Hard caps: node count and total bytes (see Memory budget); node/argset
  truncation surfaced independently.

### Tag map

One `uint32_t` node-id per dword of guest RAM (64 MB retail / 128 MB debug
→ 64–128 MB shadow), allocated at arm. Covers **all** RAM, so writer lookup
works for pushbuffers, vertex buffers, textures, and intermediate buffers
alike. Semantics:

- A tag is published **only after the guest store succeeds** — the hook
  runs post-store (or performs the access itself), so faulting stores never
  leave phantom tags.
- Every affected byte range is virtual→physical translated and
  range-checked (page crossings included) before indexing the map; only
  RAM-backed bytes are tagged.
- A store that partially covers a dword tags it with a **partial-dword**
  flag bit so the UI can distinguish full from partial attribution.
- vCPU tag writes → PFIFO tag reads use release/acquire ordering.
- Tag 0 = `pre-arm/unattributed`.

### Store instrumentation scope

Hook the common store codegen paths: integer mov-to-memory, push, rep
movs/stos, and common SSE stores. Uncovered exotic stores (atomics,
masked/conditional vector stores, etc.) degrade to unattributed — missing,
never wrong. Expected cost while armed: a few × slowdown for ~2 frames
(to be measured during implementation).

### Method events and command origin (PFIFO)

Each consumed word is logged in the pusher before lookahead:
`(method, subchannel, param, DMA object, GET offset, guest physical
address, writer tag, attribution confidence)`. The pusher computes the
physical address from the DMA object base + get offset and resolves the
writer tag at consumption time.

### Guest batches and state (PGRAPH)

Per guest batch (plus clears/blits/uploads as their own writer events):

- Full pgraph register snapshot (~8–32 KB), decoded lazily by the UI.
- Vertex shader program + constants at batch time, plus effective
  non-register state needed to reproduce decoding.
- References to immutable resource snapshots (below) for everything the
  batch consumed.
- Target surface-generation ID.

### Resource snapshots (immutable, deduplicated)

Every resource actually consumed by a guest batch is snapshotted at batch
time and referenced by ID:

- Texture and palette bytes (raw guest data + interpretation metadata).
  Dedup by content hash + metadata; `texture.c` already computes data
  hashes for dirty textures, so much of this cost is already paid.
- **Render-target-backed textures** (`texture.c:291-385` `surf_to_tex`
  path) never have valid guest RAM contents; their image content is
  reconstructed from the producing surface generation's color history
  instead, and the dependency edge to the producing writer events is
  recorded.
- Vertex and index ranges referenced by the batch (computed from attribute
  pointers + index extents), and inline geometry (already present in the
  method stream, referenced not duplicated).
- Shader programs/constants (in the state snapshot).

A finalized capture never reads live guest RAM or live GL objects.

### Surface generations

- Each effective render-surface binding gets a **capture-local generation
  ID** with full identity: address, byte extent, format, pitch, dimensions,
  swizzle, color/zeta role, anti-aliasing, internal scale factor, and
  generation counter (`SurfaceBinding` identity per `renderer.h:41-67` —
  a VRAM address alone is not identity).
- A shape/format/pitch change at the same address starts a **new
  generation**. Overlapping/aliasing generations are recorded as explicit
  relationships, never silently merged.
- **Initial snapshot:** every generation gets a baseline image captured
  before its first writer event in the capture (read at first encounter,
  before the writer executes). A newly created surface with undefined
  contents is recorded as such.

### Color history (GL)

- Around each writer event targeting a tracked surface: establish the
  **pre-writer baseline** (the previous post-writer image of that
  generation, or a fresh read if the renderer uploaded dirty RAM or
  otherwise touched the target since — uploads are themselves writer
  events), then read back post-writer and diff **pre vs post for that
  event**.
- Deltas are stored as run-length/tile-encoded spans per writer event
  (draw index + before/after colors), **not** per-pixel heap allocations.
  At finalize, a compact per-pixel index (CSR-style) is built for hover
  lookup. Periodic keyframes support O(1) seek; exact reconstruction =
  nearest keyframe + deltas.
- **Blits** (`blit.c:104-222`) are CPU-side VRAM operations that need not
  target the bound GL surface: the blit writer event records source and
  destination address ranges, formats, pitches, and rectangles, and history
  is updated for the **destination surface generation** (diffing the
  destination representation after any required upload), never implicitly
  for the current GL binding.
- Capture operates at surface resolution (including internal scale/AA);
  stored colors are exact stored values, never resampled. Higher internal
  scale multiplies readback and delta cost — budgets cap it and the UI
  reports what was dropped.

### Scanout (final display)

Every capture ends with an explicit **scanout event** recording: the
selected source surface generation, PCRTC start/line offset, interlace,
scaling/filter state, PVIDEO overlay state, the final window transform, and
an independent snapshot of the final displayed image. Hover coordinates map
through this recorded transform. Regions composed by PVIDEO or the software
VGA fallback are marked **unsupported for attribution** in v1 — hovering
them says so rather than misattributing.

## Inspector UI (ImGui overlay)

Opens when the finalized capture is published (VM pause requested
alongside). Menu entries live beside the calltrace items; hotkey Ctrl+Alt+I
arms/re-arms.

- **Hover = inspect element.** The captured displayed image fills the view;
  coordinates map through the recorded scanout transform. Hovering
  highlights the color-change mask of the topmost writer event under the
  cursor + tooltip (batch #, primitive type, innermost guest function,
  attribution confidence). Scroll wheel cycles stacked writer events at
  that pixel in reverse order. PVIDEO/unsupported regions say so.
- **Click selects** a guest batch (or blit/clear/upload event) → tabs:
  - **Origin** — call chain for the batch's trigger write: nodes show
    `call_site → callee` + interned arg sets, hex addresses with copy
    buttons, per-node attribution state. Per-method origins available.
  - **Methods** — the batch's method events in order, names via
    `methods.h.inc`, params, filter box; row hover shows that word's
    writer chain + confidence; shows how PGRAPH grouped words into host
    draws.
  - **State** — decoded key state (surface/format, blend, depth/alpha,
    combiners, texture units) with changed-since-previous-batch
    highlighting, plus raw register dump.
  - **Resources** — the batch's immutable snapshots: textures/palettes
    rendered inline, vertex/index ranges in a hex/structured view;
    RT-backed textures show their reconstructed image and link to
    producing writer events (a **resource dependency** hop — not texel
    ancestry; the user can open the producing surface's own timeline and
    hover there).
  - **Pixels** — color history at the pinned pixel:
    `(writer event, before → after color)` per change, clears/blits/uploads
    included.
- **Timelines** — a **global event timeline** (all writer events, all
  surfaces) and a **per-surface timeline**. Selecting a writer event shows
  its *target surface* reconstructed at that event; final-frame
  reconstruction is labeled as such and only offered on the scanout
  source's timeline.
- **Address lookup panel** — paste a guest address: writer chain for those
  bytes + attribution state + small hex view. The manual hop for staged
  data (parse → intermediate buffer → memcpy → pushbuffer).
- **Watch panel** — list/add/remove watches, browse invocation records,
  "Re-capture with watches" button.
- **Resume** closes the overlay and unpauses; the capture remains valid and
  reopenable (it depends on nothing live).

## Watched-call instrument

- Any call-tree node: right-click → "Watch this call" (or add by address in
  the watch panel). Watches persist for the session.
- During a watched capture, each invocation of a watched callee records:
  - Full GPR set + ESP at entry, 16 stack dwords, invocation order, and
    position in the shadow call tree.
  - Return value (EAX at the matching RET). An invocation with no matching
    RET (exception, non-local unwind, context switch, capture end) is
    marked **incomplete** — no synthesized return value or completion.
  - **Subtree write log:** while control is inside the watched call or its
    callees on the same guest thread, the store hook appends write records
    — only after the write succeeds. REP string operations log as one
    aggregate range record (address range + old/new byte buffers). RAM
    writes only; MMIO/device writes are counted but not value-logged in v1.
- Caps: 1024 invocations per watch; per-watch byte and event caps plus the
  global write-log cap (1 M entries / ~20 MB); truncation surfaced
  per-watch.

## Memory budget

Hard byte caps at every level; a **global capture budget** (default 1 GiB,
configurable) is enforced across all variable stores. The fixed-size tag
map is sized by guest RAM and accounted separately from the budget. Hitting any cap completes the
capture with a per-store truncation report (color history, keyframes,
resources, shadow tree, watch logs each reported separately) — never a
silent drop, never a wholesale abort.

| Store | Typical (500-batch frame, 1× scale) | Cap |
|---|---|---|
| Tag map | 64–128 MB | fixed by RAM size |
| Color deltas (RLE) + CSR index | ~20–80 MB | 256 MB |
| Keyframes + initial/final snapshots | ~40–60 MB | 128 MB |
| Resource snapshots (deduped) | ~30–80 MB | 256 MB |
| Register/state snapshots | ~16 MB | 64 MB |
| Method events | ~2–4 MB | 32 MB |
| Shadow call tree + argsets | ~4–16 MB | 64 MB |
| Watch write log | ~20 MB | 32 MB |

Typical-frame numbers are pre-measurement estimates; representative frames
will be measured early in implementation (a plan milestone) before these
defaults are finalized. High internal scale factors multiply readback
volume (~scale²) and will hit caps sooner; this is reported, not hidden.

## Error handling

- Non-GL renderer active → menu item disabled, tooltip "requires OpenGL
  renderer".
- Readback failure / unsupported surface format → affected writer events
  marked "no pixel data"; methods, state, resources, and origin still
  captured. A capture never aborts wholesale.
- Shadow-stack desync / thread discontinuity → explicit `lost-sync` or
  unknown-root attribution; never fabricated ancestry.
- Caps exceeded → per-store truncation report in a visible banner.
- ISR attribution noise, pre-arm unattributed writes, partial-dword tags,
  and PVIDEO regions are surfaced as explicit states in the UI, not hidden.

## Threading

- vCPU thread: shadow stacks, tag map writes (release), watch records.
- NV2A side (PFIFO pusher/puller and GL context, per the existing renderer
  architecture): method/batch/writer-event recording, readbacks, diffs,
  resource snapshots. Single writer into the capture-in-progress.
- Finalization builds an immutable capture object; publication to the UI is
  an acquire/release handoff. The UI thread only ever reads finalized
  captures. VM pause is requested asynchronously after publication, outside
  PFIFO/PGRAPH critical sections, and is UX only.

## Testing

Standalone unit tests following the calltrace pattern
(`tests/frameinspect/`, QEMU-independent headers where possible):

- Delta encode/decode + keyframe reconstruction: baseline + deltas
  reproduces the exact image at any writer event.
- Pixel diff: synthetic before/after buffers → expected delta spans;
  same-color write produces no change and **no coverage claim**.
- Initial-snapshot-then-first-writer reconstruction; undefined-content
  generations.
- Surface generations: rebind at same address with different format/pitch;
  overlapping generations recorded as aliases, not merged.
- Blit to a non-current destination surface updates the right generation.
- CPU upload immediately before a draw attributes to the upload event, not
  the draw.
- Shadow stack: push/pop, bounded-pop desync, lost-sync marking,
  ESP/thread-key discontinuity → unknown root (context switch, interrupt,
  non-local unwind cases).
- Tag map: post-store-only publication (faulting store leaves no tag),
  partial and cross-page store tagging, tag-0 semantics, overlap handling.
- Watch log: post-write append, REP aggregate records, incomplete
  invocations (no RET), per-watch caps.
- Path-node vs argset interning: varying args don't duplicate paths; caps.
- Budget enforcement: per-store and global caps produce correct truncation
  reports.

Manual integration checklist (in-repo, part of the feature docs): capture a
real game menu on the GL renderer; verify hover attribution, color history,
and confidence states; RT-backed texture shows reconstructed content;
capture remains valid after resume and after guest RAM changes; watch a
call and verify regs/args/return/write log; repeat with internal scaling
enabled and confirm budget reporting; verify PFIFO method lookahead cases
(squashed BEGIN/DRAW/END) still show correct per-word origins.

## Out of scope (v1)

- Vulkan/null renderer support.
- Saving captures to disk / a capture file format.
- Integer writer-ID / coverage attachment (last-visible-fragment
  attribution); full fragment history and rejected-fragment visibility
  (requires replay or shader instrumentation).
- PVIDEO overlay and software-VGA **attribution** (their presence is
  recorded and marked unsupported; scanout state is captured).
- Exact sampled-texel ancestry across render-to-texture hops (dependency
  edges + manual inspection only).
- Data-flow (taint) tracking — staged-copy chases are manual via the
  address lookup panel.
- Symbol names in-UI (addresses + copy buttons only).
- Record+replay, state editing, shader debugging.

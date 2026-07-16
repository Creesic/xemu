# Frame Inspector ("Inspect Element" for a Frame) — Design

**Date:** 2026-07-16
**Status:** Approved (pending spec review)
**Builds on:** the call-trace recorder (`xemu-calltrace.*`, TCG CALL hook in
`target/i386/tcg/`) and its Data-mode 6-dword argument capture.

## Goal

A debugging break for rendering: arm via hotkey/menu, xemu captures the next
frame completely — every PGRAPH method, per-draw GPU state, the exact pixels
each draw touched, and the guest call chain that wrote every command word —
then pauses. An ImGui overlay lets the user hover the frozen frame like
Chrome's "inspect element": the draw under the cursor highlights, and
selecting it shows the full tree of how it came to be, from guest function
(IDA-ready addresses) down to NV097 methods and pixel history. A follow-up
"watch" capture records deep data (registers, args, return value, every
memory write) for specific calls discovered in the first capture.

Primary workflows, equally weighted:
1. **RE attribution** — point at an on-screen element, get the guest
   call chain that drew it.
2. **Rendering debugging** — an element draws incorrectly; inspect the
   draw's full GPU state, method sequence, and pixel history to find the
   bad state.

## Decisions (from brainstorming)

| Question | Decision |
|---|---|
| Tree content | GPU state tree **and** guest code origin per draw |
| Inspector UI | In-emulator ImGui overlay; VM pauses on capture |
| Hover precision | Pixel-exact: readback + diff after every draw (Approach 1: synchronous capture) |
| Guest linkage | Per-write: TCG store instrumentation tags every written dword with the live call-path node |
| Storage | RAM only — no file format; capture lives until replaced/discarded |
| Backend | OpenGL renderer only in v1 (menu disabled otherwise) |
| Follow-up captures | Watched calls: per-invocation regs/args/return value + subtree write log |
| Rejected approaches | Deferred GPU-side snapshots (memory-heavy, same result); record+replay à la RenderDoc (huge lift, divergence risk) |

## Architecture

| Component | Location | Role |
|---|---|---|
| Inspector core | `xemu-frameinspect.c/h` (repo root) | Capture state machine, in-RAM capture store, arm/disarm API, shadow tag map, shadow call tree |
| Guest instrumentation | `target/i386/tcg/` | Reuses calltrace CALL hook; adds RET hook and store hook, gated on `xemu_frameinspect_armed` with the calltrace TB-flush pattern |
| PFIFO/PGRAPH taps | `hw/xbox/nv2a/` | Pusher records each consumed word + source guest address + writer tag; pgraph dispatch groups methods into draw records with state snapshots |
| GL readback | `hw/xbox/nv2a/pgraph/gl/` | After each draw/clear/blit during capture: read color target, diff, build touch lists + keyframes |
| Inspector UI | `ui/xui/frame-inspector.cc/hh` | ImGui overlay: hover/highlight, detail tabs, timeline, address lookup, watch management |

## Capture lifecycle

```
Hotkey (Ctrl+Alt+I) or menu
  → ARM: alloc tag map, enable CALL/RET/store instrumentation (TB flush)
frame N      guest stores tagged with call-path nodes (lead-in frame)
flip         GPU capture ON
frame N+1    methods recorded (+source addr +writer tag); per-draw state
             snapshot; readback+diff after each draw/clear/blit
flip         capture complete: disarm (TB flush), PAUSE VM, open overlay
inspect      optionally mark watches
  → "Re-capture with watches": re-arm + resume → cycle repeats
```

The capture is RAM-resident only. It persists (browsable, reopenable) until
the next capture, explicit discard, or exit. Discard frees everything
including the tag map.

## Data captured

### Shadow call tree (CPU thread)

- Interned nodes `(parent_id, call_site, callee, args[6])`; the 6-dword
  argument snapshot reuses calltrace Data mode's capture mechanism, so
  vtable `this` pointers are visible on indirect calls.
- Maintained by CALL/RET instrumentation while armed. On RET to target `T`,
  pop frames until the pushed expected-return address matches `T`, bounded
  (max 64 pops); on failure, reset to root and mark subsequent nodes
  "lost sync" — attribution degrades to partial chains, never fabricated.
- Tail calls (`jmp func`) extend the current node's attribution (accepted
  imprecision). Hardware interrupt entry is not a CALL, so ISR bodies
  attribute to the interrupted context (documented noise).
- The stack starts empty at arm time, so chains root at the game's frame
  loop, not at boot-time ancestry.

### Tag map

One `uint32_t` node-id per dword of guest RAM (64 MB retail / 128 MB debug
→ 64–128 MB shadow), allocated at arm. The store hook writes
`tag[phys >> 2] = current_node`; a store tags every dword it overlaps.
Covers **all** RAM, so writer lookup works for pushbuffers, vertex buffers,
textures, and intermediate buffers alike. Tag 0 = "unattributed (written
before arming or by an uninstrumented store)".

### Store instrumentation scope

Hook the common store codegen paths: integer mov-to-memory, push, rep
movs/stos, and common SSE stores. Uncovered exotic stores degrade to
unattributed — missing, never wrong. Expected cost while armed: a few ×
slowdown for ~2 frames. Only stores to RAM (phys < ram size) are tagged.

### Per-draw records (PGRAPH thread)

For every draw (`SET_BEGIN_END` bracket) plus clears and blits as
pseudo-draws:

- Full pgraph register snapshot (~8–32 KB), decoded lazily by the UI — no
  cherry-picked field list to get wrong.
- Vertex shader program + constants at draw time.
- The ordered method sequence since the previous draw:
  `(method, subchannel, param, source guest addr, writer node)` per word.
  The pusher computes each word's guest physical address from the DMA
  object base + get offset and resolves the writer tag at consumption time.
- Render target identity (surface guest address, format, dimensions,
  scale factor).

### Pixel attribution (GL thread)

- After each draw/clear/blit, `glReadPixels` the current color target and
  CPU-diff against the previous copy of that surface.
- Each changed pixel appends `(draw_idx, before_color, after_color)` to its
  per-pixel touch list. Periodic keyframes (every ~16 draws) plus touches
  give exact reconstruction of the frame at any draw index (timeline
  scrubbing) and exact per-pixel history.
- Touch structures and keyframes are **per render surface** (keyed by
  surface address), so render-to-texture chains attribute correctly; the
  memory budget assumes the primary surface dominates keyframe count. Hover resolves against
  the surface displayed at flip; textures that are rendered surfaces link
  back to their producing draws (one hop per click).
- Readback occurs at surface resolution (respecting surface scale/AA);
  the UI maps window coordinates through the display viewport transform.

## Inspector UI (ImGui overlay)

Opens automatically when capture completes (VM paused). Menu entries live
beside the calltrace items; hotkey Ctrl+Alt+I arms/re-arms.

- **Hover = inspect element.** Captured final frame fills the view.
  Hovering highlights the exact pixel mask of the topmost draw under the
  cursor + tooltip (draw #, primitive type, innermost guest function).
  Scroll wheel cycles through stacked draws at that pixel in reverse draw
  order.
- **Click selects** a draw → detail panel tabs:
  - **Origin** — call chain for the draw's trigger write, each node
    showing `call_site → callee` + 6 arg dwords, hex addresses with copy
    buttons. Per-method origins available (every word has its own tag).
  - **Methods** — ordered NV097 method list, names via `methods.h.inc`,
    params, filter box; row hover shows that word's writer chain.
  - **State** — decoded key state (surface/format, blend, depth/alpha,
    combiners, texture units) with changed-since-previous-draw
    highlighting, plus raw register dump.
  - **Textures** — bound textures decoded from paused guest RAM (reusing
    swizzle/s3tc paths) shown inline; RTT textures link to producing
    draws.
  - **Pixels** — full history at the pinned pixel:
    `(draw, before → after color)` per touch, clears included.
- **Timeline scrubber** over all draws — exact frame reconstruction at any
  index; clicking selects the draw at that point.
- **Address lookup panel** — paste a guest address: writer chain for those
  bytes + small hex view. This is the manual hop for chasing staged data
  (parse → intermediate buffer → memcpy → pushbuffer).
- **Watch panel** — list/add/remove watches, browse invocation records,
  "Re-capture with watches" button.
- **Resume** closes the overlay and unpauses; capture remains reopenable.

## Watched-call instrument

- Any call-tree node: right-click → "Watch this call" (or add by address in
  the watch panel). Watches persist for the session.
- During a watched capture, each invocation of a watched callee records:
  - Full GPR set + ESP at entry, 16 stack dwords, invocation order, and
    position in the shadow call tree.
  - Return value (EAX at the matching RET).
  - **Subtree write log:** while control is inside the watched call or its
    callees, the store hook additionally appends
    `(addr, old value, new value, size)`.
- Caps: 1024 invocations per watch, 1 M write-log entries total;
  truncation banner when hit.

## Memory budget (typical 500-draw frame)

| Store | Size |
|---|---|
| Tag map | 64–128 MB |
| Touch lists | ~15–50 MB |
| Keyframes (~32 @ 640×480 RGBA) | ~40 MB |
| Register snapshots | ~16 MB |
| Method records | ~2 MB |
| Watch write log (1 M-entry cap, ~20 B/entry) | ≤ ~20 MB |
| **Total typical** | **150–250 MB** |

Hard caps on every growable store; hitting one completes the capture with a
visible "truncated" banner stating what was dropped.

## Error handling

- Non-GL renderer active → menu item disabled, tooltip "requires OpenGL
  renderer".
- Readback failure / unsupported surface format → draw marked
  "no pixel data"; methods, state, and origin still captured. A capture
  never aborts wholesale.
- Shadow-stack desync → explicit "lost sync" marker on affected chains.
- Caps exceeded → truncation banner, capture otherwise usable.
- ISR attribution noise and pre-arm unattributed writes are documented
  limitations surfaced as "unattributed" in the UI, not hidden.

## Threading

- CPU (vCPU) thread: shadow stack, tag map writes, watch records.
- NV2A side (PFIFO pusher/puller and GL context, whichever threads those
  run on in the current renderer): method + draw records, readbacks,
  diffs. The capture store has a single writer at a time on this side;
  exact thread placement follows the existing renderer architecture and
  is settled in the implementation plan.
- Tag map reads (pusher) and writes (CPU) use relaxed atomics; exactness is
  best-effort during the armed window and stable once paused.
- UI reads the capture store only while the VM is paused.

## Testing

Standalone unit tests following the calltrace pattern
(`tests/frameinspect/`, QEMU-independent headers where possible):

- Touch-list append + keyframe reconstruction: keyframe + touches
  reproduces the exact image at any draw index.
- Pixel diff: synthetic before/after buffers → expected touch lists.
- Shadow stack: push/pop, bounded-pop desync recovery, lost-sync marking.
- Tag map: tagging, overlap handling, lookup, tag-0 semantics.
- Watch write log: append, old/new capture, caps.

Manual integration checklist (in-repo, part of the feature docs): capture a
real game menu on the GL renderer, verify hover attribution and pixel
history, watch a call, verify regs/args/return/write log — with and without
surface scaling.

## Out of scope (v1)

- Vulkan/null renderer support.
- Saving captures to disk / a capture file format.
- Data-flow (taint) tracking — staged-copy chases are manual via the
  address lookup panel.
- Symbol names in-UI (addresses + copy buttons only; symbols remain the
  IDA/viewer workflow).
- Record+replay, state editing, shader debugging.

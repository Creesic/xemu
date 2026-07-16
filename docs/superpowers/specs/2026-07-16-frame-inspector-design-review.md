# Frame Inspector Design Review

**Date:** 2026-07-16
**Reviewed document:** `2026-07-16-frame-inspector-design.md`
**Status:** Review findings for incorporation into the design

## Summary

The proposed frame inspector has a strong overall product shape: capture one
frame, pause, inspect it in-process, and allow a deeper watched-call recapture.
OpenGL-only support and RAM-resident captures are reasonable v1 constraints.

Several guarantees in the current design are stronger than the proposed
instrumentation can prove. In particular, color-buffer diffs do not reveal
every fragment that touched a pixel, paused guest RAM does not necessarily
contain draw-time resource data, and a render-target address does not uniquely
identify a surface. The design should distinguish exact color-change history
from fragment coverage and should capture immutable resource and scanout state.

## Findings

### 1. Color diffs do not identify every pixel a draw touched

**Severity:** Critical

The claims in the design at lines 11-18, 33, and 116-123 are stronger than the
proposed `glReadPixels` diff mechanism. Comparing color buffers detects only
pixels whose stored color value changed. It does not identify:

- A fragment that writes the same color already present in the target.
- A depth-only or stencil-only draw.
- A color-masked draw.
- A fragment rejected by clipping, discard, alpha, depth, or stencil tests.
- Every earlier contributor to a blended result.

Therefore, this mechanism can provide exact **color-change history**, including
the before and after values of changed pixels. It cannot provide RenderDoc-style
fragment history or exact coverage for every attempted draw.

Recommended changes:

- Rename the guarantee from "pixels touched" to "pixels whose stored color
  changed" wherever synchronous readback and diff are the source of truth.
- Add an optional integer writer-ID or coverage attachment if last successful
  fragment attribution is required.
- State that a writer-ID attachment still does not report rejected fragments or
  every earlier blended contributor.
- Keep full failed-fragment history out of scope unless replay or deeper shader
  instrumentation is introduced.

### 2. Draw-time resources cannot be reconstructed from paused guest RAM

**Severity:** Critical

The Textures panel at lines 151-153 reads textures from paused guest RAM, while
lines 63-66 and 163 say the capture remains reopenable after the VM resumes.
Those requirements conflict: guest memory can change after the captured draw
and after resume.

Guest RAM can also be stale during capture. OpenGL render targets may remain
dirty on the GPU and can be consumed directly as textures. The existing path in
`hw/xbox/nv2a/pgraph/gl/texture.c:255-311` detects render-target-backed textures,
and `texture.c:378-384` converts a render surface directly into a texture. The
corresponding pixels need not have been written back to guest RAM.

The design also does not preserve vertex or index data as it existed at draw
time. A register snapshot and method sequence identify addresses and formats,
but not immutable geometry or texture contents.

Recommended changes:

- Snapshot every resource actually consumed by a draw: texture and palette
  data, vertex and index ranges, inline geometry, shader constants, and
  render-target-backed texture images.
- Deduplicate snapshots by content hash plus interpretation metadata.
- Record references from each draw to immutable resource snapshot IDs.
- Never have a reopened capture depend on current guest RAM or live GL objects.
- If immutable resource capture is deferred, make the inspector valid only
  while paused and remove the promise that it remains fully browsable after
  resume.

### 3. A VRAM address is not a unique surface identity

**Severity:** Critical

Lines 124-128 key touch structures and keyframes by surface address. Existing
`SurfaceBinding` state includes more than its address:
`hw/xbox/nv2a/pgraph/gl/renderer.h:41-67` contains shape, format, pitch,
dimensions, swizzle state, color/zeta role, size, and timestamps.

The same address can be rebound with a different format, pitch, dimensions, or
anti-aliasing configuration. Different bindings can also overlap in VRAM.
Combining those generations under one address would corrupt image history and
resource dependency links.

Recommended changes:

- Give each effective surface binding a generated capture-local ID.
- Include at least address, byte extent, format, pitch, dimensions, swizzle,
  color/zeta role, anti-aliasing, internal scale, and generation in its identity.
- Record explicit alias or overlap relationships instead of silently merging
  bindings that share an address.

### 4. The render target at flip is not necessarily the displayed image

**Severity:** Critical

Lines 126-130 resolve hover against the surface displayed at flip, but xemu has
separate guest flip, scanout, and host presentation paths. The displayed image
may additionally depend on PCRTC start and line offset, interlace, PVIDEO
composition, internal scaling, filtering, gamma, and the software VGA fallback.

PVIDEO is not represented as a normal PGRAPH draw. A user hovering an overlay
pixel would therefore receive no correct draw attribution from the proposed
surface-only history.

Recommended changes:

- Add a final scanout event to every capture.
- Record the selected source surface, PCRTC state, line offset, interlace and
  scaling state, PVIDEO state, and final coordinate transform.
- Represent PVIDEO composition and software VGA fallback as explicit writer or
  composition nodes, or mark them clearly as unsupported in v1.
- Preserve a final displayed-image snapshot independently of any render-target
  snapshot.

### 5. CPU store attribution is best-effort, not "missing, never wrong"

**Severity:** High

Lines 94-99 propose instruction-specific TCG hooks for common stores. Those
hooks must translate guest virtual addresses to physical RAM and must update the
tag only after a successful store. If a tag helper runs before a store that
faults, the tag map reports a write that never happened.

Specialized helpers, atomics, conditional stores, masked vector stores, page
crossings, and exceptional paths can also complicate attribution. Dword-level
tagging of a partial-byte store attributes the entire dword to the latest
partial writer, which can be misleading for untouched bytes.

Commands already queued or stored before arming remain unattributed. Commands
copied from reusable or static pushbuffer templates may point to the copier or
remain tag zero rather than identify the semantic creator.

Recommended changes:

- Publish a tag only after the corresponding guest store succeeds.
- Translate and range-check every affected byte range before indexing the tag
  map.
- Use release/acquire synchronization between vCPU tag writes and PFIFO tag
  reads rather than relying on relaxed ordering for an exactness claim.
- Distinguish full-dword attribution from partial-dword attribution.
- Describe all CPU origin data as best-effort and surface its confidence or
  attribution status in the UI.
- State that the lead-in frame cannot recover origins for command words written
  before arming.

### 6. The memory and transfer budget is optimistic

**Severity:** High

The estimate at lines 179-189 assumes 15-50 MB for touch lists and about 40 MB
for keyframes. A touch entry containing draw index, before color, and after
color needs roughly 12 bytes before container and allocation overhead.

At 640x480, if every pixel changes in each of 500 draws, raw touch entries alone
require approximately 1.7 GiB. Per-pixel vector or list headers add substantial
fixed and allocator overhead. Keyframes are per surface, so multiple render
targets multiply the estimate.

At higher internal resolution, synchronous transfer cost also grows quickly.
Reading a 4x-scaled 640x480 target after 500 draws transfers roughly 9 GiB before
CPU diffing. Each `glReadPixels` additionally introduces a GPU synchronization
point.

Recommended changes:

- Define a strict total byte budget for the complete capture.
- Add per-surface and per-store byte caps, not only entry-count caps.
- Store tile-based or run-length-encoded deltas rather than independent heap
  allocations per pixel.
- Define whether capture operates at guest-native, anti-aliased, internally
  scaled, or final displayed resolution.
- Measure representative frames before committing to the 150-250 MB typical
  estimate or the expected slowdown.
- Report separately when pixel history, keyframes, resource data, or CPU origin
  data is truncated.

### 7. Every surface generation needs an initial snapshot

**Severity:** High

Lines 118-123 specify readback after each writer. That does not establish the
before-state for the first captured writer to a surface. Without an initial
image, the first diff and its before colors cannot be reconstructed.

Recommended changes:

- Capture a pre-first-write image for every surface generation.
- Then record post-writer deltas and optional seek keyframes.
- Treat a shape or format change as a new surface generation with a new initial
  snapshot.
- Define how a newly allocated or undefined surface is represented when no
  meaningful initial contents exist.

### 8. Image blits need a different capture path from GL draws

**Severity:** High

The existing GL image blit path performs a CPU-side VRAM operation. It resolves
explicit source and destination DMA mappings in
`hw/xbox/nv2a/pgraph/gl/blit.c:104-166`, modifies destination RAM at
`blit.c:171-216`, and marks the destination dirty at `blit.c:218-222`.
The blit destination need not be the currently bound GL color target.

Reading the current color target after every blit can therefore inspect the
wrong resource or inspect a stale GL copy that has not uploaded the CPU result.

Recommended changes:

- Record explicit source and destination address ranges, formats, pitches, and
  rectangles for every blit.
- Update history for the destination surface generation, not implicitly for
  the current GL binding.
- Diff the destination RAM representation directly when valid, or force and
  read the correct destination binding after upload.
- Keep draw, clear, upload, download, and blit writer kinds distinct in the
  event model.

### 9. "Draw" needs a precise definition

**Severity:** Medium

xemu can combine multiple guest ranges into one `glMultiDrawArrays` call in
`hw/xbox/nv2a/pgraph/gl/draw.c:391-405`. PGRAPH lookahead can also consume and
squash repeated BEGIN/DRAW/END patterns in
`hw/xbox/nv2a/pgraph/pgraph.c:783-803`.

The design uses "draw" for several different boundaries. This makes method
grouping, pixel histories, and timeline counts ambiguous.

Recommended changes:

- Define a guest method event as one decoded method parameter word.
- Define a guest batch as one logical `SET_BEGIN_END` bracket.
- Define a host draw as one actual GL draw command.
- Define a writer event as one operation after which target history is sampled.
- Reference writer events from pixel histories and group them under guest
  batches for the UI.
- Log method words in PFIFO before PGRAPH lookahead can combine them, while also
  recording how PGRAPH grouped and consumed them.

### 10. Pausing the VM does not itself halt PFIFO

**Severity:** Medium

The threading section at lines 206-216 relies partly on the VM being paused.
Normal `RUN_STATE_PAUSED` does not set `pfifo.halt`. The NV2A state-change path
only explicitly halts PFIFO for save and restore operations in
`hw/xbox/nv2a/nv2a.c:380-410`.

Recommended changes:

- Finalize all vectors, blobs, images, and indices before publishing a capture
  to the UI.
- Publish an immutable capture object through an explicit ownership or
  acquire/release protocol.
- Do not expose live PGRAPH, renderer, texture-cache, or surface-list pointers.
- Request VM pause asynchronously after capture finalization rather than
  calling a blocking stop operation while holding PFIFO/PGRAPH locks.
- Treat the pause as a user-experience feature, not the synchronization
  mechanism that makes capture data safe.

### 11. Watched-call write logging needs defined exceptional semantics

**Severity:** Medium

Lines 165-177 promise old and new values for every write in a watched subtree.
The design needs to define behavior for REP operations, atomics, failed
conditional stores, masked stores, page-crossing accesses, MMIO, faults, and
interrupts.

Matching invocation completion only at RET does not cover exceptions, tail
calls, non-local unwinds, or context switches. An interrupt occurring while a
watch is active may also have its writes incorrectly included in the watched
subtree under the currently proposed ISR attribution behavior.

Recommended changes:

- Append a write record only after a successful write.
- Define whether REP writes are one aggregate event or one event per iteration.
- Distinguish RAM, VRAM alias, and MMIO writes.
- Mark watched invocations as incomplete when no matching RET is observed.
- Record desynchronization and interruption explicitly instead of synthesizing
  a return value or normal completion.
- Add byte and event caps per watch in addition to the global cap.

### 12. The shadow call tree needs guest-thread discontinuity handling

**Severity:** High

The design maintains one shadow stack on the vCPU thread. The guest can switch
among multiple software threads on the same emulated CPU without executing a
normal CALL/RET pair visible as a balanced transition in the current stack.
Exceptions and interrupt stack changes create the same class of discontinuity.

Until a mismatching RET is observed, calls in the new context can be attached
under a stale parent from the previous guest context. That creates fabricated
ancestry, contrary to the stated rule that attribution should degrade to
missing rather than wrong.

Recommended changes:

- Detect stack-pointer or execution-context discontinuities at CALL and RET
  instrumentation points.
- Reset to an explicit unknown root before attaching a call when continuity
  cannot be established.
- Consider separate shadow stacks keyed by a reliable guest thread identity if
  one can be obtained without title-specific assumptions.
- Add context-switch, interrupt, exception, and non-local-unwind cases to the
  shadow-stack tests.

### 13. Shadow call-tree storage is absent from the memory budget

**Severity:** Medium

Each node is interned by parent, call site, callee, and six argument dwords.
Arguments that vary frequently can produce many distinct nodes even for one
call edge. The memory budget does not include this structure, its hash table,
or its parent/child indexing.

Recommended changes:

- Set a hard node count and byte cap.
- Separate call-path identity from argument-set interning so varying arguments
  do not necessarily duplicate the entire path structure.
- Surface node and argument truncation independently.
- Include measured shadow-tree size in the memory budget.

### 14. Render-to-texture links do not establish exact sampled-pixel ancestry

**Severity:** Medium

Lines 124-128 correctly propose links from a sampled render target back to its
producing draws. That provides a resource dependency edge, but it does not say
which source texel contributed to a selected output pixel. Arbitrary shader
addressing, filtering, dependent reads, wrapping, mipmapping, and projection
make that mapping non-trivial.

Recommended changes:

- Describe render-to-texture traversal as a resource dependency graph.
- Do not call the upstream link exact pixel ancestry.
- Allow the user to open the captured texture or source surface and select a
  source pixel manually.
- Reserve exact sampled-texel ancestry for replay or shader-debugging work.

### 15. CPU uploads between sampled writer events can be misattributed

**Severity:** High

The design compares a post-draw image with the previous post-draw image. If the
CPU modifies backing VRAM between those events and the renderer uploads that
data before the next draw, the next post-draw diff contains both the upload and
the draw. The changed pixels would be attributed to the draw even though some
were introduced by the upload.

Recommended changes:

- Capture or establish the target baseline immediately before each writer,
  after any required upload and before issuing the draw or clear.
- Record upload operations as separate writer events when they alter a tracked
  surface.
- Compare pre-writer and post-writer images for per-event attribution rather
  than only comparing consecutive post-writer images.

### 16. Timeline reconstruction semantics need to be surface-specific

**Severity:** Medium

A global draw index includes writers to offscreen render targets that do not
immediately change the displayed surface. Scrubbing to an offscreen writer
cannot reconstruct a different final displayed frame unless later composition
has also occurred.

Recommended changes:

- Present both a global event timeline and a per-surface writer timeline.
- When selecting a writer, show the target surface at that event.
- Label final-frame reconstruction separately from arbitrary render-target
  reconstruction.
- Include composition dependencies in the global event graph.

## Strong Parts of the Design

- OpenGL-only support is a pragmatic v1 scope reduction.
- A lead-in period before the captured GPU frame is the right general model for
  collecting CPU write attribution.
- Separating broad discovery capture from watched-call recapture keeps the
  common capture bounded.
- RAM-only captures avoid prematurely committing to a permanent file format.
- Explicit hard caps and visible truncation states are appropriate for this
  diagnostic feature.
- Standalone tests for the shadow stack, tag map, diffing, reconstruction, and
  watch log follow the successful call-trace testing pattern.
- Per-command writer tags remain useful even when their confidence is partial.
- Keeping symbols outside v1 avoids tying the first implementation to a symbol
  server or title-specific database.

## Recommended Guarantee Model

The UI and documentation should expose distinct kinds of evidence instead of
placing them under one "exact pixel history" label.

### Color History

Synchronous pre/post snapshots and compressed deltas provide:

- Exact stored color before and after each observed color-changing event.
- Exact reconstruction of tracked surface generations within capture limits.
- No claim about fragments that ran without changing stored color.

### Visible Writer Attribution

An optional integer writer-ID sidecar can provide:

- The last successful fragment writer for a visible pixel.
- A mask of pixels whose last writer has a selected event ID.
- No claim about rejected fragments or all prior blended contributors.

### Resource Dependency

Immutable draw-time resource snapshots and render-target links provide:

- The textures, geometry, constants, shaders, and surfaces consumed by a draw.
- Upstream producing events for render-target-backed resources.
- No claim about exact sampled texels without replay or shader debugging.

### Guest Origin Attribution

TCG CALL/RET/store instrumentation and the tag map provide:

- A best-effort last CPU writer call path for command and resource dwords.
- Explicit unattributed, partial-dword, lost-sync, and pre-arm states.
- No guarantee for stores that predate arming or bypass covered instrumentation.

## Recommended Data-Model Additions

The capture model should include these top-level entities:

- `FrameCapture`: state, limits, truncation flags, final scanout, and event IDs.
- `Event`: method, guest batch, host draw, clear, blit, upload, PVIDEO, or scanout.
- `CommandOrigin`: DMA object, GET offset, physical address, tag node, and
  attribution confidence.
- `SurfaceGeneration`: complete binding identity and initial image.
- `SurfaceDelta`: pre/post color changes for one writer event.
- `ResourceSnapshot`: immutable, deduplicated draw-time bytes or image data.
- `DrawState`: register image plus effective non-register state and resource
  references.
- `CallPath`: interned path identity with separately interned argument sets.
- `WriterTag`: last writer node plus byte coverage and confidence.
- `Scanout`: PCRTC, PVIDEO, selected source, transforms, and final image.

## Recommended Lifecycle Clarification

The word "flip" should be replaced with the exact capture boundary. The
existing profiler and RenderDoc capture use `NV097_FLIP_STALL` in
`hw/xbox/nv2a/pgraph/pgraph.c:901-908`.

Recommended lifecycle:

```text
User arms capture
  -> enable CALL/RET/store instrumentation and request TB flush
  -> collect a lead-in interval for writer tags
first NV097_FLIP_STALL after instrumentation is active
  -> begin GPU frame capture
  -> record methods, writers, resources, and surface histories
second NV097_FLIP_STALL
  -> capture final scanout state and image
  -> finalize immutable capture
  -> disarm instrumentation and request TB flush
  -> publish capture to UI
  -> request VM pause outside PFIFO/PGRAPH critical sections
```

The design should explicitly state that the lead-in interval cannot guarantee
origin tags for commands that were already prepared before arming.

## Testing Additions

In addition to the tests already proposed, add:

- Same-color write: color diff reports no change and does not claim coverage.
- Depth-only and stencil-only writer behavior.
- Initial-surface snapshot followed by first-writer reconstruction.
- Surface rebind at the same address with another format or pitch.
- Overlapping surface generations.
- Blit to a non-current destination surface.
- CPU upload immediately before a draw.
- Render-target-backed texture whose guest RAM copy is stale.
- Capture reopen after guest RAM has changed.
- PVIDEO or explicit unsupported-composition attribution.
- Internal scaling and anti-aliasing memory limits.
- PFIFO method lookahead and squashed BEGIN/DRAW/END command origins.
- Store fault: no writer tag is published.
- Partial and cross-page store tagging.
- Guest thread/context discontinuity in the shadow call stack.
- Watched invocation that exits through exception or non-local unwind.
- Capture finalization and UI acquisition while PFIFO remains active.

## Conclusion

The product direction is sound, and the CPU-origin tagging plus watched-call
recapture could make this substantially more useful for Xbox reverse
engineering than a conventional host GPU capture alone.

Before implementation, the design should narrow "exact" to the evidence each
mechanism can establish, preserve draw-time resources instead of consulting
live RAM, model complete surface generations and final scanout, and replace the
typical-only memory estimate with enforceable global budgets. With those
changes, the feature can remain ambitious without presenting inferred or stale
data as ground truth.

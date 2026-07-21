# Inspect Element Per-Draw State Implementation Plan

**Date:** 2026-07-20

**Design:** `docs/insoectelementplan.md`

## Goal

Add immutable per-submission geometry, state, resource, and provenance data to
the existing frame inspector, then expose it through submission-aware Overview,
Call Paths, Geometry, Pipeline, Combiner, Textures, and Targets views.

The implementation must preserve the current Commands, Readable, Methods,
Pixels, address lookup, immutable origin snapshot, and two-frame provenance
lead-in behavior.

## Repository Facts

- `xemu_frameinspect_capture_arm()` starts provenance before capture storage is
  allocated. `fi_capture_alloc()` currently runs after two complete lead-in
  frames in `xemu_frameinspect_capture_on_flip()`.
- `pgraph_gl_flush_draw()` is called once from `pgraph_gl_draw_end()` and takes
  one mutually exclusive arrays, indexed, inline-buffer, or inline-array route.
- `glMultiDrawArrays` is one submission containing ordered start/count segments.
- DMA and inline-array attribute addresses are resolved in
  `hw/xbox/nv2a/pgraph/gl/vertex.c`. Inline-buffer attributes are uploaded and
  reset in `hw/xbox/nv2a/pgraph/gl/draw.c`.
- `pfifo_run_pusher()` calls `pfifo_run_puller()` synchronously, and the puller
  invokes `pgraph_method()` before typed command records are appended.
- Direct register state and indexed transform program/constant state are
  mutated in `hw/xbox/nv2a/pgraph/pgraph.c`.
- Current render-target-backed texture resources retain only a VRAM address,
  while `pgraph_gl_fi_intern_surface()` can produce an exact capture-local
  surface generation.
- Pixel history checkpoints after a batch. Exact pixel-to-submission selection
  therefore relies on the current one-submission-per-batch invariant.

## Constraints

- Capture data is immutable after publication.
- UI code never rereads live guest RAM or live PGRAPH state.
- Missing information is marked inherited, unavailable, partial, or truncated;
  it is never replaced with a plausible zero.
- New variable-size storage is charged to `FICapture.budget`.
- Full resource provenance is aggregated by byte coverage and bounded.
- Raw NV2A values remain available beside decoded values.
- Renderer input capture shares the renderer's resolved pointers and decoding
  rules instead of independently rereading or reinterpreting data.
- Runtime validation remains manual. Do not launch or control xemu as part of
  an automated verification step.
- Do not include the existing unrelated RenderDoc/OpenGL annotation changes,
  local configuration directories, logs, or `roms/edk2` state in commits.

## Delivery Order

Each phase ends at a usable and testable boundary. Do not start state-source UI
until submission ownership is immutable, and do not start texture navigation
until typed surface-generation references exist.

## Phase 1: Submission Data Model

### Files

- Create `xemu-frameinspect-drawlog.h`.
- Create `tests/frameinspect/test-fi-drawlog.c`.

### Data Model

Define a QEMU-independent, capture-budget-aware `FIDrawLog` with growable pools
for submissions, segments, retained indices, state sources, and resource-writer
aggregates.

Define these core records:

- `FIDrawSubmission`: batch event, execution index, route, topology, submitted
  count, segment/index pool ranges, target generations, state resource IDs,
  geometry digest, color summaries, completeness flags, and fixed per-stage
  references.
- `FIDrawSegment`: first vertex and count, preserving `glMultiDrawArrays` order.
- `FIAttributeDesc`: slot, enabled/constant status, guest format, component
  count, element size, stride, DMA selection/object, guest offset, and resolved
  physical address when available.
- `FIVertexSample`: submission ordinal, source index, per-slot availability,
  up to 16 raw bytes per attribute, decoded four-component values, and source
  reference ranges.
- `FIStateSource`: logical destination, raw method and parameter, command
  physical address, optional frame command ID, writer node, confidence, source
  token, and inherited/unavailable flags.
- `FIResourceWriter`: writer node, confidence, and attributed byte count.
- `FITextureStage`: stage, enable/mode, complete guest metadata, resource ID,
  optional palette ID, optional producer surface generation/event, and bounded
  writer aggregate range.

Use fixed limits where the hardware is fixed:

- 16 vertex attribute descriptors per submission.
- 8 retained vertex samples by default.
- 4 texture-stage records per submission.
- 16 dominant resource writers per captured resource, plus aggregate partial,
  unattributed, and truncated byte counts.

Use the shared capture budget for submission, segment, retained-index, source,
and aggregate pools. A failed append must leave all pool counts unchanged.

### Tests

Cover:

- Deterministic zero initialization.
- Submission append and execution order.
- Ordered multi-draw segment retention.
- Indexed ordinal versus source-index retention.
- Eight-sample cap with full submitted count retained.
- Mixed-writer byte aggregation and deterministic top-writer ordering.
- Pool growth, budget refusal, rollback, and truncation flags.
- Free/reset returning every budget charge.

### Verification

```powershell
& "C:\msys64\ucrt64\bin\gcc.exe" -O2 -Wall -Wextra `
  -o "C:\Users\Tera\AppData\Local\Temp\opencode\test-fi-drawlog.exe" `
  "tests\frameinspect\test-fi-drawlog.c"
& "C:\Users\Tera\AppData\Local\Temp\opencode\test-fi-drawlog.exe"
```

Expected: `PASS`, exit code `0`.

## Phase 2: Capture Ownership and Submission API

### Files

- Modify `xemu-frameinspect-capture.h`.
- Modify `xemu-frameinspect-capture.c`.

### Changes

- Add `FIDrawLog draws` to `FICapture`.
- Initialize it in `fi_capture_alloc()` and release it in `fi_capture_reset()`.
- Include draw-log truncation in capture summary and publication status.
- Add a small capture API for submission begin, attribute/sample contribution,
  texture/target attachment, source attachment, completion, and abort.
- Keep one open submission under the existing capture mutex. Reject nested
  submission begins and mark malformed or incomplete submissions explicitly.
- Require the batch event supplied to submission begin to match the open batch.
- Assign `submit_index` by counting submissions for that batch; assert through
  diagnostics that current captures produce one.
- Add `FI_DRAW_DIAG` with route totals, segment/index/sample counts, incomplete
  submissions, budget truncation, and batches containing more than one
  submission.

### Verification

- Re-run `test-fi-drawlog`.
- Build both Windows targets.
- Run `git diff --check` on the two capture files and new draw-log files.

```powershell
ninja -C build qemu-system-i386.exe qemu-system-i386w.exe
```

Expected: build exit code `0`; existing unrelated warnings may remain.

## Phase 3: Exact Renderer-Input Geometry Capture

### Files

- Modify `hw/xbox/nv2a/pgraph/gl/draw.c`.
- Modify `hw/xbox/nv2a/pgraph/gl/vertex.c`.

### Arrays Route

- Begin the submission before `pgraph_gl_bind_vertex_attributes()`.
- Copy every `draw_arrays_start/count` segment in execution order.
- Build the first-eight submitted ordinal/source-index list by walking segment
  boundaries rather than treating gaps between segments as submitted vertices.
- In `pgraph_gl_bind_vertex_attributes()`, pass each resolved DMA pointer,
  stride, element size, format, and constant value to the open submission before
  `update_memory_buffer()` changes host upload state.

### Indexed Route

- Begin before attribute binding.
- Retain submitted index order within budget and always hash the full index
  stream, including repeated indices.
- Build samples from the first eight submitted ordinals and their corresponding
  source indices.
- Use the same resolved DMA attribute pointers as the GL binding path.

### Inline-Buffer Route

- Begin before iterating `vertex_attributes`.
- Capture populated inline-buffer bytes before clearing
  `inline_buffer_populated` and before replacing `inline_value` with the final
  submitted value.
- Capture unpopulated attributes as constants.

### Inline-Array Route

- Begin before `pgraph_gl_bind_inline_array()`.
- In `vertex.c`, capture after per-slot `inline_array_offset` and vertex stride
  are resolved, but before upload and before any later state mutation.
- Reuse the same layout and attribute-format interpretation as the binding path.

### Common Geometry Rules

- Compute a deterministic full geometry digest from route, topology, ordered
  segments or indices, descriptors, constant attributes, and submitted raw
  attribute bytes. Serialize integers explicitly; never hash structure padding.
- Decode sampled attributes using the renderer's existing format rules.
- Scan all submitted `COLOR0` and `COLOR1` values for component minima/maxima
  without retaining all vertices.
- Record no submission for an empty `NV097_SET_BEGIN_END`; increment an empty
  batch diagnostic instead.
- Complete the submission immediately before the corresponding GL draw call.

### Verification

- Build both Windows targets.
- Manual capture one example of each available route.
- Confirm `FI_DRAW_DIAG` reports no malformed submissions and no batch with
  more than one submission.
- Confirm repeated captures of an unchanged scene produce stable geometry
  digests and segment counts.

## Phase 4: Immutable Submission State and Transform Resources

### Files

- Modify `xemu-frameinspect-capture.h`.
- Modify `hw/xbox/nv2a/pgraph/gl/draw.c`.

### Changes

- Add resource kinds for transform program and transform constants.
- At submission time, intern the exact `pg->regs_` snapshot and attach its
  resource ID directly to the submission rather than discovering it later
  through batch resource order.
- Intern `program_data` and `vsh_constants` into the resource pool and attach
  them to the submission. Content deduplication prevents unchanged state from
  multiplying capture size.
- Copy the 16 `vertex_attributes` descriptors into submission-owned fields;
  never retain pointers to `PGRAPHState` or inline buffers.
- Record programmable/fixed mode, program start, target color/zeta generation,
  target key fields, viewport scale/offset, and final target dimensions.
- Preserve existing batch resource references until all existing UI consumers
  migrate; do not remove compatibility data in this phase.
- Mark active program length and referenced constants unavailable when xemu
  cannot prove them. Do not hash stale trailing instructions as active code.

### Verification

- Build both Windows targets.
- Capture, resume, mutate rendering state, reopen the inspector, and confirm the
  published submission values do not change.
- Confirm resource counts remain bounded through content deduplication.

## Phase 5: Setter Journal Model

### Files

- Create `xemu-frameinspect-setterlog.h`.
- Create `tests/frameinspect/test-fi-setterlog.c`.

### Model

Create a bounded QEMU-independent journal for latest-writer state:

- A monotonic source token identifies one dispatched method parameter.
- A source record stores method, subchannel, parameter, physical address,
  writer node, confidence, and optional frame command ID.
- Destination keys distinguish direct PGRAPH registers, transform instruction
  words, transform constant words, vertex-array descriptors, texture-stage
  fields, combiner fields, raster fields, and target fields.
- Multiple destination keys may point to one source token.
- Replacing a latest writer does not duplicate the source record when the same
  method parameter establishes multiple fields.
- Snapshot copies only sources referenced by the selected submission's relevant
  destination set.

### Tests

Cover direct replacement, one-source/multiple-destination mapping, indexed
destinations, missing command IDs, inherited fields, token wrap refusal,
snapshot immutability, caps, and reset.

### Verification

Compile and run `test-fi-setterlog` using the same standalone GCC pattern as
Phase 1. Expected: `PASS`, exit code `0`.

## Phase 6: Lead-In Setter Ownership

### Files

- Modify `xemu-frameinspect-capture.h`.
- Modify `xemu-frameinspect-capture.c`.

### Changes

- Allocate/reset the setter journal in `xemu_frameinspect_capture_arm()` before
  entering `FI_CAP_ARMED`.
- Keep it alive through `FI_CAP_LEAD_IN` and `FI_CAP_LEAD_IN_2`.
- Transfer or snapshot it into `fi_cap` when `fi_capture_alloc()` succeeds.
- Reset it on arm failure, OpenGL failure, cancellation, shutdown, and capture
  publication failure.
- Add lock-protected dispatch begin, destination-record, frame-command-bind,
  dispatch-end, and submission-source-snapshot APIs.
- Permit source collection in armed and lead-in states, while keeping frame
  command and method logs restricted to `FI_CAP_CAPTURING`.
- Add `FI_SETTER_DIAG` counts for lead-in versus frame sources, direct versus
  indexed destinations, resolved command links, inherited requested fields,
  and truncation.

### Verification

- Run draw-log and setter-log tests.
- Build both Windows targets.
- Cancel during each lead-in state manually and confirm the next capture starts
  with an empty journal and no stale source tokens.

## Phase 7: PFIFO-to-PGRAPH Destination Attribution

### Files

- Modify `hw/xbox/nv2a/pfifo.c`.
- Modify `hw/xbox/nv2a/pgraph/pgraph.c`.

### Dispatch Source Flow

- For every dispatched parameter while capture is armed or capturing, look up
  its writer tag and create a source token before calling
  `pfifo_run_puller()`/`pgraph_method()`.
- Make the current parameter tokens available to PGRAPH for the duration of the
  synchronous dispatch.
- When frame command records are appended after dispatch, bind their immutable
  command IDs back to the matching source tokens.
- End the dispatch context on success, error, or early break so a later method
  cannot inherit stale source identity.
- Preserve existing command-log ordering and lookahead validation.

### Destination Hooks

- Record direct register destinations for the fields required by Pipeline,
  Combiner, Textures, Targets, and vertex-array descriptors.
- In indexed handlers, record the resolved destination and parameter ordinal:
  transform instruction slot/word, transform constant row/component, and any
  other indirect state included in the submission model.
- For methods that establish multiple fields, map every field to the same
  source token.
- Do not mark emulator-internal register writes as guest setter events.
- Leave uninstrumented destinations inherited/unattributed and report them in
  diagnostics rather than inferring a setter.

### Verification

- Build both Windows targets.
- Confirm existing `FI_COMMAND_DIAG` remains `malformed=0` and
  `invalid_links=0`.
- Confirm `FI_SETTER_DIAG` reports lead-in sources and indexed transform
  destinations without truncation.
- Select a known combiner or transform field and verify its source opens the
  same immutable call path as the corresponding command writer.

## Phase 8: Shared NV2A Decode Helpers

### Files

- Create `xemu-frameinspect-nv2a-decode.h`.
- Create `tests/frameinspect/test-fi-nv2a-decode.c`.

### Changes

- Extract or wrap existing renderer interpretation for vertex formats,
  topology names, depth/alpha/blend/stencil/cull state, texture-stage metadata,
  per-stage combiner ICW/OCW, final combiner inputs and low-byte flags, MUX
  selection, and spec/fog factors.
- Keep helpers QEMU-independent where practical and return typed values rather
  than UI strings when a value has semantic structure.
- Keep raw words in every decoded result.
- Represent unknown encodings explicitly.
- Do not alter renderer behavior while extracting helpers.

### Tests

Use fixed raw-word vectors to cover every supported vertex format, representative
pipeline enums, combiner source/mapping/output fields, final flags, MUX MSB/LSB,
and separation of per-stage factors from `SPECFOGFACTOR0/1`.

### Verification

Compile and run the standalone decoder test. Build both Windows targets to prove
the renderer and UI can include the shared header without behavior changes.

## Phase 9: Submission Selection, Overview, and Geometry UI

### Files

- Modify `ui/xui/frame-inspector.cc`.
- Modify `ui/xui/frame-inspector.hh`.

### Changes

- Add stable selected-submission state reset on capture identity changes.
- Resolve submissions under the selected batch; auto-select the only current
  submission.
- Add Overview fields for route, topology, counts, segments, target identity,
  geometry digest, color summary, changed-pixel bounds, and completeness.
- Add Geometry tables for all 16 descriptors and the eight retained samples.
- Emphasize position/RHW, packed and decoded `COLOR0`/`COLOR1`, UV0-UV3,
  submission ordinal, and indexed source index.
- Link sampled source references to immutable writer call paths.
- Preserve Commands/Readable selection independently of submission selection.
- Label pixel attribution as batch-level if diagnostics ever report multiple
  submissions without per-submission checkpoints.

### Verification

- Build both Windows targets.
- Manually inspect arrays, indexed, inline-buffer, and inline-array captures.
- Resume and reopen the inspector; verify Geometry remains unchanged.
- Verify an unavailable attribute shows status, not zero-valued fake data.

## Phase 10: Pipeline, Combiner, and Classified Call Paths UI

### Files

- Modify `ui/xui/frame-inspector.cc`.
- Modify `ui/xui/frame-inspector.hh` only if additional persistent filters are
  required.

### Changes

- Replace the raw-only state presentation with named Pipeline sections while
  retaining the expandable raw register view.
- Render decoded and raw values from the shared helpers.
- Attach each field to its captured `FIStateSource`; open the immutable command
  and full call chain when available.
- Add Combiner per-stage ICW/OCW, factors, routing, mappings, final ABCD/EFG,
  low-byte flags, MUX mode, separate spec/fog factors, and texture shader modes.
- Add Call Paths groups for geometry commands, sampled vertex bytes, transform,
  combiner, texture state/content, target state, and raster state.
- Group resource writers by byte coverage and show unattributed/truncated totals.
- Never assign inherited state to the selected draw's dominant command writer.

### Verification

- Build both Windows targets.
- For a known draw, compare named values against the raw register snapshot.
- Confirm every attributed field opens a valid immutable origin chain after
  resume.
- Confirm inherited fields are labeled and have no fabricated link.

## Phase 11: Typed Texture and Render-Target References

### Files

- Modify `hw/xbox/nv2a/pgraph/gl/draw.c`.
- Modify `xemu-frameinspect-capture.h`.

### Changes

- Replace address-only RT references with typed per-stage metadata.
- For an RT-backed texture, call `pgraph_gl_fi_intern_surface(d, surface)` and
  capture the exact surface generation and producing event where available.
- Record stage, DMA selection/object, offset, format, dimensions, dimensionality,
  pitch, mip count, layout, addressing, filtering, signed-channel state,
  palette identity, and content resource ID.
- Continue refusing to read stale VRAM bytes for authoritative GL render targets.
- For RAM-backed texture and palette bytes, aggregate tag-map origins by byte
  coverage with the fixed dominant-writer cap.
- Preserve explicit unavailable/truncated status for invalid ranges and formats.

### Verification

- Build both Windows targets.
- Capture one RAM texture, one palettized texture when available, and one
  RT-backed texture.
- Verify RT-backed resources contain a generation reference and no stale RAM
  blob.
- Verify aggregate byte totals equal the captured resource length unless range
  validation or truncation is explicitly reported.

## Phase 12: Texture Preview and Target Navigation UI

### Files

- Modify `ui/xui/frame-inspector.cc`.
- Modify `ui/xui/frame-inspector.hh` only if preview texture lifetime state is
  required.

### Changes

- Add four texture-stage views using captured metadata and shared format rules.
- Decode previews only from immutable resource bytes or reconstructed producer
  surface generations.
- Link RT-backed textures to producer events and producer submissions.
- Add selected target producer/consumer lists, later sampling submissions,
  clear history, alias/self-feedback markers, and scanout relationship.
- Match aliases by overlapping guest range plus shape/format identity; do not
  equate address reuse with one persistent surface.
- Keep producer/consumer links capture-local and generation-based.

### Verification

- Build both Windows targets.
- Navigate producer to consumer and back for an RTT chain.
- Confirm a reused address with a different shape creates a distinct generation.
- Confirm previewing never changes after resume.

## Final Regression and Manual Acceptance

### Automated Checks

Run all affected standalone tests:

- `test-fi-drawlog`
- `test-fi-setterlog`
- `test-fi-nv2a-decode`
- `test-fi-commandlog`
- `test-fi-origin`
- `test-fi-shadowstack`
- `test-fi-calltree`
- Existing method, resource, event, surface, and color-history tests

Then run:

```powershell
ninja -C build qemu-system-i386.exe qemu-system-i386w.exe
git diff --check
```

### Manual Acceptance

1. Capture a frame and confirm `FI_DRAW_DIAG`, `FI_COMMAND_DIAG`,
   `FI_ORIGIN_DIAG`, and `FI_SETTER_DIAG` report no malformed records or
   truncation.
2. Select a changed pixel and reach its batch and sole submission.
3. Inspect an indexed draw and verify ordinal/index order, all four UV sets,
   packed colors, decoded colors, and whole-draw color ranges.
4. Open a geometry writer, combiner setter, transform constant setter, texture
   state setter, and texture-content writer through their full captured paths.
5. Verify final combiner flags, MUX mode, per-stage factors, and spec/fog factors
   remain distinct.
6. Follow an RT-backed texture to its producer generation and then to scanout or
   a later consumer.
7. Resume emulation and verify every displayed submission value remains frozen.
8. Capture again and confirm the prior capture is released without stale
   selection, source tokens, GL preview textures, or budget charges.

## Commit Boundaries

Use one focused commit per completed phase or tightly coupled pair:

1. `frameinspect: Add bounded draw submission log`
2. `frameinspect: Own immutable draw submissions`
3. `frameinspect: Capture exact submitted geometry`
4. `frameinspect: Snapshot per-submission transform state`
5. `frameinspect: Add bounded state setter journal`
6. `frameinspect: Track lead-in state setters`
7. `frameinspect: Attribute PGRAPH state destinations`
8. `frameinspect: Share NV2A state decoders`
9. `frameinspect(ui): Add submission geometry inspection`
10. `frameinspect(ui): Add pipeline and call-path inspection`
11. `frameinspect: Capture typed texture dependencies`
12. `frameinspect(ui): Add texture and target navigation`

Before each commit, inspect `git status`, the complete staged diff, and recent
history. Stage only files named by the phase; leave unrelated working-tree state
untouched.

## Stop Conditions

Stop and diagnose before continuing when any of these occurs:

- Renderer input captured by the inspector differs from bytes passed to GL.
- A batch reports more than one submission without an understood renderer
  change.
- A state source resolves to a different command than the source token that
  PGRAPH consumed.
- A lead-in source disappears after capture allocation.
- A texture generation link resolves only by address rather than typed identity.
- Any published record changes after resume.
- Any pool truncates in the normal MM3 validation scene.

At a stop condition, report the disproven assumption, exact diagnostic evidence,
remaining facts, and one next diagnostic step before making another behavior
change.

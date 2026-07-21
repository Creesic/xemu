# Inspect Element: Per-Draw State Plan

## Goal

Expand xemu's existing Inspect Element UI so selecting visible output leads to a complete, immutable view of the draw that produced it:

- What was submitted.
- Which geometry and textures it used.
- What NV2A state was active.
- How the register combiner was configured.
- Which target it wrote and where its inputs came from.
- Which guest functions produced the geometry, commands, state, and resource contents.
- The entire captured guest call path for each attributed writer.

This remains an xemu feature. It is useful as a known-good reference while developing other renderers, but it does not require changes to Plume or any external comparison system.

## Non-Goals

- No Plume integration or Plume-side exporter.
- No JSON packet format.
- No offline xemu-versus-Plume diff tool.
- No dependency on RenderDoc EIDs.
- No guessed `Screen -> Button -> Label` hierarchy.
- No rereading mutable guest state after the frame has been captured.

Existing clipboard actions for methods and call paths may remain, but export is not the product. The Inspect Element UI is the product.

## Current Foundation

xemu already captures most of the relationships needed to build this:

| Existing capability | Location |
|---|---|
| Frame events, render-target generations, clears, blits, and scanout | `xemu-frameinspect-eventlog.h`, `xemu-frameinspect-surfaces.h` |
| Per-batch PGRAPH register snapshot | `hw/xbox/nv2a/pgraph/gl/draw.c:198-206` |
| Texture, palette, and render-target dependency snapshots | `hw/xbox/nv2a/pgraph/gl/draw.c:208-260` |
| Typed PFIFO commands and readable command view | `ui/xui/frame-inspector.cc:730-1089` |
| Grouped method writers and full captured call chains | `ui/xui/frame-inspector.cc:483-728` |
| Pixel history and click-to-select behavior | `ui/xui/frame-inspector.cc:1181-1256,1954-2117` |
| Raw 32 KiB register dump | `ui/xui/frame-inspector.cc:1091-1122` |
| Resource metadata without decoded previews | `ui/xui/frame-inspector.cc:1124-1179` |

The missing layer is a first-class draw submission. One current `FI_EV_BATCH` covers `NV097_SET_BEGIN_END`. Today `pgraph_gl_flush_draw()` is called once from `pgraph_gl_draw_end()` and selects one mutually exclusive route: arrays, indexed elements, an inline buffer, or an inline array. An arrays submission may preserve multiple `glMultiDrawArrays` segments, but it remains one GL submission. The submission index will therefore normally be zero in the current renderer; the separate record still gives Inspect Element a precise renderer boundary and leaves room for future batching changes.

## Inspector Structure

The factual hierarchy should be:

```text
Frame
  Render target generation
    Clear
    Draw batch
      Submission
        Overview
        Call Paths
        Geometry
        Pipeline
        Combiner
        Textures
        Targets
        Commands
        Pixels
```

Selecting a pixel may continue to select the last draw that changed it. Selecting a batch with one submission opens that submission automatically. A future batch with multiple submissions shows them in execution order.

Current pixel history checkpoints after the batch, so exact pixel-to-submission attribution is valid while a batch contains one submission. If the renderer later emits multiple submissions inside one batch, the UI must label the result as batch-level until capture records a pixel checkpoint after each submission.

## Overview

The Overview tab should answer the first diagnostic questions without opening raw tables:

| Group | Fields |
|---|---|
| Identity | Event, batch, submission index, submission route, primitive topology |
| Geometry | Vertex/index count, ranges or segments, active attribute count, screen-space bounds when available |
| Target | Color and zeta guest offsets, sizes, formats, pitches, dimensions, and surface generations |
| Effect | Changed-pixel count and bounds, or an explicit statement that color history cannot prove a write |
| Shading | Fixed/program vertex mode, combiner stage count, four texture-stage modes |
| Writers | Dominant geometry writer, state writer count, texture-content writer count, attribution confidence |

The summary must use guest addresses and NV2A state. Host GL object names are optional diagnostics and must not replace guest identity.

## Call Paths

The existing Origin tab correctly shows full captured call chains, but it groups all method writers primarily by method count. The expanded Call Paths tab should classify writers by what they contributed to the selected submission.

### Writer Groups

| Group | Attribution source |
|---|---|
| Draw/geometry command writers | `DRAW_ARRAYS`, `ARRAY_ELEMENT16/32`, `INLINE_ARRAY`, and immediate vertex methods |
| Vertex-data writers | Tag-map origins for sampled DMA vertex bytes; command origins for inline data |
| Transform-state writers | Latest captured writes to transform mode, program, and constants |
| Combiner-state writers | Latest captured writes to ICW, OCW, control, per-stage factors, final inputs, and spec/fog factors |
| Texture-state writers | Latest captured writes to each stage's offset, format, control, addressing, filter, and palette methods |
| Texture-content writers | Tag-map origins for captured guest texture/palette bytes when attributable |
| Target-state writers | Latest captured surface format, pitch, color offset, and zeta offset writes |
| Raster-state writers | Latest captured depth, blend, alpha, stencil, cull, color-mask, viewport, and scissor writes |

Each writer row shows:

- Symbol and guest address.
- Contribution category and affected fields.
- Method/command count.
- Attribution confidence and partial-store status.
- Full captured `call_site -> callee` chain.
- Captured `ECX/this` and arguments at each call level, as the current Origin tab does.
- Links back to the exact state field, command, sampled vertex, or resource attributed to it.

The UI must distinguish three cases:

| Status | Meaning |
|---|---|
| Attributed | A captured command or guest-memory write has a complete origin node |
| Partial | The writer is known but the write or call path is incomplete |
| Inherited/unattributed | State was already active when capture began or no covered guest write is available |

Inherited state must not be assigned to the first function that happens to draw with it.

Resource-content provenance must remain bounded. For large vertex buffers, textures, and palettes, aggregate tagged bytes by writer and retain a capped set of dominant writers together with attributed, partial, unattributed, and truncated byte counts. Do not allocate one persistent provenance record per resource dword.

## Geometry

Record one immutable draw submission for the selected route in `pgraph_gl_flush_draw()`:

| Route | xemu call |
|---|---|
| Arrays | `glMultiDrawArrays`, preserving every start/count segment |
| Indexed | `glDrawElements`, preserving submitted index order |
| Inline buffer | `glDrawArrays` over per-attribute inline buffers |
| Inline array | `glDrawArrays` over packed inline vertex data |

Each route needs a capture point before any source metadata or inline storage is reset. Capture must consume the same resolved or staged bytes that xemu uses for the GL upload; it must not independently reread guest memory after binding, because the guest CPU or renderer-side preparation may have changed what a second read observes. For inline buffers, capture populated attributes before `inline_buffer_populated` is cleared and before the final inline value is updated. For inline arrays, capture from the same unpacked representation produced for `pgraph_gl_bind_inline_array()` or share its decoder rather than implementing a second unpack path.

### Attribute View

Show all 16 NV2A vertex attribute slots with:

- Semantic slot name.
- Enabled or constant state.
- Guest type, component count, and stride.
- DMA A/B selection and object.
- Guest offset and resolved physical source address.
- Raw little-endian bytes when available.
- Values decoded using xemu's actual attribute rules.
- Writer symbol and call-path link for the sampled source bytes.

The default vertex table should emphasize:

- Position and RHW.
- `COLOR0` and `COLOR1` in packed and decoded float form.
- UV0, UV1, UV2, and UV3 independently.
- Submitted ordinal and source index for indexed draws.

Large draws must remain bounded. Retain the first eight submitted vertices by default, compute a digest over the complete submitted geometry, and summarize whole-draw `COLOR0`/`COLOR1` component minima and maxima. This preserves the body-versus-glass alpha evidence without storing every vertex of a large mesh.

The UI may offer a larger sample cap before capture is armed, but it must not silently allocate an unbounded vertex dump.

### Transform View

Show:

- Fixed-function versus programmable execution mode.
- Program start and raw transform instructions.
- Program disassembly when xemu already has a reliable decoder.
- Vertex constants with raw words and float interpretation.
- Constants referenced by the active program highlighted.
- Viewport scale/offset and final target dimensions.
- Specular enable and separate-specular state.

Transform program, constants, and `vertex_attributes` live outside the existing `regs_` snapshot in `PGRAPHState`, so they must be copied into the immutable submission record.

## Pipeline

Replace the raw-only State tab with named sections while retaining an expandable raw-register view.

| Section | State to decode |
|---|---|
| Render target | Color/zeta offsets, pitches, formats, dimensions, anti-aliasing |
| Viewport/scissor | Viewport scale/offset, surface clip, effective dimensions |
| Rasterization | Cull enable/face, front face, fill modes, provoking vertex, smoothing |
| Depth | Enable, write enable, function, zeta format |
| Alpha test | Enable, function, reference |
| Blending | Enable, source/destination factors, equation, blend constant, logic operation |
| Stencil | Enable, function, reference, read/write masks, fail/zfail/pass operations |
| Color output | RGBA write mask, dithering |

Every decoded field shows the raw register value and, when available, the most recent captured method and writer that established it. Clicking the source opens that writer's full call path.

## Combiner

The Combiner tab must expose raw words and xemu's decoded interpretation side by side.

### Per-Stage State

For every active stage show:

- Color ICW inputs A/B/C/D, source registers, channel selection, and mappings.
- Alpha ICW inputs A/B/C/D, source registers, and mappings.
- Color and alpha OCW destinations.
- AB, CD, and SUM/MUX output routing.
- Dot-product, blue-to-alpha, bias, scale, and output-map behavior.
- Per-stage `NV_PGRAPH_COMBINEFACTOR0/1` values.
- Writer and call-path link for every raw word.

### Final State

Show:

- Final A/B/C/D and E/F/G inputs.
- `CLAMP_SUM`, `COMPLEMENT_V1`, and `COMPLEMENT_R0` from the final-input low-byte flags.
- MUX selection using R0 alpha MSB or LSB.
- `NV_PGRAPH_SPECFOGFACTOR0/1` as final C0/C1.
- A clear visual separation between final spec/fog factors and per-stage combine factors.
- Texture shader modes for stages 0 through 3.

The active shader handle may be shown as an implementation diagnostic, but it is not a substitute for the guest combiner state.

## Textures

For all four stages show:

- Enabled and active texture-shader mode.
- Guest address and DMA selection.
- Width, height, pitch, dimensionality, mip count, and format.
- Linear, swizzled, compressed, or palettized layout.
- Address U/V/P behavior and wrap flags.
- Minification, magnification, mip, and signed-channel filtering state.
- Palette address and size where applicable.
- Guest-content digest.
- Decoded preview and raw-byte view.
- Resource writer and call-path link where attributable.

Render-target-backed textures should link to the exact earlier surface generation and writer event. The UI must not preview stale VRAM bytes when the authoritative content is an active render target.

The current `FI_RESK_TEXTURE_RTREF` metadata contains only a surface VRAM address. Replace or supplement it with a typed captured reference containing the texture stage, exact `FISurfaceStore` generation, producer event, guest offset, and compatibility metadata used when the surface was bound as a texture. Address alone is insufficient because the same range may be rebound with a different format, pitch, or shape.

## Targets

The Targets tab extends the existing render-dependency tree with selected-draw context:

- Color and zeta surface generations written by the draw.
- Earlier draw that produced each render-target-backed texture.
- Later draws that sample the selected target.
- Surface aliasing and self-feedback cases.
- Clear history and first/last writer.
- Scanout relationship, including whether this target was eventually presented.

This navigation should use guest offsets plus surface generation, not only addresses, because the same address may be rebound with different format, pitch, or dimensions.

## Capture Model

Add a bounded immutable submission log to `FICapture`. A submission record owns or references:

- Batch event and execution-order index.
- Route, topology, ranges, segments, and indices.
- Vertex descriptors and bounded raw/decoded samples.
- Whole-draw geometry and color summaries.
- Transform program/constants needed to interpret the geometry.
- Target and zeta identities.
- Relevant decoded-state source references.
- Texture-stage metadata and resource references.
- Writer-node references resolved through the capture's immutable origin snapshot.

Geometry and resource source records may cover multiple dwords with different writer tags. Preserve multiple contributing writers when necessary and report byte coverage rather than assigning an entire sample or resource to whichever tag is encountered first.

State source tracking must begin when capture is armed, before the two complete provenance lead-in frames. `FICapture` and the frame command log are currently allocated only when the captured frame begins, so maintain a small armed-state setter journal outside the not-yet-allocated capture or allocate that journal at arm time. At the capture boundary, transfer its latest setters into capture-owned immutable storage. A lead-in setter that has no frame command-log row retains a compact setter snapshot containing method, parameter, physical command address, writer node, confidence, and logical destination instead of fabricating a frame command reference.

Direct register setters can use a register-indexed latest-writer map, but transform programs, constants, and other indexed methods require destination-aware tracking where `PGRAPHState` is mutated. PFIFO should provide the active command identity and writer origin; PGRAPH method handlers should report the resolved logical destination, such as a program instruction slot or constant row. Snapshot those source references into each submission. State with no observed setter remains inherited/unattributed.

Do not derive submission state later from the emulator's live PGRAPH state. The emulator resumes after capture publication, so live state is not evidence for the frozen frame.

## Implementation Tasks

### Task 1: Submission Data Model

Files:

- Create `xemu-frameinspect-drawlog.h`.
- Create `tests/frameinspect/test-fi-drawlog.c`.

Define bounded submission, segment, index, attribute, transform, source-reference, color-summary, and resource-writer aggregate records. Test append order, caps, immutable ownership, mixed-writer byte coverage, and truncation.

### Task 2: Capture Ownership

Files:

- Modify `xemu-frameinspect-capture.h`.
- Modify `xemu-frameinspect-capture.c`.

Add the submission log to `FICapture` and add an armed-state setter journal whose lifetime includes the lead-in. Transfer the journal into immutable capture ownership at the capture boundary. Charge allocations to the existing capture budget and preserve the typed command log and immutable origin snapshot.

### Task 3: Per-Submission Geometry

File:

- Modify `hw/xbox/nv2a/pgraph/gl/draw.c`.

Capture the current route at its route-specific safe point before inline data or source metadata is reset. Share the renderer's resolved/staged attribute bytes and decoding path instead of rereading mutable guest memory. Preserve multi-draw segments and indexed order. Capture first-eight samples, complete geometry digest, and complete-draw `COLOR0`/`COLOR1` ranges. Test the current invariant that a batch produces one submission while retaining an execution-order index for future changes.

### Task 4: State Sources

Files:

- Modify `hw/xbox/nv2a/pfifo.c`.
- Modify `hw/xbox/nv2a/pgraph/pgraph.c` for destination-aware indexed state mutations.
- Modify the capture module API as needed.

Carry the active PFIFO command identity and origin into PGRAPH dispatch. Track direct register destinations and report resolved indexed destinations from the handlers that mutate transform programs, constants, and similar state. Preserve compact setter snapshots for lead-in setters that have no frame command record. Snapshot relevant source references per submission. Reuse existing writer tags and origin confidence rather than creating a second provenance system.

### Task 5: Inspector Navigation

Files:

- Modify `ui/xui/frame-inspector.cc`.
- Modify `ui/xui/frame-inspector.hh` only for persistent selection/filter state.

Add submission selection beneath batches, Overview and Call Paths tabs, and links among state fields, commands, methods, resources, pixels, and origin chains. Preserve Commands/Readable behavior.

### Task 6: Decoded State Views

Files:

- Create a small shared NV2A frame-inspector decode helper and standalone test if the existing PGRAPH decoders cannot be reused directly.
- Modify `ui/xui/frame-inspector.cc`.

Implement or reuse testable decoding helpers for Geometry, Pipeline, and Combiner state, then render their immutable results in the UI. Keep raw values visible beside every decode. Do not create a second set of combiner, texture, or vertex-format semantics solely inside the UI.

### Task 7: Texture Inspection

Files:

- Modify `hw/xbox/nv2a/pgraph/gl/draw.c` to capture complete stage metadata.
- Modify `ui/xui/frame-inspector.cc` to decode and preview captured resources.

Reuse xemu's existing texture format and palette rules rather than adding an independent decoder with different semantics. Capture bounded resource-writer aggregates and typed render-target references carrying the exact surface generation and producer event.

### Task 8: Target Navigation

File:

- Modify `ui/xui/frame-inspector.cc`.

Extend the current dependency tree with producer/consumer jumps, selected-draw target context, alias markers, and scanout relationship. Derive links from typed generation references, not from VRAM address alone. Keep pixel attribution at batch level unless a pixel checkpoint exists for the selected submission.

## Priority

| Priority | Deliverable |
|---|---|
| P0.1 | Submission identity, route, ordered segments, immutable target/state snapshot, and current one-submission-per-batch invariant |
| P0.2 | Geometry descriptors, exact renderer-input sampling, `COLOR0`/`COLOR1`, and UV0-UV3 |
| P0.3 | Lead-in setter journal, destination-aware state provenance, and classified Call Paths |
| P0.4 | Shared decoded Pipeline and Combiner state with raw values and setter call-path links |
| P1.1 | Four-stage texture metadata, previews, bounded content origins, and exact RT-generation links |
| P1.2 | Target/zeta producer-consumer navigation and alias/self-feedback markers |
| P2 | Better depth/stencil-only attribution and visibility explanations |
| P2 | Cross-frame pinning after a reliable per-draw identity exists |

Frame-boundary diagnostics such as flip read/write/modulo and `PCRTC.start` may be added later for cadence investigations, but they are separate from fleshing out the selected draw.

## Acceptance Criteria

- Selecting a changed pixel can navigate to the exact batch and submission that changed it.
- Arrays, indexed, inline-buffer, and inline-array submissions appear in execution order.
- Current captures produce one submission per batch; any future multi-submission batch is explicit and does not claim exact pixel attribution without per-submission checkpoints.
- The selected submission shows all 16 attribute descriptors and bounded decoded vertex samples.
- Captured vertex samples and hashes use the same resolved/staged bytes submitted by the renderer and never reread mutable guest data after binding.
- `COLOR0` and `COLOR1` show raw/packed values, decoded values, and whole-draw ranges when available.
- UV0, UV1, UV2, and UV3 are shown independently and are never inferred from one another.
- Pipeline state is named and decoded, with the original raw register value retained.
- Combiner state includes per-stage ICW/OCW, factors, output mappings, final inputs/flags, MUX mode, and separate spec/fog factors.
- All four texture stages show complete guest metadata and decoded previews where supported.
- Color/zeta targets and RT-backed textures link to their producer and consumer events by surface generation.
- Every attributable geometry, state, texture, and target source can open its full captured guest call chain.
- State setters observed during lead-in remain available through compact immutable setter snapshots even when they have no frame command-log row.
- Indexed transform-program and constant setters resolve to their logical destination rather than only their repeated PFIFO method address.
- Large resource provenance is reported as bounded writer coverage with explicit unattributed and truncated byte counts.
- Inherited or unavailable provenance is labeled honestly rather than assigned to an unrelated draw function.
- Current Commands, Readable, Methods, Pixels, address lookup, and origin functionality does not regress.

## Deferred Work

A semantic UI tree remains out of scope until guest object layouts and relationships are proven from guest memory. The renderer can identify draws, resources, state, and guest producers; it cannot honestly infer that a rectangle is a button or that a glyph run belongs to a named label.

No runtime implementation is part of this planning change.

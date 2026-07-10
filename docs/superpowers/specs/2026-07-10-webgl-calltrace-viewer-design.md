# WebGL Renderer for the Call-Trace Viewer — Design

**Date:** 2026-07-10
**Status:** Approved (pending spec review)
**Builds on:** the existing viewer in `tools/calltrace/viewer.html`
(2D-canvas renderer, timeline, fade, collapse, noise/legend panels).

## Goal

Render the graph (nodes, edges, pulses) with WebGL so full-resolution
pan/zoom/playback stay smooth even on machines where the browser's 2D canvas
falls back to software rasterization. Keep every existing feature. Fall back
to the current 2D renderer when WebGL is unavailable.

## Why

The profiler proved the viewer is paint/upload-bound, not JS-bound: ~0.5 ms
JS per frame but ~130 ms of canvas rasterization+upload when the 2D canvas is
software-rendered. WebGL draws colored geometry on the GPU directly, and — the
key win — pan/zoom become a single uniform update rather than a full repaint,
so moving the view stops touching the rasterizer at all.

## Decisions (from brainstorming)

| Question | Decision |
|---|---|
| Text | Hybrid: WebGL for shapes, a 2D overlay canvas for labels |
| Coexistence | WebGL default; auto-fallback to the existing 2D renderer |
| Visual fidelity | Simplest shapes, prioritize speed: sharp rects, straight edges, plain orange dot pulses, no glow/rounded corners/SDF |
| Text during motion | Overlay repainted only when the view is static; hidden during motion |
| File | Single-file constraint: everything stays in `viewer.html` |

## Architecture

Two stacked canvases inside `#cvwrap`:

- `#glcv` (bottom): the WebGL canvas — nodes, edges, pulses.
- `#cvtext` (top, transparent): a 2D canvas — labels only.
- The existing `#cv` (2D) remains and is used when WebGL is unavailable.

A single flag `glActive` decides the path. On load `initGL()` tries to get a
`webgl` context and compile the program; on any failure `glActive = false` and
the viewer uses the existing 2D `draw()` unchanged. All non-render logic
(`parseXCT`, `deriveModel`, `buildTimeline`, `computeLayout`, hit-testing,
search, noise, legend, symbols, DOT export) is shared and untouched.

### Render dispatch

- `draw()` becomes a dispatcher: `glActive ? glDraw() : draw2d()` where
  `draw2d` is today's `draw` body renamed.
- `glDraw()` rebuilds GPU buffers only when the visible set changed (same
  `lastLayoutKey` boundary already used to cache layout), then sets the
  transform uniform from `view` and issues the draw calls.
- Pan/zoom/idle frames: uniform update + draw calls only — no geometry work,
  no software raster.

## WebGL rendering

WebGL1 (broadest support; no instancing/extensions needed — geometry is
batched into shared buffers). One shader program for all colored quads.

### Shaders

Vertex:
```glsl
attribute vec2 aPos;      // world coords
attribute vec4 aColor;    // rgba, 0..1
uniform vec4 uXform;      // (zoomX, zoomY, panX, panY) in clip space
varying vec4 vColor;
void main() {
  vec2 p = aPos * uXform.xy + uXform.zw;   // world -> clip
  gl_Position = vec4(p, 0.0, 1.0);
  vColor = aColor;
}
```
Fragment:
```glsl
precision mediump float;
varying vec4 vColor;
void main() { gl_FragColor = vColor; }
```

`uXform` maps world→clip using **CSS** (logical) width/height, which is
resolution-independent: `zoomX = 2*zoom/cssW`, `zoomY = -2*zoom/cssH`,
`panX = 2*panX/cssW - 1`, `panY = 1 - 2*panY/cssH` (y flipped for GL). The
`#glcv` backing store is sized `cssW*dpr × cssH*dpr` and `gl.viewport` is set
to that full device size for crisp output (WebGL is fast enough for full dpr;
no ½-res needed). Alpha (fade) is folded into `aColor.a`, so no separate
attribute.

### Geometry (built in `glBuild()`, CPU → typed arrays → buffers)

Two triangles per quad; colors per vertex; all in one interleaved array per
category, uploaded to one buffer each:

- **Nodes**: for each visible node, a rect `[x, y, w, h]` → 2 triangles, color
  = `nodeColor(n)` as rgba with `a = nodeAlpha(n)`. The selected node also
  emits a slightly larger border quad in the selection color drawn behind it.
- **Edges**: for each visible edge, a thick straight segment from
  `(a.x+a.w, a.y+a.h/2)` to `(b.x, b.y+b.h/2)` → a rotated rectangle (2
  triangles) whose half-width in **world units** is `1 + log2(1+count)*0.4`
  (so thickness still encodes call count; the GPU makes wide fills cheap, so
  no cap is needed), color grey `EDGE_CALL` or blue `EDGE_CROSS`. Plus a small
  triangle arrowhead at the target end. Recursion self-edges: a short stub.
- **Pulses** (dynamic, rebuilt per playing frame): a small quad per live pulse
  at its lerped position along the straight edge, orange, alpha by age.

Buffers for nodes/edges are rebuilt only on visible-set change. During
playback, node alpha (fade) changes per frame; the node buffer's color array
is refilled and re-uploaded each frame while playing (bounded by visible node
count). Pulses are re-uploaded each playing frame.

### Draw order per frame

1. `gl.clear` to the background color.
2. Draw edges buffer.
3. Draw nodes buffer (selection border already included).
4. Draw pulses buffer (if timeline active).
5. Repaint the text overlay **iff** the view is static this frame (see below).

## Text overlay

`#cvtext` is a transparent 2D canvas sized like the others. `drawLabels()` is
reused, drawing node labels + edge count labels at capped screen size (as it
does now), but onto `#cvtext`'s context.

To keep software text-paint off the hot path:
- A `viewStatic` flag is set false by `nudgeMotion()` (existing motion hook,
  reused) and true ~180 ms after motion settles.
- The text overlay is repainted only when the view is static, on selection
  change, and on layout/visible-set change. During motion it is cleared once
  and left blank.
- Result: labels vanish while you actively pan/zoom/play and snap back crisp
  when you stop — no per-frame glyph rasterization.

The empty-state hint ("Open or drop an .xct…") is drawn on `#cvtext`.

## Feature-parity mapping

| Feature | WebGL handling |
|---|---|
| Section colors | per-vertex node color |
| Node fade (timeline) | folded into node color alpha, refreshed per playing frame |
| Selection highlight | border quad in the node buffer |
| Collapse / mute / timeline visibility / first-fire | unchanged CPU logic decides which nodes/edges are packed |
| Cross-link vs call | edge color |
| Arrowheads | small triangle per edge |
| Pulses | dynamic orange quad buffer |
| Click / double-click / hit-test | unchanged (`nodeAt` on `layoutNodes`) |
| Search / reveal / details / legend / noise / symbols / DOT | unchanged (data/DOM) |
| Viewport culling, adaptive ½-res | not needed on the GL path; retained for the 2D fallback |
| Perf overlay | reused; `draw (JS)` now includes glBuild/glDraw phases |

## Error handling & fallback

- `getContext('webgl')` returns null, or shader compile/link fails →
  `glActive = false`, hide `#glcv`/`#cvtext`, show `#cv`, use `draw2d`.
- `webglcontextlost` event → prevent default, set `glActive = false`, fall
  back to the 2D renderer (no attempted restore in v1).
- The 2D path is unchanged and remains fully functional.

## Testing

- **Existing selftests** (parse, derive, timeline, first-fire, noise, DOT,
  collapse — 12 total) are renderer-agnostic and must stay green.
- **New selftests** (run in the `?selftest=1` harness, which has a WebGL
  context under headless Chromium/SwiftShader):
  - `initGL()` succeeds and compiles the program; `glActive === true`.
  - After loading the timed fixture and `glDraw()`, `gl.readPixels` at a known
    on-screen node center returns that node's section color (not background),
    and a point in empty space returns the background color.
  - Panning (changing `view`) then `glDraw()` again does **not** rebuild
    geometry (a build counter is unchanged) — proves pan/zoom is uniform-only.
  - Forcing `glActive = false` routes `draw()` to `draw2d` and still renders
    (fallback path intact).
- **Manual** on a real MM3 timed recording: full-res pan/zoom smooth with the
  Perf overlay showing paint cost gone; labels appear on settle; timeline
  playback, pulses, collapse, fade, selection, and the noise/legend panels all
  behave as before.

## Non-goals (v1)

- Rounded corners, soft pulse glow, curved/bezier edges, SDF text.
- WebGL2 / instancing (batched WebGL1 buffers suffice).
- Context-loss auto-restore (fall back to 2D instead).
- Replacing or deleting the 2D renderer (it is the fallback).
- GPU-side p-picking (CPU hit-test is unchanged and adequate).

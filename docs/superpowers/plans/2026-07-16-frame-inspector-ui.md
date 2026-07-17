# Frame Inspector UI (Plan 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the in-emulator ImGui inspector overlay that opens when a frame capture publishes, shows the frozen frame, and lets the user hover pixels and walk each draw's tree — guest call-path origin, methods, register state, resources, and colour history — the "inspect element for a frame" payoff.

**Architecture:** A single plain ImGui window (`ui/xui/frame-inspector.cc/hh`, debug/monitor-window style) drawn each frame from `xemu_hud_update()`. It reads the published capture only via the refcounted `xemu_frameinspect_capture_acquire()/release()` contract, auto-opens when a capture is available, uploads the scanout image to a GL texture for an interactive `ImGui::Image`, maps hover to guest-framebuffer coordinates, and resolves per-draw detail from the capture's event log / colour history / method log / resource pool. The Origin tab resolves method `writer_node` ids through a new read-only Plan-1 calltree API (valid against the live tree until the next arm). Match the existing xui debug-window aesthetic — this is a developer tool, not a bespoke visual design.

**Tech Stack:** C++ / Dear ImGui 1.91.8 (vendored fork), xemu xui layer, the Plan 2B capture data model (C, `extern "C"`), Plan 1 calltree.

**Spec:** `docs/superpowers/specs/2026-07-16-frame-inspector-design.md` (the "Inspector UI" section).

**Builds on:** Plan 2B (`docs/superpowers/plans/2026-07-16-frame-inspector-capture-fill.md`), HEAD after 2B = commit `18623d2801`.

## Global Constraints

- Read published capture data ONLY via `xemu_frameinspect_capture_acquire()` (returns `const FICapture *` or NULL) paired with `xemu_frameinspect_capture_release()`. Acquire once per Draw(), release before returning (all paths). Never retain a raw `FICapture*` or any sub-pointer across frames or past release. The capture stays valid across VM resume; a *new* capture (re-arm) frees the old one only when the last reference is released.
- **Join keys (from the 2B final review):** the `FIEvent` array index is the universal key. Colour-history events carry an `event_id` = the event index; `FIMethodBatch.batch_event` and `FIBatchResRef.event` both reference the **begin_batch** event index. When `begin_batch`'s append failed, its event ref is `FI_EVENT_INVALID` — skip such joins.
- **Honest limitations to surface, never hide (do not fabricate):** `FI_METHOD_RAW_WORD` (0xFFFF) method records are lookahead-squashed raw dwords — render `param` as a raw hex dword, not a named method. `FI_RESK_TEXTURE_RTREF` resources have `len==0`, `meta` = producing surface's VRAM addr — render as a dependency hop, not texel bytes. `FI_SCANOUT_PVIDEO` flag = overlay was on-screen but its content is NOT captured — label unsupported. Blit colour history attaches to the current GL binding, not necessarily the blit destination — treat blit pixels as best-effort. Split-batch `FI_EV_BATCH` events may have no methods/resources. No zeta/depth colour history exists.
- Everything runs on the main/UI thread inside `xemu_hud_update()` (VM already paused by the time a capture is available). GL texture uploads use the current UI GL context.
- Match xui conventions: `#include "common.hh"` first; include repo-root C headers as `"../../xemu-frameinspect-capture.h"` / `"../../xemu-frameinspect.h"`; monospace via `g_font_mgr.m_fixed_width_font`; scale sizes by `g_viewport_mgr.m_scale`; `ImTextureID` cast is `(ImTextureID)(intptr_t)tex` (ImTextureID is a 64-bit int typedef in this build).
- Emulator build (MSYS2 UCRT64): `MSYSTEM=UCRT64 C:/msys64/usr/bin/bash.exe -lc "ninja -C /c/Users/Tera/Documents/GitHub/xemu/build qemu-system-i386.exe qemu-system-i386w.exe 2>&1 | tail -15"`. Success = touched files compile, both exes link (pre-existing qtest `qemu_ftruncate64` failures unrelated). Editor clangd errors on osdep/SDL/imgui includes are false positives.
- License headers: match the existing `ui/xui/*.cc` headers (GPL-2.0-or-later, xemu authors) — copy the block from `ui/xui/monitor.cc`. Commit prefix: `frameinspect(ui):`.
- Since this is visual, most tasks are build-verified; each task's final step lists a **manual visual check** for the human to run in a live capture. Do NOT claim visual behavior works without the human confirming — state "build verified; visual check deferred to human".

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `xemu-frameinspect.h` / `.c` | modify | New read-only calltree-node lookup API for the Origin tab |
| `ui/xui/frame-inspector.hh` | create | `FrameInspectorWindow` class + `extern` instance |
| `ui/xui/frame-inspector.cc` | create | The whole inspector window (grows across tasks) |
| `ui/xui/meson.build` | modify | Add `frame-inspector.cc` |
| `ui/xui/main.cc` | modify (`xemu_hud_update` ~309-319) | Dispatch `frame_inspector_window.Draw()` |
| `ui/xui/menubar.cc` | modify (Debug menu ~222) | Menu entry to open the inspector |

---

### Task 1: Inspector window skeleton — opens on capture, shows the frozen frame

**Files:**
- Create: `ui/xui/frame-inspector.hh`, `ui/xui/frame-inspector.cc`
- Modify: `ui/xui/meson.build`, `ui/xui/main.cc`, `ui/xui/menubar.cc`

**Interfaces:**
- Consumes: `xemu_frameinspect_capture_acquire/release`, `FICapture` (surfaces/events/resources counts), `xemu_frameinspect_capture_summary`, `FICaptureState xemu_frameinspect_capture_state()`; the scanout: find the `FI_EV_SCANOUT` event (kind) in `cap->events.events[i]` and its surface generation; the scanout image lives in that generation's colour history (`cap->hist[gen]`, reconstruct the last event) OR — SIMPLER for Task 1 — just show capture stats + a placeholder, and wire the real image in Task 3. Decide and record.
- Produces: `class FrameInspectorWindow { public: bool m_is_open = false; void Draw(); }; extern FrameInspectorWindow frame_inspector_window;`

- [ ] **Step 1: Header**

Create `ui/xui/frame-inspector.hh` (license block copied from `ui/xui/monitor.cc`, `#pragma once` or guard as siblings use), including `"common.hh"`, declaring the class + `extern FrameInspectorWindow frame_inspector_window;`. Members for now: `bool m_is_open = false;` and a `bool m_auto_open_armed = true;` (so it auto-opens once per capture, not every frame after the user closes it — track the last-seen capture identity; simplest: an `unsigned m_last_seen_events = 0;` and auto-open when the acquired capture's `events.count` differs from last seen).

- [ ] **Step 2: Skeleton Draw()**

Create `ui/xui/frame-inspector.cc`:
```cpp
#include "common.hh"
#include "frame-inspector.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
extern "C" {
#include "../../xemu-frameinspect-capture.h"
#include "../../xemu-frameinspect.h"
}

FrameInspectorWindow frame_inspector_window;

void FrameInspectorWindow::Draw()
{
    const FICapture *cap = xemu_frameinspect_capture_acquire();
    if (!cap) {
        return; /* nothing published */
    }
    /* Auto-open once when a new capture publishes. */
    if (cap->events.count != m_last_seen_events) {
        m_last_seen_events = cap->events.count;
        m_is_open = true;
    }
    if (!m_is_open) {
        xemu_frameinspect_capture_release(cap);
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(900.0f * g_viewport_mgr.m_scale,
                                    620.0f * g_viewport_mgr.m_scale),
                             ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Frame Inspector", &m_is_open,
                     ImGuiWindowFlags_NoCollapse)) {
        char *summary = xemu_frameinspect_capture_summary();
        ImGui::TextUnformatted(summary);
        g_free(summary);
        ImGui::Separator();
        ImGui::Text("Events: %u   Surfaces: %u   Resources: %u",
                    cap->events.count, cap->surfaces.num_gens,
                    cap->resources.num_res);
        if (cap->truncated || cap->events.truncated || cap->surfaces.truncated ||
            cap->resources.truncated || cap->methods.truncated) {
            ImGui::TextColored(ImVec4(1,0.6f,0,1),
                "[TRUNCATED] capture hit a cap/budget; some data is missing.");
        }
        /* Task 2+ fill the rest. */
    }
    ImGui::End();
    xemu_frameinspect_capture_release(cap);
}
```
Add `unsigned m_last_seen_events = 0;` to the header class. Verify `cap->methods` exists (Task 4 of 2B added it) — read the current FICapture struct in `xemu-frameinspect-capture.h` and match field names exactly (events.count, surfaces.num_gens, resources.num_res, methods.truncated).

- [ ] **Step 3: meson + dispatch + menu**

- `ui/xui/meson.build`: add `'frame-inspector.cc',` to the `files(...)` list.
- `ui/xui/main.cc` `xemu_hud_update()` (near main.cc:309-319 where the other `.Draw()` calls are): add `frame_inspector_window.Draw();` and `#include "frame-inspector.hh"` at the top with the other window includes.
- `ui/xui/menubar.cc` Debug menu (near line 222, beside Monitor/Audio/Video): add `ImGui::MenuItem("Frame Inspector", NULL, &frame_inspector_window.m_is_open);` and include `"frame-inspector.hh"`.

- [ ] **Step 4: Build**

Run the build. Expected: frame-inspector.cc compiles, both exes link.

- [ ] **Step 5: Manual visual check (human)**

List for the human (do not run): launch a game on the GL renderer, Ctrl+Alt+I, wait for pause; the Frame Inspector window should auto-open showing the summary + event/surface/resource counts (non-zero), and the Debug→Frame Inspector menu item should toggle it.

- [ ] **Step 6: Commit**

```bash
git add ui/xui/frame-inspector.hh ui/xui/frame-inspector.cc ui/xui/meson.build ui/xui/main.cc ui/xui/menubar.cc
git commit -m "frameinspect(ui): Inspector window skeleton, opens on capture publish"
```

---

### Task 2: Event list + selection

**Files:**
- Modify: `ui/xui/frame-inspector.cc`, `ui/xui/frame-inspector.hh`

**Interfaces:**
- Consumes: `cap->events.events[i]` (`FIEvent { uint8_t kind; uint32_t surface_gen; uint32_t seq; uint32_t a0..a3; }` — verify field names in `xemu-frameinspect-eventlog.h`), event kinds `FI_EV_BATCH/CLEAR/BLIT/UPLOAD/SCANOUT`.
- Produces: a left-pane scrollable event list; `int m_selected_event = -1;` selection state on the class.

- [ ] **Step 1: Two-pane layout + event list**

In `Draw()`, below the summary, split into a left list pane (`ImGui::BeginChild("events", ImVec2(260*scale, 0), true)`) and a right detail pane (`ImGui::SameLine(); ImGui::BeginChild("detail", ...)`). In the list, iterate `cap->events.count`, render each as a `ImGui::Selectable` labeled by kind + index + surface gen, e.g. `"#%u %s gen=%u"` where the kind name comes from a small static `const char* ev_kind_name(uint8_t)` helper (BATCH/CLEAR/BLIT/UPLOAD/SCANOUT/?). Clicking sets `m_selected_event = i`. Use `ImGui::PushID(i)`. Colour scanout/blit distinctly (TextColored or a tag).
- In the detail pane, if `m_selected_event >= 0 && < count`, show the raw event fields (kind, surface_gen, seq, a0-a3 as hex) using the monospace font. Task 4 replaces this with tabs.

- [ ] **Step 2: Build + manual check + commit**

Build. Manual check (human): the event list populates; clicking an event shows its fields. Commit: `git commit -m "frameinspect(ui): Event list with selection + raw event fields"`.

---

### Task 3: Frozen-frame image + hover → framebuffer coordinate + pixel history

**Files:**
- Modify: `ui/xui/frame-inspector.cc/.hh`
- Add a small colour-history reconstruct accessor if needed (see below).

**Interfaces:**
- Consumes: the scanout generation's colour history for the final image; `fi_colorhist_reconstruct(ch, event_index, out_rgba)` and `fi_colorhist_pixel_history(ch, pixel_index, out, max)` from `xemu-frameinspect-colorhist.h` (these operate on `cap->hist[gen]`). The scanout gen: scan `cap->events` for the `FI_EV_SCANOUT` event, take its `surface_gen`; the final image = reconstruct that gen's colour history at its last event (`fi_colorhist_num_events(ch)-1`). If no scanout gen / no hist, show "no image" (missing).
- Produces: a GL texture holding the reconstructed final frame, displayed via `ImGui::Image`; hover mapping; pixel-history readout.

- [ ] **Step 1: Reconstruct the final image into a GL texture**

On selection-of-capture (detect a new capture via `m_last_seen_events`), reconstruct the scanout generation's final image (`fi_colorhist_reconstruct` into a `std::vector<uint32_t>` sized `width*height`) and upload to a GL texture (`glGenTextures`/`glTexImage2D` GL_RGBA8; keep the id + w/h on the class; delete+recreate on new capture). Read `cap->hist[gen].width/height`. Guard: gen valid, hist inited (width != 0). Note the colorhist RGBA byte order is what `pgraph_gl_fi_readback_surface` produced (GL_RGBA/GL_UNSIGNED_BYTE) — upload with matching format so colours are correct; verify and record.

- [ ] **Step 2: Display + hover mapping**

Display the texture with `ImGui::Image((ImTextureID)(intptr_t)m_frame_tex, size)` where `size` preserves aspect ratio and fits the pane. After the Image call: `if (ImGui::IsItemHovered()) { ImVec2 mn = ImGui::GetItemRectMin(); ImVec2 sz = ImGui::GetItemRectSize(); ImVec2 m = ImGui::GetMousePos(); float u = (m.x-mn.x)/sz.x, v=(m.y-mn.y)/sz.y; int px = clamp(u*width), py = clamp(v*height); ... }` → `pixel_index = py*width + px`.

- [ ] **Step 3: Pixel history on hover**

For the hovered pixel, call `fi_colorhist_pixel_history(&cap->hist[gen], pixel_index, touches, N)` → list each `{event_id, before, after}` (the events that changed that pixel — i.e. the draws that touched it). Show as a tooltip: the pixel coord, and the list of touching event indices with their kind + colour swatch (before→after). Clicking the image sets `m_selected_event` to the TOPMOST (last) touching event — the "inspect element" pick. Note: this attributes via colour-change history (the guarantee model) — a draw that wrote the same colour won't appear; state this in a small "(?)" HelpMarker.

- [ ] **Step 4: Build + manual check (human) + commit**

Manual check: the frozen frame shows; hovering shows pixel coords + the list of events that touched that pixel with colour swatches; clicking selects the topmost draw. Commit: `frameinspect(ui): Frozen frame image, hover-to-pixel, colour-change history`.

---

### Task 4: Plan-1 calltree read API + detail tabs (Origin / Methods / State / Resources / Pixels)

**Files:**
- Modify: `xemu-frameinspect.c/.h` (new node-lookup API)
- Modify: `ui/xui/frame-inspector.cc/.hh`

**Interfaces:**
- Produces (Plan-1 API): in `xemu-frameinspect.h`, declare
  ```c
  typedef struct { uint32_t parent; uint32_t call_site; uint32_t callee;
                   uint32_t args[6]; bool valid; } FINodeInfo;
  FINodeInfo xemu_frameinspect_node_info(uint32_t node_id);
  ```
  Implement in `xemu-frameinspect.c` against the live `FICallTree` (the module already owns it): return the node's parent/call_site/callee + first argset's 6 dwords (or zeros), `valid=false` if not alive or node_id out of range. This reads the CURRENT tree (valid until the next arm) — document that Origin resolution is best-effort and only meaningful before re-arming. Read the current calltree structure in `xemu-frameinspect-calltree.h` / how Plan-1 stores it to implement correctly.
- Consumes (UI): `cap->methods` (`FIMethodLog`: `recs[]` = `{method, subchannel, confidence, param, phys_addr, writer_node}`, `batches[]` = `{batch_event, first_rec, rec_count}`), `cap->resources` (`FIResource { kind, len, off, meta, hash }`, bytes at `resources.blob+off`), the register resource (kind `FI_RESK_REGS`), the batch→resource refs (`cap->batch_res[]` = `{event, res_id}`).

- [ ] **Step 1: Plan-1 node-info API**

Add + implement `xemu_frameinspect_node_info`. Build, and add a tiny sanity check in the report (node 0 = root). This is C; no UI yet.

- [ ] **Step 2: Tab bar in the detail pane**

Replace Task 2's raw-field detail with `ImGui::BeginTabBar` + tabs, shown when the selected event is a batch (or any event): **Origin**, **Methods**, **State**, **Resources**, **Pixels**. For non-batch events show the applicable subset.

- [ ] **Step 3: Methods tab**

Find the `FIMethodBatch` whose `batch_event == m_selected_event` (linear scan `cap->methods.batches`). List its records `recs[first_rec .. first_rec+rec_count)` in a monospace table: method (name via a decode helper or hex; `FI_METHOD_RAW_WORD` → "(raw dword)"), subchannel, param (hex), phys_addr (hex, copyable), and a confidence tag (attributed/partial/unattributed — TextColored). An `ImGuiTextFilter` box filters by method/hex. If no batch matches (split-batch event, or clear/blit), say "no method records for this event".

- [ ] **Step 4: Origin tab**

For the selected batch, take the FIRST attributed method record (or let the user pick a record in the Methods tab and share selection). Its `writer_node` is a calltree node id. Walk the chain: `FINodeInfo n = xemu_frameinspect_node_info(writer_node); while (n.valid && node != ROOT) { render call_site→callee + args; node = n.parent; n = node_info(node); }`. Render as an indented list (root at bottom or top — pick, label direction) with hex addresses + a copy button per node (`ImGui::SmallButton("copy")` → `ImGui::SetClipboardText`). If the node is invalid (tree freed by a re-arm, or unattributed), show "origin unavailable (unattributed or capture re-armed)". Add a HelpMarker explaining origins resolve against the live calltree.

- [ ] **Step 5: State tab**

Find the `FI_RESK_REGS` resource referenced by this batch (via `cap->batch_res` where `event == m_selected_event`, then the resource whose `kind==FI_RESK_REGS`). Render the 0x2000-word register file as a monospace hex grid (16 cols), OR a filtered list. v1: a scrollable hex/word dump with an offset filter. (Decoding named registers is a nice-to-have; raw dump is acceptable for v1 — note it.)

- [ ] **Step 6: Resources tab**

List the batch's resource refs. For each: `FI_RESK_TEXTURE`/`FI_RESK_PALETTE` — show kind, len, meta (format/dims packed — decode from the pack used in 2B Task 5), and for textures attempt an inline preview: this requires decoding the raw texture bytes to RGBA (the swizzle/format decode is complex — v1 may show a hex/summary instead of a decoded image, and note decoded-preview as a follow-up). `FI_RESK_TEXTURE_RTREF` — show "render-target texture → surface @ 0x%x (meta)" as a dependency hop (no bytes). `FI_RESK_REGS` — link to the State tab.

- [ ] **Step 7: Pixels tab**

If a pixel is pinned (from Task 3 hover-click, store `m_pinned_pixel`), show its full colour history (`fi_colorhist_pixel_history` on the relevant gen) as a table of `{event, before→after}` with colour swatches. Else "hover + click a pixel to pin its history".

- [ ] **Step 8: Build + manual check (human) + commit**

Manual: tabs populate for a selected batch; Origin shows a call chain with addresses (or "unavailable"); Methods lists records with confidence; State shows the reg dump; Resources lists textures/RTREF; Pixels shows the pinned pixel's history. Commit: `frameinspect(ui): Detail tabs — Origin/Methods/State/Resources/Pixels`.

---

### Task 5: Timeline scrubber

**Files:**
- Modify: `ui/xui/frame-inspector.cc/.hh`

**Interfaces:**
- Consumes: `fi_colorhist_reconstruct(&cap->hist[gen], event_index, out)` for the scanout (or selected) generation.

- [ ] **Step 1: Scrubber**

Add an `ImGui::SliderInt("timeline", &m_timeline_idx, 0, num_events-1)` above/below the image. When `m_timeline_idx` changes, re-reconstruct the scanout gen's image at that event index into the frame texture (reuse Task 3's upload path). Show "event N / M: <kind>". A "latest" button resets to the final event. This lets the user watch the frame build up. Note: only the scanout gen (or a chosen gen) is reconstructable; offscreen gens need a per-surface timeline (v2 — note it).

- [ ] **Step 2: Build + manual check (human) + commit**

Manual: dragging the timeline shows the frame at earlier draws. Commit: `frameinspect(ui): Timeline scrubber (reconstruct frame at any event)`.

---

### Task 6: Address lookup panel

**Files:**
- Modify: `ui/xui/frame-inspector.cc/.hh`

**Interfaces:**
- Consumes: `xemu_frameinspect_lookup_tag(uint64_t paddr)`, `FI_TAG_PARTIAL`/`FI_TAG_NODE` (from `xemu-frameinspect-tagmap.h`), `xemu_frameinspect_node_info` (Task 4).

- [ ] **Step 1: Panel**

A collapsible section (or a second tab / window region): an `ImGui::InputText` hex address field; on submit, `tag = lookup_tag(addr)`; show the confidence (unattributed / partial / attributed) and, if attributed, walk `FI_TAG_NODE(tag)` through `node_info` to render the writer call chain — the manual hop for chasing staged data. Note this resolves against the live tag map (valid until re-arm), same caveat as Origin.

- [ ] **Step 2: Build + manual check (human) + commit**

Manual: entering a guest address shows who wrote it (call chain) or "unattributed". Commit: `frameinspect(ui): Address lookup panel (who wrote address X)`.

---

## Self-review checklist (run after writing, before execution)

- **Spec coverage:** window opens on publish (T1), event list + selection (T2), frozen frame + hover→pixel + colour-change history (T3), Origin/Methods/State/Resources/Pixels tabs (T4), timeline (T5), address lookup (T6). Watch panel is DEFERRED (the Plan-1 watch engine is still latent — M2/M3/M4 findings pending; out of scope for this UI plan, note it).
- **Acquire/release:** every Draw() path releases the capture. GL textures owned by the window, recreated per capture, deleted on window teardown.
- **Honest limitations:** RAW_WORD, RTREF, PVIDEO, blit best-effort, split-batch empty, no-zeta — each surfaced in the relevant tab, never hidden.
- **Placeholder scan:** UI tasks give exact xui patterns (ImGui::Image/Selectable/TabBar/TreeNode from the codebase map) + the exact capture field names to verify against the on-disk structs; the implementer confirms field names before use (as in prior plans).

## Out of scope (this plan)

- **Watch panel + re-capture flow** — the Plan-1 watch engine is latent (no caller wires `watch_add`; M2/M3/M4 findings open). A separate plan wires watches end-to-end (arm-with-watches → deep per-invocation records → UI). 
- Decoded texture previews (swizzle/S3TC decode to RGBA) — v1 shows metadata/hex; decoded inline previews are a follow-up.
- Named-register decoding in the State tab (raw word dump in v1).
- Per-offscreen-surface timelines (only the scanout/selected gen is scrubbable in v1).
- Vulkan renderer (GL only, as the whole feature).

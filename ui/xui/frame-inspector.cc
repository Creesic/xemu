//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "common.hh"
#include "frame-inspector.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "widgets.hh"
#include <vector>
extern "C" {
#include "../../xemu-frameinspect-capture.h"
#include "../../xemu-frameinspect.h"
}

FrameInspectorWindow frame_inspector_window;

static const char *ev_kind_name(uint8_t k)
{
    switch (k) {
    case FI_EV_METHOD: return "METHOD";
    case FI_EV_BATCH: return "BATCH";
    case FI_EV_CLEAR: return "CLEAR";
    case FI_EV_BLIT: return "BLIT";
    case FI_EV_UPLOAD: return "UPLOAD";
    case FI_EV_SCANOUT: return "SCANOUT";
    default: return "?";
    }
}

/* Colour history pixels are RGBA8888 as produced by
 * pgraph_gl_fi_readback_surface (glReadPixels(GL_RGBA, GL_UNSIGNED_BYTE)):
 * byte 0 = R, byte 1 = G, byte 2 = B, byte 3 = A. Read back as a little-
 * endian uint32 that is therefore 0xAABBGGRR, i.e. R is the low byte. */
static ImVec4 fi_rgba_to_imvec4(uint32_t c)
{
    return ImVec4(((c >> 0) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f,
                  ((c >> 16) & 0xff) / 255.0f, ((c >> 24) & 0xff) / 255.0f);
}

/* Returns the surface generation displayed at scanout time, or -1 if there
 * is no FI_EV_SCANOUT event in this capture, or the scanout surface could
 * not be resolved (FI_SURFGEN_INVALID). */
static int fi_find_scanout_gen(const FICapture *cap)
{
    for (uint32_t i = 0; i < cap->events.count; i++) {
        const FIEvent *ev = &cap->events.events[i];
        if (ev->kind == FI_EV_SCANOUT && ev->surface_gen != FI_SURFGEN_INVALID) {
            return (int)ev->surface_gen;
        }
    }
    return -1;
}

static const char *fi_confidence_name(uint16_t confidence)
{
    switch (confidence) {
    case FI_ORIG_ATTRIBUTED: return "attributed";
    case FI_ORIG_PARTIAL: return "partial";
    case FI_ORIG_UNATTRIBUTED: return "unattributed";
    case FI_ORIG_LOSTSYNC: return "lostsync";
    default: return "?";
    }
}

/* Linear scan for the FIMethodBatch whose batch_event == event_idx (the
 * join key: FIMethodBatch.batch_event references the begin_batch event
 * index). Returns NULL for non-batch events (clear/blit/scanout) or a
 * split-batch event that logged no methods of its own. */
static const FIMethodBatch *fi_find_method_batch(const FICapture *cap,
                                                 int event_idx)
{
    if (event_idx < 0) {
        return nullptr;
    }
    for (uint32_t i = 0; i < cap->methods.num_batches; i++) {
        if (cap->methods.batches[i].batch_event == (uint32_t)event_idx) {
            return &cap->methods.batches[i];
        }
    }
    return nullptr;
}

/* First resource of `kind` referenced by event_idx's batch (join via
 * cap->batch_res, keyed the same way as FIMethodBatch.batch_event). */
static const FIResource *fi_find_batch_resource(const FICapture *cap,
                                                int event_idx, uint32_t kind)
{
    if (event_idx < 0) {
        return nullptr;
    }
    for (uint32_t i = 0; i < cap->num_batch_res; i++) {
        const FIBatchResRef *ref = &cap->batch_res[i];
        if (ref->event != (uint32_t)event_idx) {
            continue;
        }
        if (ref->res_id >= cap->resources.num_res) {
            continue;
        }
        if (cap->resources.res[ref->res_id].kind == kind) {
            return &cap->resources.res[ref->res_id];
        }
    }
    return nullptr;
}

static void fi_methods_tab(FrameInspectorWindow *w, const FICapture *cap,
                          const FIMethodBatch *batch)
{
    if (!batch) {
        ImGui::TextDisabled(
            "No method records for this event (clear/blit/split-batch).");
        return;
    }

    w->m_method_filter.Draw("Filter", 200.0f * g_viewport_mgr.m_scale);

    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("fi_methods_tbl", 5, flags,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("Method");
        ImGui::TableSetupColumn("Sub");
        ImGui::TableSetupColumn("Param");
        ImGui::TableSetupColumn("Phys Addr");
        ImGui::TableSetupColumn("Confidence");
        ImGui::TableHeadersRow();

        for (uint32_t i = batch->first_rec;
             i < batch->first_rec + batch->rec_count; i++) {
            const FIMethodRec *r = &cap->methods.recs[i];
            char method_str[24];
            if (r->method == FI_METHOD_RAW_WORD) {
                snprintf(method_str, sizeof(method_str), "(raw dword)");
            } else {
                snprintf(method_str, sizeof(method_str), "0x%04x", r->method);
            }
            const char *conf_str = fi_confidence_name(r->confidence);

            char filter_row[160];
            snprintf(filter_row, sizeof(filter_row),
                    "%s %u 0x%08x 0x%08x %s", method_str, r->subchannel,
                    r->param, r->phys_addr, conf_str);
            if (!w->m_method_filter.PassFilter(filter_row)) {
                continue;
            }

            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(method_str, (int)i == w->m_selected_rec,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                w->m_selected_rec = (int)i;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", r->subchannel);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("0x%08x", r->param);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("0x%08x", r->phys_addr);
            ImGui::SameLine();
            if (ImGui::SmallButton("copy")) {
                char hex[16];
                snprintf(hex, sizeof(hex), "0x%08x", r->phys_addr);
                ImGui::SetClipboardText(hex);
            }
            ImGui::TableSetColumnIndex(4);
            ImVec4 color;
            switch (r->confidence) {
            case FI_ORIG_ATTRIBUTED: color = ImVec4(0.3f, 0.9f, 0.3f, 1); break;
            case FI_ORIG_PARTIAL: color = ImVec4(0.9f, 0.9f, 0.2f, 1); break;
            default: color = ImVec4(0.6f, 0.6f, 0.6f, 1); break;
            }
            ImGui::TextColored(color, "%s", conf_str);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
}

/* Walks the call chain from the selected Methods-tab record's writer_node up
 * to the root, one row per frame (innermost/writer first). Resolves each
 * node via xemu_frameinspect_node_info(), which reads the LIVE Plan-1 call
 * tree -- valid only until the inspector's capture is re-armed. */
static void fi_origin_tab(const FICapture *cap, int selected_rec)
{
    ImGui::TextUnformatted("Call-path origin");
    ImGui::SameLine();
    HelpMarker("Origin resolves against the live call tree; only valid "
              "before re-arming the inspector.");
    ImGui::Separator();

    if (selected_rec < 0 || (uint32_t)selected_rec >= cap->methods.num_recs) {
        ImGui::TextDisabled("Origin unavailable (unattributed).");
        return;
    }
    const FIMethodRec *rec = &cap->methods.recs[selected_rec];
    if (rec->confidence != FI_ORIG_ATTRIBUTED) {
        ImGui::TextDisabled("Origin unavailable (unattributed).");
        return;
    }

    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    uint32_t node = rec->writer_node;
    if (node == FI_NODE_ROOT) {
        ImGui::TextDisabled(
            "Origin is the root frame (call path not tracked further).");
    }
    int indents = 0;
    int depth = 0;
    while (node != FI_NODE_ROOT && node != FI_NODE_INVALID) {
        FINodeInfo ni = xemu_frameinspect_node_info(node);
        if (!ni.valid) {
            ImGui::TextColored(ImVec4(1, 0.6f, 0, 1),
                              "origin unavailable (capture re-armed)");
            break;
        }
        ImGui::PushID(depth);
        ImGui::Text("call_site 0x%08x -> callee 0x%08x", ni.call_site,
                   ni.callee);
        ImGui::Text("ecx/this=0x%08x  args: %08x %08x %08x %08x %08x",
                   ni.args[0], ni.args[1], ni.args[2], ni.args[3],
                   ni.args[4], ni.args[5]);
        ImGui::SameLine();
        if (ImGui::SmallButton("copy callee")) {
            char hex[16];
            snprintf(hex, sizeof(hex), "0x%08x", ni.callee);
            ImGui::SetClipboardText(hex);
        }
        ImGui::PopID();
        ImGui::Indent();
        indents++;
        node = ni.parent;
        depth++;
    }
    for (int i = 0; i < indents; i++) {
        ImGui::Unindent();
    }
    ImGui::PopFont();
}

static void fi_state_tab(const FICapture *cap, int event_idx)
{
    const FIResource *regs = fi_find_batch_resource(cap, event_idx, FI_RESK_REGS);
    if (!regs) {
        ImGui::TextDisabled("No register-file resource for this event.");
        return;
    }
    ImGui::Text("Registers: %u bytes (raw dword dump; named-register "
               "decoding is out of scope)", regs->len);
    ImGui::BeginChild("fi_state_regs", ImVec2(0, 0), true);
    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    const uint8_t *base = cap->resources.blob + regs->off;
    uint32_t nwords = regs->len / 4;
    uint32_t nrows = (nwords + 7) / 8;
    ImGuiListClipper clipper;
    clipper.Begin((int)nrows);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            uint32_t first = (uint32_t)row * 8;
            char line[128];
            int p = snprintf(line, sizeof(line), "0x%04x:", first * 4);
            for (uint32_t k = 0; k < 8 && first + k < nwords; k++) {
                uint32_t word;
                memcpy(&word, base + (uint64_t)(first + k) * 4, 4);
                p += snprintf(line + p, sizeof(line) - p, " %08x", word);
            }
            ImGui::TextUnformatted(line);
        }
    }
    ImGui::PopFont();
    ImGui::EndChild();
}

static void fi_resources_tab(const FICapture *cap, int event_idx)
{
    ImGui::TextDisabled(
        "Texture/palette bytes are shown as metadata only; decoded inline "
        "previews are a follow-up.");
    ImGui::Separator();

    if (event_idx < 0) {
        ImGui::TextDisabled("Select an event.");
        return;
    }
    bool any = false;
    for (uint32_t i = 0; i < cap->num_batch_res; i++) {
        const FIBatchResRef *ref = &cap->batch_res[i];
        if (ref->event != (uint32_t)event_idx) {
            continue;
        }
        if (ref->res_id >= cap->resources.num_res) {
            continue;
        }
        const FIResource *r = &cap->resources.res[ref->res_id];
        any = true;
        ImGui::PushID((int)i);
        switch (r->kind) {
        case FI_RESK_REGS:
            ImGui::TextUnformatted("Registers (32 KiB) -- see State tab");
            break;
        case FI_RESK_TEXTURE: {
            /* meta pack from pgraph_gl_bind_textures() (draw.c): color_format
             * in the high 32 bits, width in bits 16-31, height in bits 0-15. */
            uint32_t color_format = (uint32_t)(r->meta >> 32);
            uint32_t width = (uint32_t)((r->meta >> 16) & 0xFFFF);
            uint32_t height = (uint32_t)(r->meta & 0xFFFF);
            ImGui::Text("Texture: %u bytes, format=0x%02x %ux%u", r->len,
                       color_format, width, height);
            break;
        }
        case FI_RESK_PALETTE:
            ImGui::Text("Palette: %u bytes", r->len);
            break;
        case FI_RESK_TEXTURE_RTREF:
            ImGui::TextColored(
                ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                "Render-target texture -> surface @ 0x%llx (dependency)",
                (unsigned long long)r->meta);
            break;
        default:
            ImGui::Text("Unknown resource kind %u, %u bytes", r->kind, r->len);
            break;
        }
        ImGui::PopID();
    }
    if (!any) {
        ImGui::TextDisabled("No resources referenced by this event.");
    }
}

static void fi_pixels_tab(const FICapture *cap, int pinned_pixel)
{
    if (pinned_pixel < 0) {
        ImGui::TextDisabled(
            "Hover + click a pixel in the frame to pin its history.");
        return;
    }
    int gen = fi_find_scanout_gen(cap);
    if (gen < 0 || !cap->hist || (uint32_t)gen >= cap->hist_count) {
        ImGui::TextDisabled("No colour history available for the scanout surface.");
        return;
    }
    const FIColorHist *ch = &cap->hist[gen];
    if ((uint32_t)pinned_pixel >= ch->npix) {
        ImGui::TextDisabled("Pinned pixel is out of range for this capture.");
        return;
    }

    FIColorTouch touches[128];
    int n = fi_colorhist_pixel_history(ch, (uint32_t)pinned_pixel, touches,
                                       128);
    ImGui::Text("Pixel index %d: %d touch(es)", pinned_pixel, n);
    if (n == 0) {
        ImGui::TextDisabled("No colour-change history for this pixel.");
        return;
    }
    if (ImGui::BeginTable("fi_pixels_tbl", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Event");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Before -> After");
        ImGui::TableHeadersRow();
        ImVec2 sw(16.0f * g_viewport_mgr.m_scale, 16.0f * g_viewport_mgr.m_scale);
        for (int i = 0; i < n; i++) {
            const FIColorTouch *t = &touches[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("#%u", t->event_id);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(t->event_id < cap->events.count ?
                                   ev_kind_name(cap->events.events[t->event_id].kind) :
                                   "?");
            ImGui::TableSetColumnIndex(2);
            ImGui::ColorButton("before", fi_rgba_to_imvec4(t->before),
                              ImGuiColorEditFlags_NoTooltip |
                                  ImGuiColorEditFlags_NoBorder,
                              sw);
            ImGui::SameLine();
            ImGui::TextUnformatted("->");
            ImGui::SameLine();
            ImGui::ColorButton("after", fi_rgba_to_imvec4(t->after),
                              ImGuiColorEditFlags_NoTooltip |
                                  ImGuiColorEditFlags_NoBorder,
                              sw);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

/* (Re)builds m_frame_tex from cap->hist[gen] reconstructed at event_index.
 * Always deletes any existing texture first (no per-capture/per-scrub
 * leak). Leaves m_frame_tex == 0 on any failure (gen out of range, colour
 * history not initialized, event_index out of range, or reconstruct
 * failure). Shared by the initial "show final frame" build and the Task 5
 * timeline scrubber. */
void FrameInspectorWindow::UploadFrame(const FICapture *cap, int gen,
                                       uint32_t event_index)
{
    ReleaseTexture();

    if (gen < 0 || !cap->hist || (uint32_t)gen >= cap->hist_count) {
        return;
    }
    const FIColorHist *ch = &cap->hist[gen];
    if (ch->width == 0 || ch->height == 0) {
        return; /* not inited */
    }
    if (event_index >= fi_colorhist_num_events(ch)) {
        return;
    }

    std::vector<uint32_t> pixels((size_t)ch->width * ch->height);
    if (!fi_colorhist_reconstruct(ch, event_index, pixels.data())) {
        return;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)ch->width,
                (GLsizei)ch->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    m_frame_tex = tex;
    m_frame_w = (int)ch->width;
    m_frame_h = (int)ch->height;
    m_frame_gen = gen;
}

void FrameInspectorWindow::ReleaseTexture()
{
    if (m_frame_tex) {
        GLuint tex = (GLuint)m_frame_tex;
        glDeleteTextures(1, &tex);
    }
    m_frame_tex = 0;
    m_frame_w = 0;
    m_frame_h = 0;
    m_frame_gen = -1;
}

/* Timeline scrubber (Task 5): lets the user re-reconstruct the currently
 * displayed frame at any event of the scanout generation's colour history,
 * not just the final one -- watch the frame build up draw by draw. Only
 * shown when the scanout gen (m_frame_gen, set by UploadFrame on success)
 * has more than one recorded event. Note: only the scanout generation is
 * scrubbable in v1; offscreen render targets (other surfaces) would need a
 * per-surface timeline of their own (out of scope, see HelpMarker). */
static void fi_timeline_scrubber(FrameInspectorWindow *w, const FICapture *cap)
{
    if (w->m_frame_gen < 0 || (uint32_t)w->m_frame_gen >= cap->hist_count) {
        return;
    }
    const FIColorHist *ch = &cap->hist[w->m_frame_gen];
    uint32_t num_ev = fi_colorhist_num_events(ch);
    if (num_ev <= 1) {
        return; /* nothing to scrub through */
    }

    int idx = (w->m_timeline_idx < 0) ? (int)(num_ev - 1) : w->m_timeline_idx;
    if (ImGui::SliderInt("Timeline", &idx, 0, (int)num_ev - 1)) {
        w->m_timeline_idx = idx;
        w->UploadFrame(cap, w->m_frame_gen, (uint32_t)idx);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Latest")) {
        w->m_timeline_idx = -1;
        w->UploadFrame(cap, w->m_frame_gen, num_ev - 1);
    }
    ImGui::SameLine();
    HelpMarker("Only the scanout generation is scrubbable in v1; offscreen "
              "render targets would need a per-surface timeline of their "
              "own (out of scope).");

    uint32_t shown =
        (w->m_timeline_idx < 0) ? num_ev - 1 : (uint32_t)w->m_timeline_idx;
    const char *kind = shown < cap->events.count ?
                       ev_kind_name(cap->events.events[shown].kind) : "?";
    ImGui::Text("event %u / %u: %s", shown, num_ev - 1, kind);
}

void FrameInspectorWindow::Draw()
{
    const FICapture *cap = xemu_frameinspect_capture_acquire();
    if (!cap) {
        return; /* nothing published */
    }

    /* Auto-open once when a new capture publishes. */
    bool new_capture = cap->events.count != m_last_seen_events;
    if (new_capture) {
        m_last_seen_events = cap->events.count;
        m_is_open = true;
    }

    if (!m_is_open) {
        xemu_frameinspect_capture_release(cap);
        return;
    }

    /* A new/smaller capture may have fewer events than the prior selection. */
    if (m_selected_event >= (int)cap->events.count) {
        m_selected_event = -1;
    }

    if (new_capture) {
        m_pinned_pixel = -1; /* stale index into a possibly-differently-sized image */
        m_selected_rec = -1; /* stale index into the old capture's methods log */
        m_timeline_idx = -1; /* show the final frame first on a new capture */
    }
    if (new_capture || m_frame_tex == 0) {
        int gen = fi_find_scanout_gen(cap);
        uint32_t num_ev = (gen >= 0 && cap->hist && (uint32_t)gen < cap->hist_count) ?
                          fi_colorhist_num_events(&cap->hist[gen]) :
                          0;
        bool have_idx = m_timeline_idx >= 0 && num_ev > 0 &&
                        (uint32_t)m_timeline_idx < num_ev;
        uint32_t idx = have_idx ? (uint32_t)m_timeline_idx :
                                  (num_ev > 0 ? num_ev - 1 : 0);
        UploadFrame(cap, gen, idx);
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
        if (cap->truncated || cap->events.truncated ||
            cap->surfaces.truncated || cap->resources.truncated ||
            cap->methods.truncated) {
            ImGui::TextColored(
                ImVec4(1, 0.6f, 0, 1),
                "[TRUNCATED] capture hit a cap/budget; some data is missing.");
        }
        ImGui::BeginChild("fi_events", ImVec2(260.0f * g_viewport_mgr.m_scale, 0),
                          true);
        for (uint32_t i = 0; i < cap->events.count; i++) {
            const FIEvent *ev = &cap->events.events[i];
            ImGui::PushID((int)i);
            char label[64];
            snprintf(label, sizeof(label), "#%u %s gen=%u", i,
                     ev_kind_name(ev->kind), ev->surface_gen);
            bool is_special =
                ev->kind == FI_EV_SCANOUT || ev->kind == FI_EV_BLIT;
            if (is_special) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
            }
            if (ImGui::Selectable(label, (int)i == m_selected_event)) {
                m_selected_event = (int)i;
            }
            if (is_special) {
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("fi_detail", ImVec2(0, 0), true);
        if (m_frame_tex && m_frame_w > 0 && m_frame_h > 0) {
            float avail_w = ImGui::GetContentRegionAvail().x;
            float img_scale = avail_w / (float)m_frame_w;
            ImVec2 img_size(avail_w, m_frame_h * img_scale);
            ImGui::Image((ImTextureID)(intptr_t)m_frame_tex, img_size);
            bool img_hovered = ImGui::IsItemHovered();
            bool img_clicked = img_hovered && ImGui::IsItemClicked();
            if (img_hovered && m_frame_gen >= 0 &&
                (uint32_t)m_frame_gen < cap->hist_count) {
                ImVec2 mn = ImGui::GetItemRectMin();
                ImVec2 sz = ImGui::GetItemRectSize();
                ImVec2 mp = ImGui::GetMousePos();
                float u = ImClamp((mp.x - mn.x) / sz.x, 0.0f, 0.9999f);
                float v = ImClamp((mp.y - mn.y) / sz.y, 0.0f, 0.9999f);
                int px = (int)(u * m_frame_w);
                int py = (int)(v * m_frame_h);
                uint32_t pixel_index = (uint32_t)py * (uint32_t)m_frame_w +
                                       (uint32_t)px;

                const FIColorHist *ch = &cap->hist[m_frame_gen];
                FIColorTouch touches[16];
                int n = fi_colorhist_pixel_history(ch, pixel_index, touches,
                                                   16);

                ImGui::BeginTooltip();
                ImGui::Text("px (%d, %d)", px, py);
                ImGui::SameLine();
                HelpMarker(
                    "Shows draws that CHANGED this pixel's colour "
                    "(colour-change history); a draw that writes the same "
                    "colour it already had will not appear here.");
                if (n == 0) {
                    ImGui::TextDisabled("no colour-change history for this pixel");
                } else {
                    for (int i = 0; i < n; i++) {
                        const FIColorTouch *t = &touches[i];
                        const char *kind = "?";
                        if (t->event_id < cap->events.count) {
                            kind = ev_kind_name(
                                cap->events.events[t->event_id].kind);
                        }
                        ImGui::PushID(i);
                        ImGui::Text("#%u %s", t->event_id, kind);
                        ImGui::SameLine();
                        ImVec2 sw(16.0f * g_viewport_mgr.m_scale,
                                  16.0f * g_viewport_mgr.m_scale);
                        ImGui::ColorButton(
                            "before", fi_rgba_to_imvec4(t->before),
                            ImGuiColorEditFlags_NoTooltip |
                                ImGuiColorEditFlags_NoBorder,
                            sw);
                        ImGui::SameLine();
                        ImGui::TextUnformatted("->");
                        ImGui::SameLine();
                        ImGui::ColorButton(
                            "after", fi_rgba_to_imvec4(t->after),
                            ImGuiColorEditFlags_NoTooltip |
                                ImGuiColorEditFlags_NoBorder,
                            sw);
                        ImGui::PopID();
                    }
                }
                ImGui::EndTooltip();

                if (img_clicked) {
                    if (n > 0) {
                        m_selected_event = (int)touches[n - 1].event_id;
                    }
                    m_pinned_pixel = (int)pixel_index;
                }
            }
            fi_timeline_scrubber(this, cap);
            ImGui::Separator();
        } else {
            ImGui::TextDisabled(
                fi_find_scanout_gen(cap) < 0 ?
                    "no image (no scanout event in this capture)" :
                    "no image (scanout surface has no colour history)");
            ImGui::Separator();
        }
        if (m_selected_event >= 0 &&
            m_selected_event < (int)cap->events.count) {
            /* Anchor the Methods-tab selection to the current event's
             * batch, defaulting to its first ATTRIBUTED record whenever the
             * selection doesn't belong to it (new event/capture, no batch,
             * or first display) -- an in-range selection means the user
             * already picked a row in the Methods tab, so leave it alone. */
            const FIMethodBatch *cur_batch =
                fi_find_method_batch(cap, m_selected_event);
            bool rec_in_range =
                cur_batch && m_selected_rec >= (int)cur_batch->first_rec &&
                m_selected_rec <
                    (int)(cur_batch->first_rec + cur_batch->rec_count);
            if (!rec_in_range) {
                m_selected_rec = -1;
                if (cur_batch) {
                    for (uint32_t i = cur_batch->first_rec;
                         i < cur_batch->first_rec + cur_batch->rec_count;
                         i++) {
                        if (cap->methods.recs[i].confidence ==
                            FI_ORIG_ATTRIBUTED) {
                            m_selected_rec = (int)i;
                            break;
                        }
                    }
                    if (m_selected_rec < 0 && cur_batch->rec_count > 0) {
                        m_selected_rec = (int)cur_batch->first_rec;
                    }
                }
            }

            if (ImGui::BeginTabBar("fi_tabs")) {
                if (ImGui::BeginTabItem("Origin")) {
                    fi_origin_tab(cap, m_selected_rec);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Methods")) {
                    fi_methods_tab(this, cap, cur_batch);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("State")) {
                    fi_state_tab(cap, m_selected_event);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Resources")) {
                    fi_resources_tab(cap, m_selected_event);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Pixels")) {
                    fi_pixels_tab(cap, m_pinned_pixel);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        } else {
            ImGui::TextDisabled("Select an event");
        }
        ImGui::EndChild();
    }
    ImGui::End();

    xemu_frameinspect_capture_release(cap);
}

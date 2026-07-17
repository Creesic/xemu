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

/* (Re)build m_frame_tex from the scanout generation's final reconstructed
 * image. Always deletes any existing texture first (no per-capture leak).
 * Leaves m_frame_tex == 0 on any failure (no scanout event, gen out of
 * range, colour history not initialized, or reconstruct failure). */
static void fi_rebuild_frame_texture(FrameInspectorWindow *w,
                                     const FICapture *cap)
{
    w->ReleaseTexture();

    int gen = fi_find_scanout_gen(cap);
    if (gen < 0 || !cap->hist || (uint32_t)gen >= cap->hist_count) {
        return;
    }
    const FIColorHist *ch = &cap->hist[gen];
    if (ch->width == 0 || ch->height == 0) {
        return; /* not inited */
    }
    uint32_t num_ev = fi_colorhist_num_events(ch);
    if (num_ev == 0) {
        return;
    }

    std::vector<uint32_t> pixels((size_t)ch->width * ch->height);
    if (!fi_colorhist_reconstruct(ch, num_ev - 1, pixels.data())) {
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

    w->m_frame_tex = tex;
    w->m_frame_w = (int)ch->width;
    w->m_frame_h = (int)ch->height;
    w->m_frame_gen = gen;
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
    }
    if (new_capture || m_frame_tex == 0) {
        fi_rebuild_frame_texture(this, cap);
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
            const FIEvent *ev = &cap->events.events[m_selected_event];
            ImGui::PushFont(g_font_mgr.m_fixed_width_font);
            ImGui::Text("kind:       %s", ev_kind_name(ev->kind));
            ImGui::Text("surface_gen: %u", ev->surface_gen);
            ImGui::Text("seq:        %u", ev->seq);
            ImGui::Text("a0:         0x%08x", ev->a0);
            ImGui::Text("a1:         0x%08x", ev->a1);
            ImGui::Text("a2:         0x%08x", ev->a2);
            ImGui::Text("a3:         0x%08x", ev->a3);
            ImGui::PopFont();
        } else {
            ImGui::TextDisabled("Select an event");
        }
        ImGui::EndChild();
    }
    ImGui::End();

    xemu_frameinspect_capture_release(cap);
}

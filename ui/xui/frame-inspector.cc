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

    /* A new/smaller capture may have fewer events than the prior selection. */
    if (m_selected_event >= (int)cap->events.count) {
        m_selected_event = -1;
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

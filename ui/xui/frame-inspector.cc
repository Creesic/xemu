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
extern "C" {
#include "../../xemu-frameinspect-capture.h"
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
        if (cap->truncated || cap->events.truncated ||
            cap->surfaces.truncated || cap->resources.truncated ||
            cap->methods.truncated) {
            ImGui::TextColored(
                ImVec4(1, 0.6f, 0, 1),
                "[TRUNCATED] capture hit a cap/budget; some data is missing.");
        }
        /* Task 2+ fill the rest. */
    }
    ImGui::End();

    xemu_frameinspect_capture_release(cap);
}

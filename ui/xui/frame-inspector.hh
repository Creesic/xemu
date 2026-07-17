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
#pragma once
#include "common.hh"

struct FICapture;

class FrameInspectorWindow
{
public:
    bool m_is_open = false;
    /* Identity of the last-seen published capture, used to detect a new
     * capture (see Draw()). Compared by pointer, not by event count: two
     * captures of the same scene can have identical event counts, so an
     * event-count comparison can miss a re-capture and leave stale
     * frame/selection state on screen. */
    const FICapture *m_last_seen_cap = nullptr;
    int m_selected_event = -1;
    /* Selected FIMethodRec index in the Methods tab (a global index into
     * cap->methods.recs), shared with the Origin tab. -1 = none. Reset to
     * the current batch's first ATTRIBUTED record whenever it falls outside
     * that batch's record range (new event, new capture, or no batch). */
    int m_selected_rec = -1;
    ImGuiTextFilter m_method_filter;

    /* Frozen-frame GL texture (Task 3): reconstructed final scanout image.
     * m_frame_tex == 0 means no image is currently held. Deleted+recreated
     * on each new capture (see Draw()); note there is no window dtor here,
     * so a texture leaked on emulator exit is acceptable (single alloc). */
    unsigned m_frame_tex = 0;
    int m_frame_w = 0, m_frame_h = 0;
    int m_frame_gen = -1;
    /* Pixel pinned by clicking the frame image; consumed by the Pixels tab
     * added in Task 4. */
    int m_pinned_pixel = -1;

    /* Timeline scrubber (Task 5): event index within the scanout gen's
     * colour history that m_frame_tex currently shows. -1 = latest/final
     * event (the default). Reset to -1 whenever a new capture publishes. */
    int m_timeline_idx = -1;

    /* Address Lookup panel (Task 6): hex input buffer (digits only -- see
     * ImGuiInputTextFlags_CharsHexadecimal) and the last lookup's result.
     * m_lookup_tag/node_info() read module globals (the live tag map / call
     * tree), not this capture's snapshot, so this state is not reset on a
     * new capture the way the other per-capture fields above are. */
    char m_lookup_addr[19] = "";
    uint64_t m_lookup_result_addr = 0;
    uint32_t m_lookup_tag = 0;
    bool m_lookup_done = false;

    void Draw();
    void ReleaseTexture();
    /* (Re)builds m_frame_tex from cap->hist[gen] reconstructed at
     * event_index. Reused by the initial "show final frame" path and by the
     * Task 5 timeline scrubber to re-reconstruct at an arbitrary event.
     * Deletes any existing texture first; leaves m_frame_tex == 0 on
     * failure (gen invalid, hist not inited, event_index out of range, or
     * reconstruct failure). */
    void UploadFrame(const struct FICapture *cap, int gen,
                     uint32_t event_index);
};

extern FrameInspectorWindow frame_inspector_window;

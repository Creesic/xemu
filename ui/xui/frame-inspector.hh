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

class FrameInspectorWindow
{
public:
    bool m_is_open = false;
    unsigned m_last_seen_events = 0;
    int m_selected_event = -1;

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

    void Draw();
    void ReleaseTexture();
};

extern FrameInspectorWindow frame_inspector_window;

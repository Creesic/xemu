/*
 * Geforce NV2A PGRAPH OpenGL Surface-Texture Compatibility
 *
 * Copyright (c) 2026 xemu Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"

#include "hw/xbox/nv2a/nv2a_regs.h"
#include "surface-texture-compat.h"

bool pgraph_gl_zeta_to_y16_compatible(
    const PGRAPHGLSurfaceTextureLayout *layout)
{
    uint64_t surface_row_bytes =
        (uint64_t)layout->surface_width *
        layout->surface_bytes_per_pixel;
    uint64_t texture_row_bytes =
        (uint64_t)layout->texture_width *
        layout->texture_bytes_per_pixel;

    return !layout->surface_color &&
           !layout->surface_swizzled &&
           layout->texture_linear &&
           !layout->texture_cubemap &&
           layout->texture_levels == 1 &&
           layout->texture_color_format ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16 &&
           layout->surface_height == layout->texture_height &&
           layout->surface_pitch == layout->texture_pitch &&
           surface_row_bytes == texture_row_bytes;
}

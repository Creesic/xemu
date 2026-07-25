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

#ifndef HW_XBOX_NV2A_PGRAPH_GL_SURFACE_TEXTURE_COMPAT_H
#define HW_XBOX_NV2A_PGRAPH_GL_SURFACE_TEXTURE_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct PGRAPHGLSurfaceTextureLayout {
    bool surface_color;
    bool surface_swizzled;
    uint32_t surface_width;
    uint32_t surface_height;
    uint32_t surface_pitch;
    uint32_t surface_bytes_per_pixel;
    bool texture_linear;
    bool texture_cubemap;
    uint32_t texture_levels;
    uint32_t texture_color_format;
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t texture_pitch;
    uint32_t texture_bytes_per_pixel;
} PGRAPHGLSurfaceTextureLayout;

bool pgraph_gl_zeta_to_y16_compatible(
    const PGRAPHGLSurfaceTextureLayout *layout);

#endif

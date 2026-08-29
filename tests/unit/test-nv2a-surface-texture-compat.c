/*
 * NV2A OpenGL surface-texture compatibility tests
 *
 * Copyright (c) 2026 xemu Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/gl/surface-texture-compat.h"

static PGRAPHGLSurfaceTextureLayout beetle_layout(void)
{
    return (PGRAPHGLSurfaceTextureLayout) {
        .surface_color = false,
        .surface_swizzled = false,
        .surface_width = 640,
        .surface_height = 480,
        .surface_pitch = 2560,
        .surface_bytes_per_pixel = 4,
        .texture_linear = true,
        .texture_cubemap = false,
        .texture_levels = 1,
        .texture_color_format =
            NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y16,
        .texture_width = 1280,
        .texture_height = 480,
        .texture_pitch = 2560,
        .texture_bytes_per_pixel = 2,
    };
}

static void test_beetle_zeta_y16(void)
{
    PGRAPHGLSurfaceTextureLayout layout = beetle_layout();

    g_assert_true(pgraph_gl_zeta_to_y16_compatible(&layout));
}

static void test_reject_color_surface(void)
{
    PGRAPHGLSurfaceTextureLayout layout = beetle_layout();

    layout.surface_color = true;
    g_assert_false(pgraph_gl_zeta_to_y16_compatible(&layout));
}

static void test_reject_swizzled_surface(void)
{
    PGRAPHGLSurfaceTextureLayout layout = beetle_layout();

    layout.surface_swizzled = true;
    g_assert_false(pgraph_gl_zeta_to_y16_compatible(&layout));
}

static void test_reject_nonlinear_texture(void)
{
    PGRAPHGLSurfaceTextureLayout layout = beetle_layout();

    layout.texture_linear = false;
    g_assert_false(pgraph_gl_zeta_to_y16_compatible(&layout));
}

static void test_reject_non_y16_texture(void)
{
    PGRAPHGLSurfaceTextureLayout layout = beetle_layout();

    layout.texture_color_format = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_Y8;
    g_assert_false(pgraph_gl_zeta_to_y16_compatible(&layout));
}

static void test_reject_cubemap(void)
{
    PGRAPHGLSurfaceTextureLayout layout = beetle_layout();

    layout.texture_cubemap = true;
    g_assert_false(pgraph_gl_zeta_to_y16_compatible(&layout));
}

static void test_reject_mip_levels(void)
{
    PGRAPHGLSurfaceTextureLayout layout = beetle_layout();

    layout.texture_levels = 2;
    g_assert_false(pgraph_gl_zeta_to_y16_compatible(&layout));
}

static void test_reject_height_mismatch(void)
{
    PGRAPHGLSurfaceTextureLayout layout = beetle_layout();

    layout.texture_height = 479;
    g_assert_false(pgraph_gl_zeta_to_y16_compatible(&layout));
}

static void test_reject_pitch_mismatch(void)
{
    PGRAPHGLSurfaceTextureLayout layout = beetle_layout();

    layout.texture_pitch = 2048;
    g_assert_false(pgraph_gl_zeta_to_y16_compatible(&layout));
}

static void test_reject_row_bytes_mismatch(void)
{
    PGRAPHGLSurfaceTextureLayout layout = beetle_layout();

    layout.texture_width = 1279;
    g_assert_false(pgraph_gl_zeta_to_y16_compatible(&layout));
}

static PGRAPHGLSurfaceTextureLayout color_layout(void)
{
    PGRAPHGLSurfaceTextureLayout layout = beetle_layout();

    layout.surface_color = true;
    layout.surface_color_format =
        NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
    layout.surface_host_format = 1;
    layout.surface_bytes_per_pixel = 4;
    layout.texture_color_format =
        NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8;
    layout.texture_host_format = 1;
    layout.texture_width = layout.surface_width;
    layout.texture_bytes_per_pixel = 4;
    return layout;
}

static void test_color_same_layout(void)
{
    PGRAPHGLSurfaceTextureLayout layout = color_layout();

    g_assert_true(pgraph_gl_color_surface_to_texture_compatible(&layout));
}

static void test_color_reject_texel_size_mismatch(void)
{
    PGRAPHGLSurfaceTextureLayout layout = color_layout();

    layout.texture_bytes_per_pixel = 2;
    g_assert_false(pgraph_gl_color_surface_to_texture_compatible(&layout));
}

static void test_color_reject_linear_pitch_mismatch(void)
{
    PGRAPHGLSurfaceTextureLayout layout = color_layout();

    layout.texture_pitch -= 4;
    g_assert_false(pgraph_gl_color_surface_to_texture_compatible(&layout));
}

static void test_color_reject_host_format_mismatch(void)
{
    PGRAPHGLSurfaceTextureLayout layout = color_layout();

    layout.texture_host_format = 2;
    g_assert_false(pgraph_gl_color_surface_to_texture_compatible(&layout));
}

static void test_color_x8_conversion(void)
{
    PGRAPHGLSurfaceTextureLayout layout = color_layout();

    layout.texture_color_format =
        NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8;
    layout.texture_host_format = 2;
    g_assert_true(pgraph_gl_color_surface_to_texture_compatible(&layout));
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/nv2a/surface-texture-compat/beetle-zeta-y16",
                    test_beetle_zeta_y16);
    g_test_add_func("/nv2a/surface-texture-compat/reject-color-surface",
                    test_reject_color_surface);
    g_test_add_func("/nv2a/surface-texture-compat/reject-swizzled-surface",
                    test_reject_swizzled_surface);
    g_test_add_func("/nv2a/surface-texture-compat/reject-nonlinear-texture",
                    test_reject_nonlinear_texture);
    g_test_add_func("/nv2a/surface-texture-compat/reject-non-y16-texture",
                    test_reject_non_y16_texture);
    g_test_add_func("/nv2a/surface-texture-compat/reject-cubemap",
                    test_reject_cubemap);
    g_test_add_func("/nv2a/surface-texture-compat/reject-mip-levels",
                    test_reject_mip_levels);
    g_test_add_func("/nv2a/surface-texture-compat/reject-height-mismatch",
                    test_reject_height_mismatch);
    g_test_add_func("/nv2a/surface-texture-compat/reject-pitch-mismatch",
                    test_reject_pitch_mismatch);
    g_test_add_func("/nv2a/surface-texture-compat/reject-row-bytes-mismatch",
                    test_reject_row_bytes_mismatch);
    g_test_add_func("/nv2a/surface-texture-compat/color-same-layout",
                    test_color_same_layout);
    g_test_add_func(
        "/nv2a/surface-texture-compat/color-reject-texel-size-mismatch",
        test_color_reject_texel_size_mismatch);
    g_test_add_func(
        "/nv2a/surface-texture-compat/color-reject-linear-pitch-mismatch",
        test_color_reject_linear_pitch_mismatch);
    g_test_add_func(
        "/nv2a/surface-texture-compat/color-reject-host-format-mismatch",
        test_color_reject_host_format_mismatch);
    g_test_add_func("/nv2a/surface-texture-compat/color-x8-conversion",
                    test_color_x8_conversion);
    return g_test_run();
}

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../../xemu-frameinspect-nv2a-decode.h"

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        return 1; \
    } \
} while (0)

static bool closef(float a, float b)
{
    return fabsf(a - b) < 0.0001f;
}

static void store_float(uint8_t *dst, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    dst[0] = bits;
    dst[1] = bits >> 8;
    dst[2] = bits >> 16;
    dst[3] = bits >> 24;
}

static int test_vertex(void)
{
    CHECK(strcmp(fi_nv2a_vertex_format_name(FI_NV2A_VERTEX_F), "F") == 0);
    CHECK(fi_nv2a_vertex_format_name(3) == NULL);
    CHECK(strcmp(fi_nv2a_topology_name(5), "triangles") == 0);
    CHECK(fi_nv2a_topology_name(11) == NULL);

    uint8_t color[] = { 10, 20, 30, 40 };
    FINV2AVertexValue value;
    CHECK(fi_nv2a_decode_vertex(FI_NV2A_VERTEX_UB_D3D, 4, color,
                                sizeof(color), true, &value));
    CHECK(closef(value.value[0], 30.0f / 255.0f));
    CHECK(closef(value.value[1], 20.0f / 255.0f));
    CHECK(closef(value.value[2], 10.0f / 255.0f));
    CHECK(closef(value.value[3], 40.0f / 255.0f));
    CHECK(value.bytes_consumed == 4 && value.valid);

    CHECK(fi_nv2a_decode_vertex(FI_NV2A_VERTEX_UB_D3D, 4, color,
                                sizeof(color), false, &value));
    CHECK(closef(value.value[0], 10.0f / 255.0f));
    CHECK(closef(value.value[2], 30.0f / 255.0f));

    uint8_t shorts[] = { 0x00, 0x80, 0x00, 0x40 };
    CHECK(fi_nv2a_decode_vertex(FI_NV2A_VERTEX_S1, 2, shorts,
                                sizeof(shorts), true, &value));
    CHECK(closef(value.value[0], -1.0f));
    CHECK(closef(value.value[1], 16384.0f / 32767.0f));
    CHECK(fi_nv2a_decode_vertex(FI_NV2A_VERTEX_S32K, 2, shorts,
                                sizeof(shorts), true, &value));
    CHECK(closef(value.value[0], -32768.0f));
    CHECK(closef(value.value[1], 16384.0f));

    uint8_t floats[8];
    store_float(floats, 1.25f);
    store_float(floats + 4, -2.5f);
    CHECK(fi_nv2a_decode_vertex(FI_NV2A_VERTEX_F, 2, floats,
                                sizeof(floats), true, &value));
    CHECK(closef(value.value[0], 1.25f));
    CHECK(closef(value.value[1], -2.5f));
    CHECK(closef(value.value[3], 1.0f));

    uint32_t packed = 0x400 | (0x3ff << 11) | (0x200u << 22);
    uint8_t cmp[] = { packed, packed >> 8, packed >> 16, packed >> 24 };
    CHECK(fi_nv2a_decode_vertex(FI_NV2A_VERTEX_CMP, 1, cmp,
                                sizeof(cmp), true, &value));
    CHECK(closef(value.value[0], -1.0f));
    CHECK(closef(value.value[1], 1.0f));
    CHECK(closef(value.value[2], -1.0f));
    CHECK(!fi_nv2a_decode_vertex(3, 4, color, sizeof(color), true, &value));
    CHECK(!fi_nv2a_decode_vertex(FI_NV2A_VERTEX_F, 2, floats, 4, true,
                                 &value));
    return 0;
}

static int test_pipeline(void)
{
    uint32_t control0 = 0x7f | (5u << 8) | (1u << 12) | (1u << 14) |
        (6u << 16) | (1u << 22) | (1u << 23) | (1u << 24) |
        (1u << 25) | (0xbu << 26);
    uint32_t control1 = 1 | (3u << 4) | (0x55u << 8) |
        (0xaau << 16) | (0xccu << 24);
    uint32_t control2 = 1 | (6u << 4) | (8u << 8);
    uint32_t blend = 2 | (1u << 3) | (4u << 4) | (7u << 8) |
        (0xau << 12) | (1u << 16);
    uint32_t raster = 2 | (1u << 2) | (1u << 9) | (1u << 10) |
        (1u << 11) | (2u << 21) | (1u << 23) | (1u << 28) |
        (1u << 29) | (1u << 31);
    FINV2APipelineState state;
    fi_nv2a_decode_pipeline(control0, control1, control2, blend, raster,
                            &state);
    CHECK(state.alpha_test_enable && state.alpha_ref == 0x7f);
    CHECK(state.alpha_func == 5 && state.alpha_func_valid);
    CHECK(state.depth_test_enable && state.depth_write_enable);
    CHECK(state.depth_perspective_enable);
    CHECK(state.depth_func == 6 && state.depth_func_valid);
    CHECK(state.color_write_mask == 0xb && state.dither_enable);
    CHECK(state.stencil_test_enable && state.stencil_write_enable);
    CHECK(state.stencil_func == 3 && state.stencil_func_valid);
    CHECK(state.stencil_ref == 0x55);
    CHECK(state.stencil_read_mask == 0xaa);
    CHECK(state.stencil_write_mask == 0xcc);
    CHECK(state.stencil_fail == 1 && state.stencil_zfail == 6 &&
          state.stencil_zpass == 8 && state.stencil_ops_valid);
    CHECK(state.blend_enable && state.blend_equation == 2);
    CHECK(state.blend_equation_valid && state.blend_src_factor_valid &&
          state.blend_dst_factor_valid);
    CHECK(state.blend_src_factor == 4 && state.blend_dst_factor == 7);
    CHECK(state.logic_op_enable && state.logic_op == 0xa);
    CHECK(state.front_fill == 2 && state.back_fill == 1);
    CHECK(state.front_fill_valid && state.back_fill_valid);
    CHECK(state.cull_enable && state.cull_face == 2 && state.front_face_ccw);
    CHECK(state.cull_face_valid);
    CHECK(state.point_smooth_enable && state.line_smooth_enable &&
          state.polygon_smooth_enable);
    CHECK(state.zeta_float && state.window_clip_exclusive);

    fi_nv2a_decode_pipeline(9u << 8, 9u << 4, 0, 7 | (11u << 4),
                            3 | (3u << 2), &state);
    CHECK(!state.alpha_func_valid);
    CHECK(!state.stencil_func_valid);
    CHECK(!state.stencil_ops_valid);
    CHECK(!state.blend_equation_valid);
    CHECK(!state.blend_src_factor_valid);
    CHECK(!state.front_fill_valid && !state.back_fill_valid);
    CHECK(!state.cull_face_valid);
    return 0;
}

static int test_combiner(void)
{
    uint32_t color_icw[FI_NV2A_COMBINER_STAGES] = { 0 };
    uint32_t alpha_icw[FI_NV2A_COMBINER_STAGES] = { 0 };
    uint32_t color_ocw[FI_NV2A_COMBINER_STAGES] = { 0 };
    uint32_t alpha_ocw[FI_NV2A_COMBINER_STAGES] = { 0 };
    uint32_t factor0[FI_NV2A_COMBINER_STAGES] = { 0 };
    uint32_t factor1[FI_NV2A_COMBINER_STAGES] = { 0 };
    color_icw[0] = 0xc435a98f;
    alpha_icw[0] = 0x01020304;
    uint32_t output_flags = 1 | 2 | 4 | 0x18 | 0x40 | 0x80;
    color_ocw[0] = (output_flags << 12) | (4u << 8) | (0xdu << 4) | 0xc;
    alpha_ocw[0] = (0x30u << 12) | (1u << 8) | (2u << 4) | 3;
    factor0[0] = 0x11223344;
    factor1[0] = 0x55667788;

    uint32_t control = 4 | ((1u | 0x10u | 0x100u) << 8);
    uint32_t final0 = 0x01020304;
    uint32_t final1 = 0x050607e0;
    uint32_t shader_program = 1 | (3u << 5) | (0x12u << 10) | (0x1fu << 15);
    FINV2ACombinerState state;
    fi_nv2a_decode_combiner(
        control, color_icw, alpha_icw, color_ocw, alpha_ocw, factor0,
        factor1, final0, final1, 0xaabbccdd, 0x10203040,
        shader_program, &state);

    CHECK(state.stage_count == 4 && state.stage_count_valid);
    CHECK(state.mux_msb && state.unique_c0 && state.unique_c1);
    CHECK(state.stage[0].color_inputs.input[0].raw == 0xc4);
    CHECK(state.stage[0].color_inputs.input[0].reg == 4);
    CHECK(state.stage[0].color_inputs.input[0].mapping == 0xc0);
    CHECK(!state.stage[0].color_inputs.input[0].alpha_channel);
    CHECK(state.stage[0].color_inputs.input[1].raw == 0x35);
    CHECK(state.stage[0].color_inputs.input[1].alpha_channel);
    CHECK(state.stage[0].color_output.cd_dest == 0xc);
    CHECK(state.stage[0].color_output.ab_dest == 0xd);
    CHECK(state.stage[0].color_output.mux_sum_dest == 4);
    CHECK(state.stage[0].color_output.cd_dot);
    CHECK(state.stage[0].color_output.ab_dot);
    CHECK(state.stage[0].color_output.mux);
    CHECK(state.stage[0].color_output.mapping == 0x18);
    CHECK(state.stage[0].color_output.mapping_valid);
    CHECK(state.stage[0].color_output.ab_blue_to_alpha);
    CHECK(state.stage[0].color_output.cd_blue_to_alpha);
    CHECK(state.stage[0].factor0_raw == 0x11223344);
    CHECK(state.stage[0].factor1_raw == 0x55667788);

    CHECK(state.final.enabled && state.final.inputs0_raw == final0);
    CHECK(state.final.input[0].raw == 1);
    CHECK(state.final.input[4].raw == 5);
    CHECK(state.final.input[6].raw == 7);
    CHECK(state.final.clamp_sum && state.final.complement_v1 &&
          state.final.complement_r0);
    CHECK(state.specfog_factor0_raw == 0xaabbccdd);
    CHECK(state.specfog_factor1_raw == 0x10203040);
    CHECK(state.texture_mode[0] == 1 && state.texture_mode_valid[0]);
    CHECK(state.texture_mode[1] == 3 && state.texture_mode_valid[1]);
    CHECK(state.texture_mode[2] == 0x12 && state.texture_mode_valid[2]);
    CHECK(state.texture_mode[3] == 0x1f && !state.texture_mode_valid[3]);
    CHECK(!fi_nv2a_decode_combiner_input(0x44, true).final_mapping_valid);
    CHECK(!fi_nv2a_decode_combiner_output(0x28u << 12).mapping_valid);

    control = 9;
    fi_nv2a_decode_combiner(
        control, color_icw, alpha_icw, color_ocw, alpha_ocw, factor0,
        factor1, 0, 0, 0, 0, 0, &state);
    CHECK(!state.stage_count_valid);
    CHECK(!state.final.enabled);
    return 0;
}

int main(void)
{
    CHECK(test_vertex() == 0);
    CHECK(test_pipeline() == 0);
    CHECK(test_combiner() == 0);
    printf("PASS\n");
    return 0;
}

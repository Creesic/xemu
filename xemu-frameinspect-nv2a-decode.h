/*
 * xemu frame inspector: shared, side-effect-free NV2A state decoders
 *
 * Header-only and QEMU-independent for standalone unit testing.
 *
 * Copyright (C) 2026 xemu contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XEMU_FRAMEINSPECT_NV2A_DECODE_H
#define XEMU_FRAMEINSPECT_NV2A_DECODE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define FI_NV2A_COMBINER_STAGES 8u
#define FI_NV2A_TEXTURE_STAGES 4u

typedef enum FINV2AVertexFormat {
    FI_NV2A_VERTEX_UB_D3D = 0,
    FI_NV2A_VERTEX_S1 = 1,
    FI_NV2A_VERTEX_F = 2,
    FI_NV2A_VERTEX_UB_OGL = 4,
    FI_NV2A_VERTEX_S32K = 5,
    FI_NV2A_VERTEX_CMP = 6,
} FINV2AVertexFormat;

typedef struct FINV2AVertexValue {
    float value[4];
    uint8_t format;
    uint8_t components;
    uint8_t bytes_consumed;
    bool valid;
} FINV2AVertexValue;

static inline uint16_t fi_nv2a_read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)data[1] << 8;
}

static inline uint32_t fi_nv2a_read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static inline const char *fi_nv2a_vertex_format_name(uint8_t format)
{
    switch (format) {
    case FI_NV2A_VERTEX_UB_D3D: return "UB_D3D";
    case FI_NV2A_VERTEX_S1: return "S1";
    case FI_NV2A_VERTEX_F: return "F";
    case FI_NV2A_VERTEX_UB_OGL: return "UB_OGL";
    case FI_NV2A_VERTEX_S32K: return "S32K";
    case FI_NV2A_VERTEX_CMP: return "CMP";
    default: return NULL;
    }
}

static inline bool fi_nv2a_decode_vertex(uint8_t format, uint8_t components,
                                         const uint8_t *raw, uint32_t raw_len,
                                         bool array_enabled,
                                         FINV2AVertexValue *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->format = format;
    out->components = components;
    out->value[3] = 1.0f;
    if (!raw || components == 0 || components > 4) {
        return false;
    }

    uint32_t required;
    switch (format) {
    case FI_NV2A_VERTEX_UB_D3D:
    case FI_NV2A_VERTEX_UB_OGL:
        required = components;
        break;
    case FI_NV2A_VERTEX_S1:
    case FI_NV2A_VERTEX_S32K:
        required = components * 2;
        break;
    case FI_NV2A_VERTEX_F:
        required = components * 4;
        break;
    case FI_NV2A_VERTEX_CMP:
        required = 4;
        if (components != 1) {
            return false;
        }
        break;
    default:
        return false;
    }
    if (required > raw_len) {
        return false;
    }

    switch (format) {
    case FI_NV2A_VERTEX_UB_D3D:
        if (array_enabled && components == 4) {
            out->value[0] = (float)raw[2] / 255.0f;
            out->value[1] = (float)raw[1] / 255.0f;
            out->value[2] = (float)raw[0] / 255.0f;
            out->value[3] = (float)raw[3] / 255.0f;
            break;
        }
        /* Stride-zero constants follow pgraph_update_inline_value(). */
        /* fall through */
    case FI_NV2A_VERTEX_UB_OGL:
        for (uint32_t i = 0; i < components; i++) {
            out->value[i] = (float)raw[i] / 255.0f;
        }
        break;
    case FI_NV2A_VERTEX_S1:
        for (uint32_t i = 0; i < components; i++) {
            int16_t value = (int16_t)fi_nv2a_read_le16(raw + i * 2);
            float decoded = (float)value / 32767.0f;
            out->value[i] = decoded < -1.0f ? -1.0f : decoded;
        }
        break;
    case FI_NV2A_VERTEX_F:
        for (uint32_t i = 0; i < components; i++) {
            uint32_t bits = fi_nv2a_read_le32(raw + i * 4);
            memcpy(&out->value[i], &bits, sizeof(bits));
        }
        break;
    case FI_NV2A_VERTEX_S32K:
        for (uint32_t i = 0; i < components; i++) {
            int16_t value = (int16_t)fi_nv2a_read_le16(raw + i * 2);
            out->value[i] = (float)value;
        }
        break;
    case FI_NV2A_VERTEX_CMP: {
        int32_t packed = (int32_t)fi_nv2a_read_le32(raw);
        int32_t x = packed & 0x7ff;
        int32_t y = (packed >> 11) & 0x7ff;
        int32_t z = (packed >> 22) & 0x3ff;
        if (x & 0x400) x |= ~0x7ff;
        if (y & 0x400) y |= ~0x7ff;
        if (z & 0x200) z |= ~0x3ff;
        out->value[0] = (float)x / 1023.0f;
        out->value[1] = (float)y / 1023.0f;
        out->value[2] = (float)z / 511.0f;
        if (out->value[0] < -1.0f) out->value[0] = -1.0f;
        if (out->value[1] < -1.0f) out->value[1] = -1.0f;
        if (out->value[2] < -1.0f) out->value[2] = -1.0f;
        break;
    }
    default:
        return false;
    }
    out->bytes_consumed = required;
    out->valid = true;
    return true;
}

static inline const char *fi_nv2a_topology_name(uint32_t topology)
{
    static const char *const names[] = {
        "end", "points", "lines", "line_loop", "line_strip",
        "triangles", "triangle_strip", "triangle_fan", "quads",
        "quad_strip", "polygon",
    };
    return topology < sizeof(names) / sizeof(names[0]) ? names[topology] : NULL;
}

typedef struct FINV2APipelineState {
    uint32_t control0_raw;
    uint32_t control1_raw;
    uint32_t control2_raw;
    uint32_t blend_raw;
    uint32_t setup_raster_raw;
    uint8_t alpha_ref;
    uint8_t alpha_func;
    uint8_t depth_func;
    uint8_t color_write_mask; /* bit 0=A, 1=R, 2=G, 3=B */
    uint8_t stencil_func;
    uint8_t stencil_ref;
    uint8_t stencil_read_mask;
    uint8_t stencil_write_mask;
    uint8_t stencil_fail;
    uint8_t stencil_zfail;
    uint8_t stencil_zpass;
    uint8_t blend_equation;
    uint8_t blend_src_factor;
    uint8_t blend_dst_factor;
    uint8_t logic_op;
    uint8_t front_fill;
    uint8_t back_fill;
    uint8_t cull_face;
    bool alpha_test_enable;
    bool alpha_func_valid;
    bool depth_test_enable;
    bool depth_write_enable;
    bool depth_perspective_enable;
    bool depth_func_valid;
    bool dither_enable;
    bool stencil_test_enable;
    bool stencil_write_enable;
    bool stencil_func_valid;
    bool stencil_ops_valid;
    bool blend_enable;
    bool blend_equation_valid;
    bool blend_src_factor_valid;
    bool blend_dst_factor_valid;
    bool logic_op_enable;
    bool front_fill_valid;
    bool back_fill_valid;
    bool cull_enable;
    bool cull_face_valid;
    bool front_face_ccw;
    bool point_smooth_enable;
    bool line_smooth_enable;
    bool polygon_smooth_enable;
    bool zeta_float;
    bool window_clip_exclusive;
} FINV2APipelineState;

static inline bool fi_nv2a_blend_factor_valid(uint8_t factor)
{
    return factor <= 10 || factor >= 12;
}

static inline void fi_nv2a_decode_pipeline(
    uint32_t control0, uint32_t control1, uint32_t control2,
    uint32_t blend, uint32_t setup_raster, FINV2APipelineState *out)
{
    memset(out, 0, sizeof(*out));
    out->control0_raw = control0;
    out->control1_raw = control1;
    out->control2_raw = control2;
    out->blend_raw = blend;
    out->setup_raster_raw = setup_raster;
    out->alpha_ref = control0 & 0xff;
    out->alpha_func = (control0 >> 8) & 0xf;
    out->alpha_test_enable = control0 & (1u << 12);
    out->alpha_func_valid = out->alpha_func <= 7;
    out->depth_test_enable = control0 & (1u << 14);
    out->depth_func = (control0 >> 16) & 0xf;
    out->depth_func_valid = out->depth_func <= 7;
    out->dither_enable = control0 & (1u << 22);
    out->depth_perspective_enable = control0 & (1u << 23);
    out->depth_write_enable = control0 & (1u << 24);
    out->stencil_write_enable = control0 & (1u << 25);
    out->color_write_mask = (control0 >> 26) & 0xf;

    out->stencil_test_enable = control1 & 1;
    out->stencil_func = (control1 >> 4) & 0xf;
    out->stencil_func_valid = out->stencil_func <= 7;
    out->stencil_ref = (control1 >> 8) & 0xff;
    out->stencil_read_mask = (control1 >> 16) & 0xff;
    out->stencil_write_mask = (control1 >> 24) & 0xff;
    out->stencil_fail = control2 & 0xf;
    out->stencil_zfail = (control2 >> 4) & 0xf;
    out->stencil_zpass = (control2 >> 8) & 0xf;
    out->stencil_ops_valid = out->stencil_fail >= 1 &&
        out->stencil_fail <= 8 && out->stencil_zfail >= 1 &&
        out->stencil_zfail <= 8 && out->stencil_zpass >= 1 &&
        out->stencil_zpass <= 8;

    out->blend_equation = blend & 7;
    out->blend_equation_valid = out->blend_equation <= 6;
    out->blend_enable = blend & (1u << 3);
    out->blend_src_factor = (blend >> 4) & 0xf;
    out->blend_dst_factor = (blend >> 8) & 0xf;
    out->blend_src_factor_valid =
        fi_nv2a_blend_factor_valid(out->blend_src_factor);
    out->blend_dst_factor_valid =
        fi_nv2a_blend_factor_valid(out->blend_dst_factor);
    out->logic_op = (blend >> 12) & 0xf;
    out->logic_op_enable = blend & (1u << 16);

    out->front_fill = setup_raster & 3;
    out->back_fill = (setup_raster >> 2) & 3;
    out->front_fill_valid = out->front_fill <= 2;
    out->back_fill_valid = out->back_fill <= 2;
    out->point_smooth_enable = setup_raster & (1u << 9);
    out->line_smooth_enable = setup_raster & (1u << 10);
    out->polygon_smooth_enable = setup_raster & (1u << 11);
    out->cull_face = (setup_raster >> 21) & 3;
    out->cull_face_valid = out->cull_face >= 1 && out->cull_face <= 3;
    out->front_face_ccw = setup_raster & (1u << 23);
    out->cull_enable = setup_raster & (1u << 28);
    out->zeta_float = setup_raster & (1u << 29);
    out->window_clip_exclusive = setup_raster & (1u << 31);
}

typedef struct FINV2ACombinerInput {
    uint8_t raw;
    uint8_t reg;
    uint8_t mapping;
    bool alpha_channel;
    bool register_valid;
    bool final_mapping_valid;
} FINV2ACombinerInput;

typedef struct FINV2ACombinerInputs {
    uint32_t raw;
    FINV2ACombinerInput input[4]; /* A, B, C, D */
} FINV2ACombinerInputs;

typedef struct FINV2ACombinerOutput {
    uint32_t raw;
    uint8_t cd_dest;
    uint8_t ab_dest;
    uint8_t mux_sum_dest;
    uint8_t mapping;
    bool cd_dot;
    bool ab_dot;
    bool mux;
    bool ab_blue_to_alpha;
    bool cd_blue_to_alpha;
    bool mapping_valid;
} FINV2ACombinerOutput;

typedef struct FINV2ACombinerStage {
    FINV2ACombinerInputs color_inputs;
    FINV2ACombinerInputs alpha_inputs;
    FINV2ACombinerOutput color_output;
    FINV2ACombinerOutput alpha_output;
    uint32_t factor0_raw;
    uint32_t factor1_raw;
} FINV2ACombinerStage;

typedef struct FINV2AFinalCombiner {
    uint32_t inputs0_raw;
    uint32_t inputs1_raw;
    FINV2ACombinerInput input[7]; /* A through G */
    bool enabled;
    bool clamp_sum;
    bool complement_v1;
    bool complement_r0;
} FINV2AFinalCombiner;

typedef struct FINV2ACombinerState {
    uint32_t control_raw;
    uint32_t shader_stage_program_raw;
    uint32_t specfog_factor0_raw;
    uint32_t specfog_factor1_raw;
    uint8_t stage_count;
    uint8_t texture_mode[FI_NV2A_TEXTURE_STAGES];
    bool stage_count_valid;
    bool mux_msb;
    bool unique_c0;
    bool unique_c1;
    bool texture_mode_valid[FI_NV2A_TEXTURE_STAGES];
    FINV2ACombinerStage stage[FI_NV2A_COMBINER_STAGES];
    FINV2AFinalCombiner final;
} FINV2ACombinerState;

static inline bool fi_nv2a_combiner_register_valid(uint8_t reg)
{
    return reg <= 5 || reg >= 8;
}

static inline FINV2ACombinerInput fi_nv2a_decode_combiner_input(uint8_t raw,
                                                                bool final)
{
    FINV2ACombinerInput input = {
        .raw = raw,
        .reg = (uint8_t)(raw & 0xf),
        .mapping = (uint8_t)(raw & 0xe0),
        .alpha_channel = (raw & 0x10) != 0,
    };
    input.register_valid = fi_nv2a_combiner_register_valid(input.reg);
    input.final_mapping_valid = !final || input.mapping == 0 ||
                                input.mapping == 0x20;
    return input;
}

static inline FINV2ACombinerInputs fi_nv2a_decode_combiner_inputs(
    uint32_t raw, bool final)
{
    FINV2ACombinerInputs inputs = { .raw = raw };
    inputs.input[0] = fi_nv2a_decode_combiner_input(raw >> 24, final);
    inputs.input[1] = fi_nv2a_decode_combiner_input(raw >> 16, final);
    inputs.input[2] = fi_nv2a_decode_combiner_input(raw >> 8, final);
    inputs.input[3] = fi_nv2a_decode_combiner_input(raw, final);
    return inputs;
}

static inline FINV2ACombinerOutput fi_nv2a_decode_combiner_output(uint32_t raw)
{
    uint32_t flags = raw >> 12;
    FINV2ACombinerOutput output = {
        .raw = raw,
        .cd_dest = (uint8_t)(raw & 0xf),
        .ab_dest = (uint8_t)((raw >> 4) & 0xf),
        .mux_sum_dest = (uint8_t)((raw >> 8) & 0xf),
        .mapping = (uint8_t)(flags & 0x38),
        .cd_dot = (flags & 1) != 0,
        .ab_dot = (flags & 2) != 0,
        .mux = (flags & 4) != 0,
        .ab_blue_to_alpha = (flags & 0x80) != 0,
        .cd_blue_to_alpha = (flags & 0x40) != 0,
    };
    output.mapping_valid = output.mapping == 0 || output.mapping == 0x08 ||
        output.mapping == 0x10 || output.mapping == 0x18 ||
        output.mapping == 0x20 || output.mapping == 0x30;
    return output;
}

static inline void fi_nv2a_decode_combiner(
    uint32_t control, const uint32_t color_icw[FI_NV2A_COMBINER_STAGES],
    const uint32_t alpha_icw[FI_NV2A_COMBINER_STAGES],
    const uint32_t color_ocw[FI_NV2A_COMBINER_STAGES],
    const uint32_t alpha_ocw[FI_NV2A_COMBINER_STAGES],
    const uint32_t factor0[FI_NV2A_COMBINER_STAGES],
    const uint32_t factor1[FI_NV2A_COMBINER_STAGES],
    uint32_t final_inputs0, uint32_t final_inputs1,
    uint32_t specfog_factor0, uint32_t specfog_factor1,
    uint32_t shader_stage_program, FINV2ACombinerState *out)
{
    memset(out, 0, sizeof(*out));
    out->control_raw = control;
    out->stage_count = control & 0xff;
    out->stage_count_valid = out->stage_count <= FI_NV2A_COMBINER_STAGES;
    out->mux_msb = control & (1u << 8);
    out->unique_c0 = control & (1u << 12);
    out->unique_c1 = control & (1u << 16);
    out->shader_stage_program_raw = shader_stage_program;
    out->specfog_factor0_raw = specfog_factor0;
    out->specfog_factor1_raw = specfog_factor1;
    for (uint32_t i = 0; i < FI_NV2A_COMBINER_STAGES; i++) {
        out->stage[i].color_inputs =
            fi_nv2a_decode_combiner_inputs(color_icw[i], false);
        out->stage[i].alpha_inputs =
            fi_nv2a_decode_combiner_inputs(alpha_icw[i], false);
        out->stage[i].color_output =
            fi_nv2a_decode_combiner_output(color_ocw[i]);
        out->stage[i].alpha_output =
            fi_nv2a_decode_combiner_output(alpha_ocw[i]);
        out->stage[i].factor0_raw = factor0[i];
        out->stage[i].factor1_raw = factor1[i];
    }
    for (uint32_t i = 0; i < FI_NV2A_TEXTURE_STAGES; i++) {
        out->texture_mode[i] = (shader_stage_program >> (i * 5)) & 0x1f;
        out->texture_mode_valid[i] = out->texture_mode[i] <= 0x12;
    }

    out->final.inputs0_raw = final_inputs0;
    out->final.inputs1_raw = final_inputs1;
    out->final.enabled = final_inputs0 || final_inputs1;
    FINV2ACombinerInputs abcd =
        fi_nv2a_decode_combiner_inputs(final_inputs0, true);
    FINV2ACombinerInputs efg_flags =
        fi_nv2a_decode_combiner_inputs(final_inputs1, true);
    for (uint32_t i = 0; i < 4; i++) {
        out->final.input[i] = abcd.input[i];
    }
    for (uint32_t i = 0; i < 3; i++) {
        out->final.input[4 + i] = efg_flags.input[i];
    }
    uint8_t final_flags = final_inputs1 & 0xff;
    out->final.clamp_sum = final_flags & 0x80;
    out->final.complement_v1 = final_flags & 0x40;
    out->final.complement_r0 = final_flags & 0x20;
}

#endif

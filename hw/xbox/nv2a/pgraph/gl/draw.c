/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
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

#include "qemu/fast-hash.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "debug.h"
#include "renderer.h"
#include "xemu-frameinspect-capture.h"

void pgraph_gl_fi_vertex_capture_begin(
    uint32_t draw_id, uint16_t route, uint32_t topology,
    const FIDrawSegment *segments, uint32_t num_segments,
    const uint32_t *indices, uint32_t num_indices, uint32_t vertex_count);
bool pgraph_gl_fi_vertex_capture_result(uint32_t draw_id, uint64_t *hash,
                                        FIColorSummary *color0,
                                        FIColorSummary *color1);
unsigned int pgraph_gl_fi_inline_array_vertex_count(PGRAPHState *pg);

static void pgraph_gl_fi_hash_bytes(uint64_t *hash, const void *data,
                                    size_t len)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < len; i++) {
        *hash ^= bytes[i];
        *hash *= 1099511628211ull;
    }
}

static void pgraph_gl_fi_hash_u32(uint64_t *hash, uint32_t value)
{
    uint8_t bytes[4] = {
        value, value >> 8, value >> 16, value >> 24,
    };
    pgraph_gl_fi_hash_bytes(hash, bytes, sizeof(bytes));
}

static void pgraph_gl_fi_color_add(FIColorSummary *summary,
                                   const float decoded[4])
{
    if (!summary->available) {
        summary->available = true;
        memcpy(summary->min, decoded, sizeof(summary->min));
        memcpy(summary->max, decoded, sizeof(summary->max));
        return;
    }
    for (uint32_t i = 0; i < 4; i++) {
        summary->min[i] = MIN(summary->min[i], decoded[i]);
        summary->max[i] = MAX(summary->max[i], decoded[i]);
    }
}

static bool pgraph_gl_fi_finish_vertex_submission(uint32_t draw_id)
{
    if (draw_id == FI_DRAW_INVALID) {
        return false;
    }
    uint64_t hash;
    FIColorSummary color0, color1;
    if (!pgraph_gl_fi_vertex_capture_result(draw_id, &hash, &color0,
                                             &color1) ||
        !xemu_frameinspect_capture_submission_geometry(
            draw_id, hash, &color0, &color1) ||
        !xemu_frameinspect_capture_submission_complete(draw_id)) {
        xemu_frameinspect_capture_submission_abort(draw_id);
        return false;
    }
    return true;
}

static uint32_t pgraph_gl_fi_attach_submission_state(NV2AState *d,
                                                     uint32_t draw_id)
{
    if (draw_id == FI_DRAW_INVALID) {
        return FI_DRAW_INVALID;
    }
    PGRAPHState *pg = &d->pgraph;
    uint32_t regs_resource = xemu_frameinspect_capture_resource(
        FI_RESK_REGS, pg->regs_, sizeof(pg->regs_), 0);
    uint32_t program_resource = xemu_frameinspect_capture_resource(
        FI_RESK_VSH_PROGRAM, pg->program_data, sizeof(pg->program_data), 0);
    uint32_t constants_resource = xemu_frameinspect_capture_resource(
        FI_RESK_VSH_CONSTANTS, pg->vsh_constants,
        sizeof(pg->vsh_constants), 0);
    uint32_t color_gen = pgraph_color_write_enabled(pg) ?
        pgraph_gl_fi_intern_current_color(d) : FI_SURFGEN_INVALID;
    uint32_t zeta_gen = pgraph_zeta_write_enabled(pg) ?
        pgraph_gl_fi_intern_current_zeta(d) : FI_SURFGEN_INVALID;

    if (regs_resource == FI_RES_INVALID ||
        program_resource == FI_RES_INVALID ||
        constants_resource == FI_RES_INVALID ||
        !xemu_frameinspect_capture_submission_resources(
            draw_id, color_gen, zeta_gen, regs_resource, program_resource,
            constants_resource)) {
        xemu_frameinspect_capture_submission_abort(draw_id);
        return FI_DRAW_INVALID;
    }
    return draw_id;
}

static bool pgraph_gl_fi_capture_inline_buffer(NV2AState *d,
                                               uint32_t draw_id)
{
    PGRAPHState *pg = &d->pgraph;
    if (draw_id == FI_DRAW_INVALID) {
        return false;
    }
    uint32_t sample_count = MIN(pg->inline_buffer_length,
                                FI_DRAW_SAMPLE_LIMIT);
    FIVertexSample samples[FI_DRAW_SAMPLE_LIMIT] = {};
    for (uint32_t i = 0; i < sample_count; i++) {
        samples[i].ordinal = i;
        samples[i].source_index = i;
        for (uint32_t slot = 0; slot < FI_DRAW_ATTRIBUTE_COUNT; slot++) {
            samples[i].attrs[slot].source_first = FI_DRAW_INVALID;
            samples[i].attrs[slot].status = FI_VALUE_UNAVAILABLE;
        }
    }

    uint64_t hash = 14695981039346656037ull;
    pgraph_gl_fi_hash_u32(&hash, FI_DRAW_ROUTE_INLINE_BUFFER);
    pgraph_gl_fi_hash_u32(&hash, pg->primitive_mode);
    pgraph_gl_fi_hash_u32(&hash, pg->inline_buffer_length);
    pgraph_gl_fi_hash_u32(&hash, 1);
    pgraph_gl_fi_hash_u32(&hash, 0);
    pgraph_gl_fi_hash_u32(&hash, pg->inline_buffer_length);
    pgraph_gl_fi_hash_u32(&hash, 0);

    FIColorSummary color0 = {}, color1 = {};
    bool valid = true;
    for (uint32_t slot = 0; slot < NV2A_VERTEXSHADER_ATTRIBUTES; slot++) {
        VertexAttribute *attr = &pg->vertex_attributes[slot];
        bool populated = attr->inline_buffer_populated;
        FIAttributeDesc desc = {
            .dma_object = FI_DRAW_INVALID,
            .guest_offset = 0,
            .phys_addr = UINT64_MAX,
            .format = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F,
            .stride = populated ? sizeof(float) * 4 : 0,
            .slot = slot,
            .status = populated ? FI_VALUE_PRESENT : FI_VALUE_CONSTANT,
            .components = 4,
            .element_size = sizeof(float),
            .dma_select = 3,
            .source_first = FI_DRAW_INVALID,
        };
        valid &= xemu_frameinspect_capture_submission_attribute(
            draw_id, slot, &desc);
        pgraph_gl_fi_hash_u32(&hash, slot);
        pgraph_gl_fi_hash_u32(&hash, desc.status);
        pgraph_gl_fi_hash_u32(&hash, desc.format);
        pgraph_gl_fi_hash_u32(&hash, desc.components);
        pgraph_gl_fi_hash_u32(&hash, desc.element_size);
        pgraph_gl_fi_hash_u32(&hash, desc.stride);
        pgraph_gl_fi_hash_u32(&hash, desc.dma_select);

        if (populated) {
            for (uint32_t i = 0; i < pg->inline_buffer_length; i++) {
                const float *value = attr->inline_buffer + i * 4;
                for (uint32_t component = 0; component < 4; component++) {
                    uint32_t bits;
                    memcpy(&bits, &value[component], sizeof(bits));
                    pgraph_gl_fi_hash_u32(&hash, bits);
                }
                if (slot == NV2A_VERTEX_ATTR_DIFFUSE ||
                    slot == NV2A_VERTEX_ATTR_SPECULAR) {
                    pgraph_gl_fi_color_add(
                        slot == NV2A_VERTEX_ATTR_DIFFUSE ? &color0 : &color1,
                        value);
                }
            }
            for (uint32_t i = 0; i < sample_count; i++) {
                FIVertexAttributeSample *sample = &samples[i].attrs[slot];
                const float *value = attr->inline_buffer + i * 4;
                sample->status = FI_VALUE_PRESENT;
                sample->raw_len = sizeof(float) * 4;
                for (uint32_t component = 0; component < 4; component++) {
                    uint32_t bits;
                    memcpy(&bits, &value[component], sizeof(bits));
                    stl_le_p(sample->raw + component * sizeof(bits), bits);
                }
                memcpy(sample->decoded, value, sizeof(sample->decoded));
            }
        } else {
            for (uint32_t i = 0; i < 4; i++) {
                uint32_t bits;
                memcpy(&bits, &attr->inline_value[i], sizeof(bits));
                pgraph_gl_fi_hash_u32(&hash, bits);
            }
            if (slot == NV2A_VERTEX_ATTR_DIFFUSE ||
                slot == NV2A_VERTEX_ATTR_SPECULAR) {
                pgraph_gl_fi_color_add(
                    slot == NV2A_VERTEX_ATTR_DIFFUSE ? &color0 : &color1,
                    attr->inline_value);
            }
            for (uint32_t i = 0; i < sample_count; i++) {
                FIVertexAttributeSample *sample = &samples[i].attrs[slot];
                sample->status = FI_VALUE_CONSTANT;
                memcpy(sample->decoded, attr->inline_value,
                       sizeof(sample->decoded));
            }
        }
    }

    for (uint32_t i = 0; i < sample_count; i++) {
        if (xemu_frameinspect_capture_submission_sample(
                draw_id, &samples[i]) == FI_DRAW_INVALID) {
            valid = false;
        }
    }
    valid &= xemu_frameinspect_capture_submission_geometry(
        draw_id, hash, &color0, &color1);
    valid &= xemu_frameinspect_capture_submission_complete(draw_id);
    if (!valid) {
        xemu_frameinspect_capture_submission_abort(draw_id);
    }
    return valid;
}

static void pgraph_gl_fi_seed_color_baseline(NV2AState *d,
                                              uint32_t surface_gen)
{
    if (!xemu_frameinspect_capture_needs_baseline(surface_gen)) {
        return;
    }
    uint32_t w = 0, h = 0;
    uint32_t *rgba = pgraph_gl_fi_readback_color(d, &w, &h);
    xemu_frameinspect_capture_baseline(surface_gen, rgba, w, h);
    g_free(rgba);
}

void pgraph_gl_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    NV2A_DPRINTF("---------PRE CLEAR ------\n");
    pg->clearing = true;

    GLbitfield gl_mask = 0;

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    if (write_zeta) {
        GLint gl_clear_stencil;
        GLfloat gl_clear_depth;
        pgraph_get_clear_depth_stencil_value(pg, &gl_clear_depth,
                                             &gl_clear_stencil);

        if (parameter & NV097_CLEAR_SURFACE_Z) {
            gl_mask |= GL_DEPTH_BUFFER_BIT;
            glDepthMask(GL_TRUE);
            glClearDepth(gl_clear_depth);
        }
        if (parameter & NV097_CLEAR_SURFACE_STENCIL) {
            gl_mask |= GL_STENCIL_BUFFER_BIT;
            glStencilMask(0xff);
            glClearStencil(gl_clear_stencil);
        }
    }
    if (write_color) {
        gl_mask |= GL_COLOR_BUFFER_BIT;
        glColorMask((parameter & NV097_CLEAR_SURFACE_R)
                         ? GL_TRUE : GL_FALSE,
                    (parameter & NV097_CLEAR_SURFACE_G)
                         ? GL_TRUE : GL_FALSE,
                    (parameter & NV097_CLEAR_SURFACE_B)
                         ? GL_TRUE : GL_FALSE,
                    (parameter & NV097_CLEAR_SURFACE_A)
                         ? GL_TRUE : GL_FALSE);

        GLfloat rgba[4];
        pgraph_get_clear_color(pg, rgba);
        glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    }

    pgraph_gl_surface_update(d, true, write_color, write_zeta);
    uint32_t color_gen = write_color ?
        pgraph_gl_fi_intern_current_color(d) : FI_SURFGEN_INVALID;
    uint32_t zeta_gen = write_zeta ?
        pgraph_gl_fi_intern_current_zeta(d) : FI_SURFGEN_INVALID;
    pgraph_gl_fi_seed_color_baseline(d, color_gen);

    /* FIXME: Needs confirmation */
    unsigned int xmin =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX), NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX), NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY), NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY), NV_PGRAPH_CLEARRECTY_YMAX);

    NV2A_DPRINTF(
        "------------------CLEAR 0x%x %d,%d - %d,%d  %x---------------\n",
        parameter, xmin, ymin, xmax, ymax,
        d->pgraph.regs_[NV_PGRAPH_COLORCLEARVALUE]);

    unsigned int scissor_width = xmax - xmin + 1,
                 scissor_height = ymax - ymin + 1;
    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    NV2A_DPRINTF("Translated clear rect to %d,%d - %d,%d\n", xmin, ymin,
                 xmin + scissor_width - 1, ymin + scissor_height - 1);

    bool full_clear = !xmin && !ymin &&
                      scissor_width >= pg->surface_binding_dim.width &&
                      scissor_height >= pg->surface_binding_dim.height;

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    /* FIXME: Respect window clip?!?! */
    glEnable(GL_SCISSOR_TEST);
    glScissor(xmin, ymin, scissor_width, scissor_height);

    /* Dither */
    /* FIXME: Maybe also disable it here? + GL implementation dependent */
    if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) & NV_PGRAPH_CONTROL_0_DITHERENABLE) {
        glEnable(GL_DITHER);
    } else {
        glDisable(GL_DITHER);
    }

    glClear(gl_mask);

    glDisable(GL_SCISSOR_TEST);

    pgraph_gl_set_surface_dirty(pg, write_color, write_zeta);

    if (r->color_binding) {
        r->color_binding->cleared = full_clear && write_color;
    }
    if (r->zeta_binding) {
        r->zeta_binding->cleared = full_clear && write_zeta;
    }

    xemu_frameinspect_capture_clear(color_gen, zeta_gen, parameter);

    if (xemu_frameinspect_capture_state() == FI_CAP_CAPTURING) {
        uint32_t w = 0, h = 0;
        uint32_t *rgba = pgraph_gl_fi_readback_color(d, &w, &h);
        xemu_frameinspect_capture_attach_pixels(color_gen, rgba, w, h);
        g_free(rgba);
    }

    pg->clearing = false;
}

void pgraph_gl_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    NV2A_GL_DGROUP_BEGIN("NV097_SET_BEGIN_END: 0x%x", pg->primitive_mode);

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    pgraph_gl_surface_update(d, true, true, depth_test || stencil_test);

    if (is_nop_draw) {
        return;
    }

    assert(r->color_binding || r->zeta_binding);

    uint32_t color_gen = color_write ?
        pgraph_gl_fi_intern_current_color(d) : FI_SURFGEN_INVALID;
    uint32_t zeta_gen = pgraph_zeta_write_enabled(pg) ?
        pgraph_gl_fi_intern_current_zeta(d) : FI_SURFGEN_INVALID;
    pgraph_gl_fi_seed_color_baseline(d, color_gen);
    xemu_frameinspect_capture_begin_batch(color_gen, zeta_gen);

    pgraph_gl_bind_textures(d);

    if (xemu_frameinspect_capture_state() == FI_CAP_CAPTURING) {
        /* Snapshot the full PGRAPH register file and the textures this batch
         * consumes (now that pgraph_gl_bind_textures() has resolved texture
         * state), content-hash-deduplicated into fi_cap.resources and
         * referenced from this batch. One 32 KiB register hash per draw is
         * acceptable since capture only runs for a single frame. */
        uint32_t regs_res = xemu_frameinspect_capture_resource(
            FI_RESK_REGS, pg->regs_, sizeof(pg->regs_), 0);
        xemu_frameinspect_capture_batch_resource_ref(regs_res);

        for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            /* Mirror pgraph_gl_bind_textures()'s enable check exactly. */
            if (!pgraph_is_texture_enabled(pg, i)) {
                continue;
            }

            TextureShape state = pgraph_get_texture_shape(pg, i);
            hwaddr texture_vram_offset, palette_vram_offset;
            size_t length, palette_length;
            length = pgraph_get_texture_length(pg, &state);
            texture_vram_offset = pgraph_get_texture_phys_addr(pg, i);
            palette_vram_offset = pgraph_get_texture_palette_phys_addr_length(
                pg, i, &palette_length);

            if (length == 0 || length > (16u << 20) ||
                texture_vram_offset + length > memory_region_size(d->vram)) {
                /* Absurd/zero length, or texture read would exceed VRAM bounds:
                 * skip rather than snapshot garbage. This bounds the reads
                 * independently of bind_textures' assert. Missing, never wrong. */
                continue;
            }

            SurfaceBinding *surface =
                pgraph_gl_surface_get(d, texture_vram_offset);
            bool rt_backed = surface &&
                pgraph_gl_check_surface_to_texture_compatibility(surface,
                                                                  &state);

            uint32_t tex_res;
            if (rt_backed) {
                /* Rendered surface: its guest-RAM bytes are stale/invalid, so
                 * record a dependency reference instead of reading VRAM. */
                tex_res = xemu_frameinspect_capture_resource(
                    FI_RESK_TEXTURE_RTREF, NULL, 0, surface->vram_addr);
                xemu_frameinspect_capture_batch_resource_ref(tex_res);
            } else {
                uint64_t meta = ((uint64_t)state.color_format << 32) |
                                 ((uint64_t)state.width << 16) | state.height;
                tex_res = xemu_frameinspect_capture_resource(
                    FI_RESK_TEXTURE, d->vram_ptr + texture_vram_offset,
                    (uint32_t)length, meta);
                xemu_frameinspect_capture_batch_resource_ref(tex_res);

                bool is_indexed = (state.color_format ==
                    NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8);
                if (is_indexed && palette_length > 0 &&
                    palette_vram_offset + palette_length <= memory_region_size(d->vram)) {
                    /* Palette read bounds-checked independently of bind_textures' assert. */
                    uint32_t pal_res = xemu_frameinspect_capture_resource(
                        FI_RESK_PALETTE, d->vram_ptr + palette_vram_offset,
                        (uint32_t)palette_length, palette_vram_offset);
                    xemu_frameinspect_capture_batch_resource_ref(pal_res);
                }
            }
        }
    }

    pgraph_gl_bind_shaders(pg);

    glColorMask(mask_red, mask_green, mask_blue, mask_alpha);
    glDepthMask(!!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE));
    glStencilMask(GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                           NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE));

    if (pgraph_reg_r(pg, NV_PGRAPH_BLEND) & NV_PGRAPH_BLEND_EN) {
        glEnable(GL_BLEND);
        uint32_t sfactor = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                    NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dfactor = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                    NV_PGRAPH_BLEND_DFACTOR);
        assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_gl_map));
        assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_gl_map));
        glBlendFunc(pgraph_blend_factor_gl_map[sfactor],
                    pgraph_blend_factor_gl_map[dfactor]);

        uint32_t equation = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                     NV_PGRAPH_BLEND_EQN);
        assert(equation < ARRAY_SIZE(pgraph_blend_equation_gl_map));
        glBlendEquation(pgraph_blend_equation_gl_map[equation]);

        uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
        float gl_blend_color[4];
        pgraph_argb_pack32_to_rgba_float(blend_color, gl_blend_color);
        glBlendColor(gl_blend_color[0], gl_blend_color[1], gl_blend_color[2],
                     gl_blend_color[3]);
    } else {
        glDisable(GL_BLEND);
    }

    /* Face culling */
    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER)
            & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
        uint32_t cull_face = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                      NV_PGRAPH_SETUPRASTER_CULLCTRL);
        assert(cull_face < ARRAY_SIZE(pgraph_cull_face_gl_map));
        glCullFace(pgraph_cull_face_gl_map[cull_face]);
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }

    /* Front-face select */
    /* Winding is reverse here because clip-space y-coordinates are inverted */
    glFrontFace(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER)
                    & NV_PGRAPH_SETUPRASTER_FRONTFACE
                        ? GL_CW : GL_CCW);

    /* Polygon offset is handled in geometry and fragment shaders explicitly */
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_POLYGON_OFFSET_LINE);
    glDisable(GL_POLYGON_OFFSET_POINT);

    /* Depth testing */
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);

        uint32_t depth_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0),
                                       NV_PGRAPH_CONTROL_0_ZFUNC);
        assert(depth_func < ARRAY_SIZE(pgraph_depth_func_gl_map));
        glDepthFunc(pgraph_depth_func_gl_map[depth_func]);
    } else {
        glDisable(GL_DEPTH_TEST);
    }

    glEnable(GL_DEPTH_CLAMP);

    /* Set first vertex convention to match Vulkan default */
    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);

    if (stencil_test) {
        glEnable(GL_STENCIL_TEST);

        uint32_t stencil_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                    NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
        uint32_t stencil_ref = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                    NV_PGRAPH_CONTROL_1_STENCIL_REF);
        uint32_t func_mask = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
        uint32_t op_fail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
        uint32_t op_zfail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
        uint32_t op_zpass = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

        assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_gl_map));
        assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_gl_map));
        assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_gl_map));
        assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_gl_map));

        glStencilFunc(
            pgraph_stencil_func_gl_map[stencil_func],
            stencil_ref,
            func_mask);

        glStencilOp(
            pgraph_stencil_op_gl_map[op_fail],
            pgraph_stencil_op_gl_map[op_zfail],
            pgraph_stencil_op_gl_map[op_zpass]);

    } else {
        glDisable(GL_STENCIL_TEST);
    }

    /* Dither */
    /* FIXME: GL implementation dependent */
    if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
            NV_PGRAPH_CONTROL_0_DITHERENABLE) {
        glEnable(GL_DITHER);
    } else {
        glDisable(GL_DITHER);
    }

    glEnable(GL_PROGRAM_POINT_SIZE);

    bool anti_aliasing = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING), NV_PGRAPH_ANTIALIASING_ENABLE);

    /* Edge Antialiasing */
    if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                              NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE) {
        glEnable(GL_LINE_SMOOTH);
        glLineWidth(MIN(r->supported_smooth_line_width_range[1], pg->surface_scale_factor));
    } else {
        glDisable(GL_LINE_SMOOTH);
        glLineWidth(MIN(r->supported_aliased_line_width_range[1], pg->surface_scale_factor));
    }
    if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                              NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE) {
        glEnable(GL_POLYGON_SMOOTH);
    } else {
        glDisable(GL_POLYGON_SMOOTH);
    }

    unsigned int vp_width = pg->surface_binding_dim.width,
                 vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);
    glViewport(0, 0, vp_width, vp_height);

    /* Surface clip */
    /* FIXME: Consider moving to PSH w/ window clip */
    unsigned int xmin = pg->surface_shape.clip_x,
                 ymin = pg->surface_shape.clip_y;

    unsigned int scissor_width = pg->surface_shape.clip_width,
                 scissor_height = pg->surface_shape.clip_height;

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);
    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    glEnable(GL_SCISSOR_TEST);
    glScissor(xmin, ymin, scissor_width, scissor_height);

    /* Visibility testing */
    if (pg->zpass_pixel_count_enable) {
        r->gl_zpass_pixel_count_query_count++;
        r->gl_zpass_pixel_count_queries = (GLuint*)g_realloc(
            r->gl_zpass_pixel_count_queries,
            sizeof(GLuint) * r->gl_zpass_pixel_count_query_count);

        GLuint gl_query;
        glGenQueries(1, &gl_query);
        r->gl_zpass_pixel_count_queries[
            r->gl_zpass_pixel_count_query_count - 1] = gl_query;
        glBeginQuery(GL_SAMPLES_PASSED, gl_query);
    }
}

void pgraph_gl_draw_end(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    if (is_nop_draw) {
        // FIXME: Check PGRAPH register 0x880.
        // HW uses bit 11 in 0x880 to enable or disable a color/zeta limit
        // check that will raise an exception in the case that a draw should
        // modify the color and/or zeta buffer but the target(s) are masked
        // off. This check only seems to trigger during the fragment
        // processing, it is legal to attempt a draw that is entirely
        // clipped regardless of 0x880. See xemu#635 for context.
        NV2A_GL_DGROUP_END();
        return;
    }

    pgraph_gl_flush_draw(d);

    /* End of visibility testing */
    if (pg->zpass_pixel_count_enable) {
        nv2a_profile_inc_counter(NV2A_PROF_QUERY);
        glEndQuery(GL_SAMPLES_PASSED);
    }

    pg->draw_time++;
    if (r->color_binding && pgraph_color_write_enabled(pg)) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && pgraph_zeta_write_enabled(pg)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    if (xemu_frameinspect_capture_state() == FI_CAP_CAPTURING) {
        uint32_t w = 0, h = 0;
        uint32_t *rgba = pgraph_gl_fi_readback_color(d, &w, &h);
        xemu_frameinspect_capture_attach_pixels(
            pgraph_gl_fi_intern_current_color(d), rgba, w, h);
        g_free(rgba);
    }
    xemu_frameinspect_capture_end_batch();

    pgraph_gl_set_surface_dirty(pg, color_write, depth_test || stencil_test);
    NV2A_GL_DGROUP_END();
}

void pgraph_gl_flush_draw(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        return;
    }
    assert(r->shader_binding);

    if (pg->draw_arrays_length) {
        NV2A_GL_DPRINTF(false, "Draw Arrays");
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);
        assert(pg->inline_elements_length == 0);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        FIDrawSegment segments[ARRAY_SIZE(pg->draw_arrays_start)];
        uint64_t submitted = 0;
        for (uint32_t i = 0; i < pg->draw_arrays_length; i++) {
            segments[i].first = pg->draw_arrays_start[i];
            segments[i].count = pg->draw_arrays_count[i];
            submitted += segments[i].count;
        }
        uint32_t draw_id = submitted <= UINT32_MAX ?
            xemu_frameinspect_capture_submission_begin(
                FI_DRAW_ROUTE_ARRAYS, pg->primitive_mode,
                (uint32_t)submitted, 0) :
            FI_DRAW_INVALID;
        if (draw_id != FI_DRAW_INVALID &&
            !xemu_frameinspect_capture_submission_segments(
                draw_id, segments, pg->draw_arrays_length)) {
            xemu_frameinspect_capture_submission_abort(draw_id);
            draw_id = FI_DRAW_INVALID;
        }
        draw_id = pgraph_gl_fi_attach_submission_state(d, draw_id);
        pgraph_gl_fi_vertex_capture_begin(
            draw_id, FI_DRAW_ROUTE_ARRAYS, pg->primitive_mode, segments,
            pg->draw_arrays_length, NULL, 0, (uint32_t)submitted);

        pgraph_gl_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                       pg->draw_arrays_max_count - 1,
                                       false, 0,
                                       pg->draw_arrays_max_count - 1);
        pgraph_gl_fi_finish_vertex_submission(draw_id);
        glMultiDrawArrays(r->shader_binding->gl_primitive_mode,
                          pg->draw_arrays_start,
                          pg->draw_arrays_count,
                          pg->draw_arrays_length);
    } else if (pg->inline_elements_length) {
        NV2A_GL_DPRINTF(false, "Inline Elements");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (int i=0; i < pg->inline_elements_length; i++) {
            max_element = MAX(pg->inline_elements[i], max_element);
            min_element = MIN(pg->inline_elements[i], min_element);
        }

        uint32_t draw_id = xemu_frameinspect_capture_submission_begin(
            FI_DRAW_ROUTE_INDEXED, pg->primitive_mode,
            max_element - min_element + 1, pg->inline_elements_length);
        if (draw_id != FI_DRAW_INVALID &&
            !xemu_frameinspect_capture_submission_indices(
                draw_id, pg->inline_elements, pg->inline_elements_length)) {
            xemu_frameinspect_capture_submission_abort(draw_id);
            draw_id = FI_DRAW_INVALID;
        }
        draw_id = pgraph_gl_fi_attach_submission_state(d, draw_id);
        pgraph_gl_fi_vertex_capture_begin(
            draw_id, FI_DRAW_ROUTE_INDEXED, pg->primitive_mode, NULL, 0,
            pg->inline_elements, pg->inline_elements_length,
            pg->inline_elements_length);

        pgraph_gl_bind_vertex_attributes(
                d, min_element, max_element, false, 0,
                pg->inline_elements[pg->inline_elements_length - 1]);

        VertexKey k;
        memset(&k, 0, sizeof(VertexKey));
        k.count = pg->inline_elements_length;
        k.gl_type = GL_UNSIGNED_INT;
        k.gl_normalize = GL_FALSE;
        k.stride = sizeof(uint32_t);
        uint64_t h = fast_hash((uint8_t*)pg->inline_elements,
                               pg->inline_elements_length * 4);

        LruNode *node = lru_lookup(&r->element_cache, h, &k);
        VertexLruNode *found = container_of(node, VertexLruNode, node);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, found->gl_buffer);
        if (!found->initialized) {
            nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_4);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         pg->inline_elements_length * 4,
                         pg->inline_elements, GL_STATIC_DRAW);
            found->initialized = true;
        } else {
            nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_4_NOTDIRTY);
        }
        pgraph_gl_fi_finish_vertex_submission(draw_id);
        glDrawElements(r->shader_binding->gl_primitive_mode,
                       pg->inline_elements_length, GL_UNSIGNED_INT,
                       (void *)0);
    } else if (pg->inline_buffer_length) {
        NV2A_GL_DPRINTF(false, "Inline Buffer");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        assert(pg->inline_array_length == 0);

        if (pg->compressed_attrs) {
            pg->compressed_attrs = 0;
            pgraph_gl_bind_shaders(pg);
        }

        FIDrawSegment segment = {
            .first = 0,
            .count = pg->inline_buffer_length,
        };
        uint32_t draw_id = xemu_frameinspect_capture_submission_begin(
            FI_DRAW_ROUTE_INLINE_BUFFER, pg->primitive_mode,
            pg->inline_buffer_length, 0);
        if (draw_id != FI_DRAW_INVALID &&
            !xemu_frameinspect_capture_submission_segments(
                draw_id, &segment, 1)) {
            xemu_frameinspect_capture_submission_abort(draw_id);
            draw_id = FI_DRAW_INVALID;
        }
        draw_id = pgraph_gl_fi_attach_submission_state(d, draw_id);
        pgraph_gl_fi_capture_inline_buffer(d, draw_id);

        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->inline_buffer_populated) {
                nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_3);
                glBindBuffer(GL_ARRAY_BUFFER, r->gl_inline_buffer[i]);
                glBufferData(GL_ARRAY_BUFFER,
                             pg->inline_buffer_length * sizeof(float) * 4,
                             attr->inline_buffer, GL_STREAM_DRAW);
                glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, 0, 0);
                glEnableVertexAttribArray(i);
                attr->inline_buffer_populated = false;
                memcpy(attr->inline_value,
                       attr->inline_buffer + (pg->inline_buffer_length - 1) * 4,
                       sizeof(attr->inline_value));
            } else {
                glDisableVertexAttribArray(i);
                glVertexAttrib4fv(i, attr->inline_value);
            }
        }

        glDrawArrays(r->shader_binding->gl_primitive_mode,
                     0, pg->inline_buffer_length);
    } else if (pg->inline_array_length) {
        NV2A_GL_DPRINTF(false, "Inline Array");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        uint32_t vertex_count =
            pgraph_gl_fi_inline_array_vertex_count(pg);
        FIDrawSegment segment = {
            .first = 0,
            .count = vertex_count,
        };
        uint32_t draw_id = xemu_frameinspect_capture_submission_begin(
            FI_DRAW_ROUTE_INLINE_ARRAY, pg->primitive_mode, vertex_count, 0);
        if (draw_id != FI_DRAW_INVALID &&
            !xemu_frameinspect_capture_submission_segments(
                draw_id, &segment, 1)) {
            xemu_frameinspect_capture_submission_abort(draw_id);
            draw_id = FI_DRAW_INVALID;
        }
        draw_id = pgraph_gl_fi_attach_submission_state(d, draw_id);
        pgraph_gl_fi_vertex_capture_begin(
            draw_id, FI_DRAW_ROUTE_INLINE_ARRAY, pg->primitive_mode, &segment,
            1, NULL, 0, vertex_count);
        unsigned int index_count = pgraph_gl_bind_inline_array(d);
        assert(index_count == vertex_count);
        pgraph_gl_fi_finish_vertex_submission(draw_id);
        glDrawArrays(r->shader_binding->gl_primitive_mode,
                     0, index_count);
    } else {
        NV2A_GL_DPRINTF(true, "EMPTY NV097_SET_BEGIN_END");
        NV2A_UNCONFIRMED("EMPTY NV097_SET_BEGIN_END");
    }
}

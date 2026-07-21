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

#include "hw/xbox/nv2a/nv2a_regs.h"
#include <hw/xbox/nv2a/nv2a_int.h>
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

typedef struct FIGLVertexCapture {
    uint32_t draw_id;
    const FIDrawSegment *segments;
    const uint32_t *indices;
    uint32_t num_segments;
    uint32_t num_indices;
    uint32_t sample_count;
    uint64_t hash;
    FIColorSummary color0;
    FIColorSummary color1;
    FIVertexSample samples[FI_DRAW_SAMPLE_LIMIT];
    bool active;
    bool valid;
    bool ready;
} FIGLVertexCapture;

static FIGLVertexCapture fi_vertex_capture;

static void fi_hash_bytes(uint64_t *hash, const void *data, size_t len)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < len; i++) {
        *hash ^= bytes[i];
        *hash *= 1099511628211ull;
    }
}

static void fi_hash_u32(uint64_t *hash, uint32_t value)
{
    uint8_t bytes[4] = {
        value, value >> 8, value >> 16, value >> 24,
    };
    fi_hash_bytes(hash, bytes, sizeof(bytes));
}

static uint32_t fi_vertex_capture_source(uint32_t ordinal)
{
    if (fi_vertex_capture.indices) {
        return ordinal < fi_vertex_capture.num_indices ?
            fi_vertex_capture.indices[ordinal] : FI_DRAW_INVALID;
    }
    for (uint32_t i = 0; i < fi_vertex_capture.num_segments; i++) {
        const FIDrawSegment *segment = &fi_vertex_capture.segments[i];
        if (ordinal < segment->count) {
            return segment->first + ordinal;
        }
        ordinal -= segment->count;
    }
    return FI_DRAW_INVALID;
}

static void fi_decode_attribute(const VertexAttribute *attr,
                                const uint8_t *raw, bool array_enabled,
                                float decoded[4])
{
    if (array_enabled &&
        attr->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D) {
        decoded[0] = (float)raw[2] / 255.0f;
        decoded[1] = (float)raw[1] / 255.0f;
        decoded[2] = (float)raw[0] / 255.0f;
        decoded[3] = (float)raw[3] / 255.0f;
        return;
    }
    VertexAttribute copy = *attr;
    pgraph_update_inline_value(&copy, raw);
    memcpy(decoded, copy.inline_value, sizeof(copy.inline_value));
}

static void fi_color_add(FIColorSummary *summary,
                         const VertexAttribute *attr, const uint8_t *raw,
                         const float decoded[4])
{
    if (!summary->available) {
        summary->available = true;
        if ((attr->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D ||
             attr->format == NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL) &&
            attr->size * attr->count == sizeof(uint32_t)) {
            summary->packed_first = ldl_le_p(raw);
        }
        memcpy(summary->min, decoded, sizeof(summary->min));
        memcpy(summary->max, decoded, sizeof(summary->max));
        return;
    }
    for (uint32_t i = 0; i < 4; i++) {
        summary->min[i] = MIN(summary->min[i], decoded[i]);
        summary->max[i] = MAX(summary->max[i], decoded[i]);
    }
}

void pgraph_gl_fi_vertex_capture_begin(
    uint32_t draw_id, uint16_t route, uint32_t topology,
    const FIDrawSegment *segments, uint32_t num_segments,
    const uint32_t *indices, uint32_t num_indices, uint32_t vertex_count)
{
    memset(&fi_vertex_capture, 0, sizeof(fi_vertex_capture));
    if (draw_id == FI_DRAW_INVALID) {
        return;
    }
    fi_vertex_capture.draw_id = draw_id;
    fi_vertex_capture.segments = segments;
    fi_vertex_capture.num_segments = num_segments;
    fi_vertex_capture.indices = indices;
    fi_vertex_capture.num_indices = num_indices;
    fi_vertex_capture.sample_count = MIN(vertex_count, FI_DRAW_SAMPLE_LIMIT);
    fi_vertex_capture.hash = 14695981039346656037ull;
    fi_hash_u32(&fi_vertex_capture.hash, route);
    fi_hash_u32(&fi_vertex_capture.hash, topology);
    fi_hash_u32(&fi_vertex_capture.hash, vertex_count);
    fi_hash_u32(&fi_vertex_capture.hash, num_segments);
    for (uint32_t i = 0; i < num_segments; i++) {
        fi_hash_u32(&fi_vertex_capture.hash, segments[i].first);
        fi_hash_u32(&fi_vertex_capture.hash, segments[i].count);
    }
    fi_hash_u32(&fi_vertex_capture.hash, num_indices);
    for (uint32_t i = 0; i < num_indices; i++) {
        fi_hash_u32(&fi_vertex_capture.hash, indices[i]);
    }
    for (uint32_t i = 0; i < fi_vertex_capture.sample_count; i++) {
        fi_vertex_capture.samples[i].ordinal = i;
        fi_vertex_capture.samples[i].source_index =
            fi_vertex_capture_source(i);
        for (uint32_t j = 0; j < FI_DRAW_ATTRIBUTE_COUNT; j++) {
            fi_vertex_capture.samples[i].attrs[j].source_first =
                FI_DRAW_INVALID;
            fi_vertex_capture.samples[i].attrs[j].status =
                FI_VALUE_UNAVAILABLE;
        }
    }
    fi_vertex_capture.active = true;
    fi_vertex_capture.valid = true;
}

static void fi_capture_attribute(NV2AState *d, PGRAPHState *pg, uint32_t slot,
                                 VertexAttribute *attr, const uint8_t *base,
                                 size_t stride, bool inline_data,
                                 bool array_enabled)
{
    if (!fi_vertex_capture.active) {
        return;
    }
    FIAttributeDesc desc = {
        .dma_object = !base || inline_data ? FI_DRAW_INVALID :
            (attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a),
        .guest_offset = inline_data ? attr->inline_array_offset : attr->offset,
        .phys_addr = !base || inline_data ? UINT64_MAX :
            (uint64_t)(base - d->vram_ptr),
        .format = attr->format,
        .stride = stride,
        .slot = slot,
        .status = array_enabled ? FI_VALUE_PRESENT : FI_VALUE_CONSTANT,
        .components = attr->count,
        .element_size = attr->size,
        .dma_select = !base ? UINT8_MAX :
            (inline_data ? 2 : attr->dma_select),
        .source_first = FI_DRAW_INVALID,
    };
    if (!xemu_frameinspect_capture_submission_attribute(
            fi_vertex_capture.draw_id, slot, &desc)) {
        fi_vertex_capture.valid = false;
    }

    fi_hash_u32(&fi_vertex_capture.hash, slot);
    fi_hash_u32(&fi_vertex_capture.hash, desc.status);
    fi_hash_u32(&fi_vertex_capture.hash, desc.format);
    fi_hash_u32(&fi_vertex_capture.hash, desc.components);
    fi_hash_u32(&fi_vertex_capture.hash, desc.element_size);
    fi_hash_u32(&fi_vertex_capture.hash, desc.stride);
    fi_hash_u32(&fi_vertex_capture.hash, desc.dma_select);

    if (!array_enabled) {
        uint32_t raw_len = attr->size * attr->count;
        if (base && raw_len) {
            fi_hash_bytes(&fi_vertex_capture.hash, base, raw_len);
            if (slot == NV2A_VERTEX_ATTR_DIFFUSE ||
                slot == NV2A_VERTEX_ATTR_SPECULAR) {
                fi_color_add(slot == NV2A_VERTEX_ATTR_DIFFUSE ?
                                 &fi_vertex_capture.color0 :
                                 &fi_vertex_capture.color1,
                             attr, base, attr->inline_value);
            }
        } else {
            for (uint32_t i = 0; i < 4; i++) {
                uint32_t bits;
                memcpy(&bits, &attr->inline_value[i], sizeof(bits));
                fi_hash_u32(&fi_vertex_capture.hash, bits);
            }
        }
        for (uint32_t i = 0; i < fi_vertex_capture.sample_count; i++) {
            FIVertexAttributeSample *sample =
                &fi_vertex_capture.samples[i].attrs[slot];
            sample->status = FI_VALUE_CONSTANT;
            if (base && raw_len) {
                sample->raw_len = raw_len;
                memcpy(sample->raw, base, raw_len);
            }
            memcpy(sample->decoded, attr->inline_value,
                   sizeof(sample->decoded));
        }
        return;
    }

    uint32_t raw_len = attr->size * attr->count;
    assert(raw_len <= FI_DRAW_RAW_ATTRIBUTE_MAX);
    for (uint32_t ordinal = 0;; ordinal++) {
        uint32_t source_index = fi_vertex_capture_source(ordinal);
        if (source_index == FI_DRAW_INVALID) {
            break;
        }
        const uint8_t *raw = base + (stride ? source_index * stride : 0);
        float decoded[4];
        fi_hash_bytes(&fi_vertex_capture.hash, raw, raw_len);
        if (slot == NV2A_VERTEX_ATTR_DIFFUSE ||
            slot == NV2A_VERTEX_ATTR_SPECULAR) {
            fi_decode_attribute(attr, raw, true, decoded);
            fi_color_add(slot == NV2A_VERTEX_ATTR_DIFFUSE ?
                             &fi_vertex_capture.color0 :
                             &fi_vertex_capture.color1,
                         attr, raw, decoded);
        }
    }

    for (uint32_t i = 0; i < fi_vertex_capture.sample_count; i++) {
        uint32_t source_index = fi_vertex_capture.samples[i].source_index;
        const uint8_t *raw = base + (stride ? source_index * stride : 0);
        FIVertexAttributeSample *sample =
            &fi_vertex_capture.samples[i].attrs[slot];
        sample->status = stride ? FI_VALUE_PRESENT : FI_VALUE_CONSTANT;
        sample->raw_len = raw_len;
        memcpy(sample->raw, raw, raw_len);
        fi_decode_attribute(attr, raw, stride != 0, sample->decoded);
    }
}

static void fi_vertex_capture_end(void)
{
    if (!fi_vertex_capture.active) {
        return;
    }
    for (uint32_t i = 0; i < fi_vertex_capture.sample_count; i++) {
        if (xemu_frameinspect_capture_submission_sample(
                fi_vertex_capture.draw_id,
                &fi_vertex_capture.samples[i]) == FI_DRAW_INVALID) {
            fi_vertex_capture.valid = false;
        }
    }
    fi_vertex_capture.active = false;
    fi_vertex_capture.ready = true;
}

bool pgraph_gl_fi_vertex_capture_result(uint32_t draw_id, uint64_t *hash,
                                        FIColorSummary *color0,
                                        FIColorSummary *color1)
{
    if (!fi_vertex_capture.ready || fi_vertex_capture.draw_id != draw_id) {
        return false;
    }
    *hash = fi_vertex_capture.hash;
    *color0 = fi_vertex_capture.color0;
    *color1 = fi_vertex_capture.color1;
    bool valid = fi_vertex_capture.valid;
    memset(&fi_vertex_capture, 0, sizeof(fi_vertex_capture));
    return valid;
}

static void update_memory_buffer(NV2AState *d, hwaddr addr, hwaddr size,
                                 bool quick)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    glBindBuffer(GL_ARRAY_BUFFER, r->gl_memory_buffer);

    hwaddr end = TARGET_PAGE_ALIGN(addr + size);
    addr &= TARGET_PAGE_MASK;
    assert(end < memory_region_size(d->vram));

    static hwaddr last_addr, last_end;
    if (quick && (addr >= last_addr) && (end <= last_end)) {
        return;
    }
    last_addr = addr;
    last_end = end;

    size = end - addr;
    if (memory_region_test_and_clear_dirty(d->vram, addr, size,
                                           DIRTY_MEMORY_NV2A)) {
        glBufferSubData(GL_ARRAY_BUFFER, addr, size,
                        d->vram_ptr + addr);
        nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_1);
    }
}

void pgraph_gl_update_entire_memory_buffer(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    glBindBuffer(GL_ARRAY_BUFFER, r->gl_memory_buffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, memory_region_size(d->vram), d->vram_ptr);
}

void pgraph_gl_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                   unsigned int max_element, bool inline_data,
                                   unsigned int inline_stride,
                                   unsigned int provoking_element)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    bool updated_memory_buffer = false;
    unsigned int num_elements = max_element - min_element + 1;

    if (inline_data) {
        NV2A_GL_DGROUP_BEGIN("%s (num_elements: %d inline stride: %d)",
                             __func__, num_elements, inline_stride);
    } else {
        NV2A_GL_DGROUP_BEGIN("%s (num_elements: %d)", __func__, num_elements);
    }

    pg->compressed_attrs = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];

        if (!attr->count) {
            fi_capture_attribute(d, pg, i, attr, NULL, 0, false, false);
            glDisableVertexAttribArray(i);
            glVertexAttrib4fv(i, attr->inline_value);
            continue;
        }

        NV2A_DPRINTF("vertex data array format=%d, count=%d, stride=%d\n",
                     attr->format, attr->count, attr->stride);

        GLint gl_count = attr->count;
        GLenum gl_type;
        GLboolean gl_normalize;
        bool needs_conversion = false;

        switch (attr->format) {
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
            gl_type = GL_UNSIGNED_BYTE;
            gl_normalize = GL_TRUE;
            // http://www.opengl.org/registry/specs/ARB/vertex_array_bgra.txt
            gl_count = GL_BGRA;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
            gl_type = GL_UNSIGNED_BYTE;
            gl_normalize = GL_TRUE;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
            gl_type = GL_SHORT;
            gl_normalize = GL_TRUE;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
            gl_type = GL_FLOAT;
            gl_normalize = GL_FALSE;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
            gl_type = GL_SHORT;
            gl_normalize = GL_FALSE;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
            /* 3 signed, normalized components packed in 32-bits. (11,11,10) */
            gl_type = GL_INT;
            assert(attr->count == 1);
            needs_conversion = true;
            break;
        default:
            fprintf(stderr, "Unknown vertex type: 0x%x\n", attr->format);
            assert(!"Unknown vertex type");
            break;
        }

        nv2a_profile_inc_counter(NV2A_PROF_ATTR_BIND);
        hwaddr attrib_data_addr;
        size_t stride;
        const uint8_t *capture_base;

        if (needs_conversion) {
            pg->compressed_attrs |= (1 << i);
        }

        hwaddr start = 0;
        if (inline_data) {
            glBindBuffer(GL_ARRAY_BUFFER, r->gl_inline_array_buffer);
            attrib_data_addr = attr->inline_array_offset;
            stride = inline_stride;
            capture_base = (const uint8_t *)pg->inline_array +
                           attr->inline_array_offset;
        } else {
            hwaddr dma_len;
            uint8_t *attr_data = (uint8_t *)nv_dma_map(
                d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                &dma_len);
            assert(attr->offset < dma_len);
            attrib_data_addr = attr_data + attr->offset - d->vram_ptr;
            stride = attr->stride;
            capture_base = attr_data + attr->offset;
            start = attrib_data_addr + min_element * stride;
            update_memory_buffer(d, start, num_elements * stride,
                                        updated_memory_buffer);
            updated_memory_buffer = true;
        }

        uint32_t provoking_element_index = provoking_element - min_element;
        size_t element_size = attr->size * attr->count;
        assert(element_size <= sizeof(attr->inline_value));
        const uint8_t *last_entry;

        if (inline_data) {
            last_entry = (uint8_t*)pg->inline_array + attr->inline_array_offset;
        } else {
            last_entry = d->vram_ptr + start;
        }
        if (!stride) {
            // Stride of 0 indicates that only the first element should be
            // used.
            pgraph_update_inline_value(attr, last_entry);
            fi_capture_attribute(d, pg, i, attr, capture_base, stride,
                                 inline_data, false);
            glDisableVertexAttribArray(i);
            glVertexAttrib4fv(i, attr->inline_value);
            continue;
        }

        fi_capture_attribute(d, pg, i, attr, capture_base, stride, inline_data,
                             true);

        if (needs_conversion) {
            glVertexAttribIPointer(i, gl_count, gl_type, stride,
                                   (void *)attrib_data_addr);
        } else {
            glVertexAttribPointer(i, gl_count, gl_type, gl_normalize, stride,
                                  (void *)attrib_data_addr);
        }

        glEnableVertexAttribArray(i);
        last_entry += stride * provoking_element_index;
        pgraph_update_inline_value(attr, last_entry);
    }

    fi_vertex_capture_end();
    NV2A_GL_DGROUP_END();
}

static unsigned int pgraph_gl_inline_array_layout(PGRAPHState *pg)
{
    unsigned int offset = 0;
    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (attr->count == 0) {
            continue;
        }

        /* FIXME: Double check */
        offset = ROUND_UP(offset, attr->size);
        attr->inline_array_offset = offset;
        NV2A_DPRINTF("bind inline attribute %d size=%d, count=%d\n",
            i, attr->size, attr->count);
        offset += attr->size * attr->count;
        offset = ROUND_UP(offset, attr->size);
    }
    return offset;
}

unsigned int pgraph_gl_fi_inline_array_vertex_count(PGRAPHState *pg)
{
    unsigned int vertex_size = pgraph_gl_inline_array_layout(pg);
    return vertex_size ? pg->inline_array_length * 4 / vertex_size : 0;
}

unsigned int pgraph_gl_bind_inline_array(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    unsigned int vertex_size = pgraph_gl_inline_array_layout(pg);
    unsigned int index_count = vertex_size ?
        pg->inline_array_length * 4 / vertex_size : 0;

    NV2A_DPRINTF("draw inline array %d, %d\n", vertex_size, index_count);

    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_2);
    glBindBuffer(GL_ARRAY_BUFFER, r->gl_inline_array_buffer);
    GLsizeiptr buffer_size = index_count * vertex_size;
    glBufferData(GL_ARRAY_BUFFER, buffer_size, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, buffer_size, pg->inline_array);
    pgraph_gl_bind_vertex_attributes(d, 0, index_count-1, true, vertex_size,
                                  index_count-1);

    return index_count;
}

static void vertex_cache_entry_init(Lru *lru, LruNode *node, const void *key)
{
    VertexLruNode *vnode = container_of(node, VertexLruNode, node);
    memcpy(&vnode->key, key, sizeof(struct VertexKey));
    vnode->initialized = false;
}

static bool vertex_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    VertexLruNode *vnode = container_of(node, VertexLruNode, node);
    return memcmp(&vnode->key, key, sizeof(VertexKey));
}

static const size_t element_cache_size = 50*1024;

void pgraph_gl_init_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    lru_init(&r->element_cache);
    r->element_cache_entries = g_malloc_n(element_cache_size, sizeof(VertexLruNode));
    assert(r->element_cache_entries != NULL);
    GLuint element_cache_buffers[element_cache_size];
    glGenBuffers(element_cache_size, element_cache_buffers);
    for (int i = 0; i < element_cache_size; i++) {
        r->element_cache_entries[i].gl_buffer = element_cache_buffers[i];
        lru_add_free(&r->element_cache, &r->element_cache_entries[i].node);
    }

    r->element_cache.init_node = vertex_cache_entry_init;
    r->element_cache.compare_nodes = vertex_cache_entry_compare;

    GLint max_vertex_attributes;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_vertex_attributes);
    assert(max_vertex_attributes >= NV2A_VERTEXSHADER_ATTRIBUTES);

    glGenBuffers(NV2A_VERTEXSHADER_ATTRIBUTES, r->gl_inline_buffer);
    glGenBuffers(1, &r->gl_inline_array_buffer);

    glGenBuffers(1, &r->gl_memory_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, r->gl_memory_buffer);
    glBufferData(GL_ARRAY_BUFFER, memory_region_size(d->vram),
                 NULL, GL_DYNAMIC_DRAW);

    glGenVertexArrays(1, &r->gl_vertex_array);
    glBindVertexArray(r->gl_vertex_array);

    assert(glGetError() == GL_NO_ERROR);
}

void pgraph_gl_finalize_buffers(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    GLuint element_cache_buffers[element_cache_size];
    for (int i = 0; i < element_cache_size; i++) {
        element_cache_buffers[i] = r->element_cache_entries[i].gl_buffer;
    }
    glDeleteBuffers(element_cache_size, element_cache_buffers);
    lru_flush(&r->element_cache);

    g_free(r->element_cache_entries);
    r->element_cache_entries = NULL;

    glDeleteBuffers(NV2A_VERTEXSHADER_ATTRIBUTES, r->gl_inline_buffer);
    memset(r->gl_inline_buffer, 0, sizeof(r->gl_inline_buffer));

    glDeleteBuffers(1, &r->gl_inline_array_buffer);
    r->gl_inline_array_buffer = 0;

    glDeleteBuffers(1, &r->gl_memory_buffer);
    r->gl_memory_buffer = 0;

    glDeleteVertexArrays(1, &r->gl_vertex_array);
    r->gl_vertex_array = 0;
}

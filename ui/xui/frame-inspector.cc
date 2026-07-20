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
#include "font-manager.hh"
#include "widgets.hh"
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>
extern "C" {
#include "../../xemu-frameinspect-capture.h"
#include "../../xemu-frameinspect.h"
#include "../../xemu-frameinspect-symbols.h"
#include "../../hw/xbox/nv2a/nv2a_regs.h"
#include "../xemu-settings.h"
}

FrameInspectorWindow frame_inspector_window;

/* IDA symbol map (address -> function name), loaded on demand and persisted
 * via g_config.general.frameinspect_symbols. Not capture-scoped -- it applies
 * to the running title's guest addresses across captures. */
static FISymbols g_fi_symbols;
static bool g_fi_symbols_autoload_tried;

/* Format a guest address as "name+0xNN" (or "name") when a symbol is known,
 * else "0x%08x". Writes into buf and returns it. */
static const char *fi_sym(uint32_t addr, char *buf, size_t n)
{
    uint32_t off = 0;
    const char *name = fi_symbols_lookup(&g_fi_symbols, addr, &off);
    if (name) {
        if (off) {
            snprintf(buf, n, "%s+0x%x", name, off);
        } else {
            snprintf(buf, n, "%s", name);
        }
    } else {
        snprintf(buf, n, "0x%08x", addr);
    }
    return buf;
}

static const char *ev_kind_name(uint8_t k)
{
    switch (k) {
    case FI_EV_METHOD: return "METHOD";
    case FI_EV_BATCH: return "BATCH";
    case FI_EV_CLEAR: return "CLEAR";
    case FI_EV_BLIT: return "BLIT";
    case FI_EV_UPLOAD: return "UPLOAD";
    case FI_EV_SCANOUT: return "SCANOUT";
    default: return "?";
    }
}

/* Colour history pixels are RGBA8888 as produced by
 * pgraph_gl_fi_readback_surface (glReadPixels(GL_RGBA, GL_UNSIGNED_BYTE)):
 * byte 0 = R, byte 1 = G, byte 2 = B, byte 3 = A. Read back as a little-
 * endian uint32 that is therefore 0xAABBGGRR, i.e. R is the low byte. */
static ImVec4 fi_rgba_to_imvec4(uint32_t c)
{
    return ImVec4(((c >> 0) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f,
                  ((c >> 16) & 0xff) / 255.0f, ((c >> 24) & 0xff) / 255.0f);
}

/* Returns the surface generation displayed at scanout time, or -1 if there
 * is no FI_EV_SCANOUT event in this capture, or the scanout surface could
 * not be resolved (FI_SURFGEN_INVALID). */
static int fi_find_scanout_gen(const FICapture *cap)
{
    for (uint32_t i = 0; i < cap->events.count; i++) {
        const FIEvent *ev = &cap->events.events[i];
        if (ev->kind == FI_EV_SCANOUT && ev->surface_gen != FI_SURFGEN_INVALID) {
            return (int)ev->surface_gen;
        }
    }
    return -1;
}

static const FIEvent *fi_find_scanout_event(const FICapture *cap)
{
    for (uint32_t i = cap->events.count; i-- > 0;) {
        if (cap->events.events[i].kind == FI_EV_SCANOUT) {
            return &cap->events.events[i];
        }
    }
    return nullptr;
}

/* Map a global event-log index to its dense per-surface colour-history
 * index. Not every event has an image: failed readbacks, zeta-only work, and
 * untracked blit destinations intentionally return false. */
static bool fi_find_history_event(const FICapture *cap, uint32_t event_id,
                                  int *gen_out, uint32_t *index_out)
{
    if (event_id >= cap->events.count) {
        return false;
    }
    uint32_t gen = cap->events.events[event_id].surface_gen;
    if (gen == FI_SURFGEN_INVALID || !cap->hist || gen >= cap->hist_count) {
        return false;
    }
    const FIColorHist *ch = &cap->hist[gen];
    for (uint32_t i = 0; i < ch->num_events; i++) {
        if (ch->events[i].event_id == event_id) {
            *gen_out = (int)gen;
            *index_out = i;
            return true;
        }
    }
    return false;
}

static const char *fi_confidence_name(uint16_t confidence)
{
    switch (confidence) {
    case FI_ORIG_ATTRIBUTED: return "attributed";
    case FI_ORIG_PARTIAL: return "partial";
    case FI_ORIG_UNATTRIBUTED: return "unattributed";
    case FI_ORIG_LOSTSYNC: return "lostsync";
    default: return "?";
    }
}

static const char *fi_command_kind_name(uint8_t kind)
{
    switch (kind) {
    case FI_CMD_METHOD_HEADER: return "METHOD_HEADER";
    case FI_CMD_METHOD_PARAM: return "METHOD_PARAM";
    case FI_CMD_JUMP: return "JUMP";
    case FI_CMD_OLD_JUMP: return "OLD_JUMP";
    case FI_CMD_CALL: return "CALL";
    case FI_CMD_RETURN: return "RETURN";
    case FI_CMD_RESERVED: return "RESERVED";
    default: return "?";
    }
}

static void fi_format_command(const FICommandRec *rec, char *buf, size_t size)
{
    switch (rec->kind) {
    case FI_CMD_METHOD_HEADER:
        snprintf(buf, size, "method=0x%04x sub=%u count=%u %s",
                 rec->data.header.method, rec->data.header.subchannel,
                 rec->data.header.count,
                 rec->data.header.method_type == FI_CMD_METHOD_INCREASING ?
                     "increasing" : "non-increasing");
        break;
    case FI_CMD_METHOD_PARAM:
        if (rec->data.parameter.packet == FI_COMMAND_INVALID) {
            snprintf(buf, size,
                     "packet=missing method=0x%04x sub=%u param[%u] %s",
                     rec->data.parameter.method,
                     rec->data.parameter.subchannel,
                     rec->data.parameter.parameter_index,
                     rec->data.parameter.method_type ==
                             FI_CMD_METHOD_INCREASING ?
                         "increasing" : "non-increasing");
        } else {
            snprintf(buf, size,
                     "packet=#%u method=0x%04x sub=%u param[%u] %s",
                     rec->data.parameter.packet, rec->data.parameter.method,
                     rec->data.parameter.subchannel,
                     rec->data.parameter.parameter_index,
                     rec->data.parameter.method_type ==
                             FI_CMD_METHOD_INCREASING ?
                         "increasing" : "non-increasing");
        }
        break;
    case FI_CMD_JUMP:
    case FI_CMD_OLD_JUMP:
    case FI_CMD_CALL:
    case FI_CMD_RETURN:
        snprintf(buf, size, "target=0x%08x", rec->data.control.target);
        break;
    default:
        snprintf(buf, size, "-");
        break;
    }
}

static void fi_nv097_method_name(uint32_t method, char *buf, size_t size)
{
#define DEF_METHOD(gclass, name) \
    if (method == gclass ## _ ## name) { \
        snprintf(buf, size, #gclass "_" #name); \
        return; \
    }
#define DEF_METHOD_RANGE(gclass, name, range) \
    if (method >= gclass ## _ ## name && \
        method < gclass ## _ ## name + 4 * (range)) { \
        snprintf(buf, size, #gclass "_" #name "[%u]", \
                 (method - gclass ## _ ## name) / 4); \
        return; \
    }
#define DEF_METHOD_CASE_4_OFFSET(gclass, name, offset, stride) \
    if (method >= gclass ## _ ## name + (offset) && \
        method <= gclass ## _ ## name + (offset) + 3 * (stride) && \
        (method - gclass ## _ ## name - (offset)) % (stride) == 0) { \
        snprintf(buf, size, #gclass "_" #name "[%u]+0x%x", \
                 (method - gclass ## _ ## name - (offset)) / (stride), \
                 (unsigned int)(offset)); \
        return; \
    }
#define DEF_METHOD_CASE_4(gclass, name, stride) \
    DEF_METHOD_CASE_4_OFFSET(gclass, name, 0, stride)
#include "../../hw/xbox/nv2a/pgraph/methods.h.inc"
#undef DEF_METHOD
#undef DEF_METHOD_RANGE
#undef DEF_METHOD_CASE_4_OFFSET
#undef DEF_METHOD_CASE_4
    snprintf(buf, size, "NV097_METHOD_0x%04x", method);
}

static const char *fi_primitive_name(uint32_t primitive)
{
    switch (primitive) {
    case NV097_SET_BEGIN_END_OP_END: return "END";
    case NV097_SET_BEGIN_END_OP_POINTS: return "POINTS";
    case NV097_SET_BEGIN_END_OP_LINES: return "LINES";
    case NV097_SET_BEGIN_END_OP_LINE_LOOP: return "LINE_LOOP";
    case NV097_SET_BEGIN_END_OP_LINE_STRIP: return "LINE_STRIP";
    case NV097_SET_BEGIN_END_OP_TRIANGLES: return "TRIANGLES";
    case NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP: return "TRIANGLE_STRIP";
    case NV097_SET_BEGIN_END_OP_TRIANGLE_FAN: return "TRIANGLE_FAN";
    case NV097_SET_BEGIN_END_OP_QUADS: return "QUADS";
    case NV097_SET_BEGIN_END_OP_QUAD_STRIP: return "QUAD_STRIP";
    case NV097_SET_BEGIN_END_OP_POLYGON: return "POLYGON";
    default: return "UNKNOWN";
    }
}

static void fi_format_readable_command(const FICommandRec *rec, char *operation,
                                       size_t operation_size, char *meaning,
                                       size_t meaning_size)
{
    if (rec->kind != FI_CMD_METHOD_PARAM) {
        snprintf(operation, operation_size, "%s",
                 fi_command_kind_name(rec->kind));
        if (rec->kind == FI_CMD_JUMP || rec->kind == FI_CMD_OLD_JUMP ||
            rec->kind == FI_CMD_CALL || rec->kind == FI_CMD_RETURN) {
            snprintf(meaning, meaning_size, "target=0x%08x",
                     rec->data.control.target);
        } else {
            snprintf(meaning, meaning_size, "raw=0x%08x", rec->raw);
        }
        return;
    }

    uint32_t method = rec->data.parameter.method;
    fi_nv097_method_name(method, operation, operation_size);
    switch (method) {
    case NV097_SET_BEGIN_END:
        snprintf(meaning, meaning_size, "primitive=%s (0x%x)",
                 fi_primitive_name(rec->raw), rec->raw);
        break;
    case NV097_DRAW_ARRAYS: {
        uint32_t start = rec->raw & 0x00ffffff;
        uint32_t count = ((rec->raw >> 24) & 0xff) + 1;
        snprintf(meaning, meaning_size,
                 "start=%u (0x%x), count=%u, last=%u", start, start, count,
                 start + count - 1);
        break;
    }
    case NV097_ARRAY_ELEMENT16:
        snprintf(meaning, meaning_size, "indices=%u, %u", rec->raw & 0xffff,
                 rec->raw >> 16);
        break;
    case NV097_ARRAY_ELEMENT32:
        snprintf(meaning, meaning_size, "index=%u (0x%x)", rec->raw,
                 rec->raw);
        break;
    case NV097_INLINE_ARRAY:
        snprintf(meaning, meaning_size, "inline vertex dword=0x%08x",
                 rec->raw);
        break;
    default:
        snprintf(meaning, meaning_size, "value=0x%08x", rec->raw);
        break;
    }
}

static void fi_command_writer_symbol(const FICapture *cap,
                                     const FICommandRec *rec, char *buf,
                                     size_t size)
{
    if (rec->confidence != FI_ORIG_ATTRIBUTED &&
        rec->confidence != FI_ORIG_PARTIAL) {
        snprintf(buf, size, "%s", fi_confidence_name(rec->confidence));
        return;
    }
    const FIOriginNode *node =
        fi_origin_snapshot_node(&cap->origins, rec->writer_node);
    if (!node) {
        snprintf(buf, size, "node %u (origin unavailable)", rec->writer_node);
        return;
    }
    char symbol[160];
    fi_sym(node->callee, symbol, sizeof(symbol));
    snprintf(buf, size, "%s%s", symbol,
             rec->confidence == FI_ORIG_PARTIAL ? " (partial)" : "");
}

/* Mirrors xemu-frameinspect-tagmap.h's tag encoding (tag==0 => unattributed;
 * else node_id+1, with the high bit flagging a store that covered only part
 * of the dword). Defined locally rather than including tagmap.h: that header
 * is C-only (its calloc()/free() results rely on C's implicit void*
 * conversion and won't compile as C++) -- the same reasoning
 * xemu-frameinspect.h documents for mirroring FI_NODE_* instead of including
 * calltree.h there. */
static constexpr uint32_t FI_TAG_PARTIAL_BIT = 0x80000000u;
static uint32_t fi_tag_to_node(uint32_t tag)
{
    return (tag & 0x7FFFFFFFu) - 1u;
}

/* Linear scan for the FIMethodBatch whose batch_event == event_idx (the
 * join key: FIMethodBatch.batch_event references the begin_batch event
 * index). Returns NULL for non-batch events (clear/blit/scanout) or a
 * split-batch event that logged no methods of its own. */
static const FIMethodBatch *fi_find_method_batch(const FICapture *cap,
                                                 int event_idx)
{
    if (event_idx < 0) {
        return nullptr;
    }
    for (uint32_t i = 0; i < cap->methods.num_batches; i++) {
        if (cap->methods.batches[i].batch_event == (uint32_t)event_idx) {
            return &cap->methods.batches[i];
        }
    }
    return nullptr;
}

/* First resource of `kind` referenced by event_idx's batch (join via
 * cap->batch_res, keyed the same way as FIMethodBatch.batch_event). */
static const FIResource *fi_find_batch_resource(const FICapture *cap,
                                                int event_idx, uint32_t kind)
{
    if (event_idx < 0) {
        return nullptr;
    }
    for (uint32_t i = 0; i < cap->num_batch_res; i++) {
        const FIBatchResRef *ref = &cap->batch_res[i];
        if (ref->event != (uint32_t)event_idx) {
            continue;
        }
        if (ref->res_id >= cap->resources.num_res) {
            continue;
        }
        if (cap->resources.res[ref->res_id].kind == kind) {
            return &cap->resources.res[ref->res_id];
        }
    }
    return nullptr;
}

static void fi_methods_tab(FrameInspectorWindow *w, const FICapture *cap,
                          const FIMethodBatch *batch)
{
    if (!batch) {
        ImGui::TextDisabled(
            "No method records for this event (clear/blit/split-batch).");
        return;
    }

    if (ImGui::Button("Copy all methods")) {
        std::string text;
        char line[256];
        snprintf(line, sizeof(line), "event #%u: %u method records\n",
                 batch->batch_event, batch->rec_count);
        text.append(line);
        for (uint32_t i = batch->first_rec;
             i < batch->first_rec + batch->rec_count; i++) {
            const FIMethodRec *r = &cap->methods.recs[i];
            if (r->method == FI_METHOD_RAW_WORD) {
                snprintf(line, sizeof(line),
                         "[%u] method=RAW_WORD sub=%u param=0x%08x "
                         "phys=0x%08x confidence=%s writer_node=%u\n",
                         i, r->subchannel, r->param, r->phys_addr,
                         fi_confidence_name(r->confidence), r->writer_node);
            } else {
                snprintf(line, sizeof(line),
                         "[%u] method=0x%04x sub=%u param=0x%08x "
                         "phys=0x%08x confidence=%s writer_node=%u\n",
                         i, r->method, r->subchannel, r->param, r->phys_addr,
                         fi_confidence_name(r->confidence), r->writer_node);
            }
            text.append(line);
        }
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    HelpMarker("Copies every method in the selected batch as plain text, "
               "independent of the table filter.");

    w->m_method_filter.Draw("Filter", 200.0f * g_viewport_mgr.m_scale);

    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("fi_methods_tbl", 5, flags,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("Method");
        ImGui::TableSetupColumn("Sub");
        ImGui::TableSetupColumn("Param");
        ImGui::TableSetupColumn("Phys Addr");
        ImGui::TableSetupColumn("Confidence");
        ImGui::TableHeadersRow();

        for (uint32_t i = batch->first_rec;
             i < batch->first_rec + batch->rec_count; i++) {
            const FIMethodRec *r = &cap->methods.recs[i];
            char method_str[24];
            if (r->method == FI_METHOD_RAW_WORD) {
                snprintf(method_str, sizeof(method_str), "(raw dword)");
            } else {
                snprintf(method_str, sizeof(method_str), "0x%04x", r->method);
            }
            const char *conf_str = fi_confidence_name(r->confidence);

            char filter_row[160];
            snprintf(filter_row, sizeof(filter_row),
                    "%s %u 0x%08x 0x%08x %s", method_str, r->subchannel,
                    r->param, r->phys_addr, conf_str);
            if (!w->m_method_filter.PassFilter(filter_row)) {
                continue;
            }

            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(method_str, (int)i == w->m_selected_rec,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                w->m_selected_rec = (int)i;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", r->subchannel);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("0x%08x", r->param);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("0x%08x", r->phys_addr);
            ImGui::SameLine();
            if (ImGui::SmallButton("copy")) {
                char hex[16];
                snprintf(hex, sizeof(hex), "0x%08x", r->phys_addr);
                ImGui::SetClipboardText(hex);
            }
            ImGui::TableSetColumnIndex(4);
            ImVec4 color;
            switch (r->confidence) {
            case FI_ORIG_ATTRIBUTED: color = ImVec4(0.3f, 0.9f, 0.3f, 1); break;
            case FI_ORIG_PARTIAL: color = ImVec4(0.9f, 0.9f, 0.2f, 1); break;
            default: color = ImVec4(0.6f, 0.6f, 0.6f, 1); break;
            }
            ImGui::TextColored(color, "%s", conf_str);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
}

/* Walks a captured call chain from start_node up to the root, one row per
 * frame (innermost/writer first). */
static void fi_render_call_chain(const FICapture *cap, uint32_t start_node)
{
    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    uint32_t node = start_node;
    if (node == FI_NODE_ROOT) {
        ImGui::TextDisabled(
            "Origin is the root frame (call path not tracked further).");
    }
    int indents = 0;
    int depth = 0;
    while (node != FI_NODE_ROOT && node != FI_NODE_INVALID) {
        const FIOriginNode *ni = fi_origin_snapshot_node(&cap->origins, node);
        if (!ni) {
            ImGui::TextColored(ImVec4(1, 0.6f, 0, 1),
                              "origin unavailable (not captured or truncated)");
            break;
        }
        const uint32_t *args =
            fi_origin_snapshot_argset(&cap->origins, ni, 0);
        static const uint32_t no_args[6] = {};
        if (!args) {
            args = no_args;
        }
        ImGui::PushID(depth);
        char cs[128], ce[128];
        ImGui::Text("%s -> %s", fi_sym(ni->call_site, cs, sizeof(cs)),
                    fi_sym(ni->callee, ce, sizeof(ce)));
        ImGui::Text("call_site=0x%08x callee=0x%08x  ecx/this=0x%08x  "
                    "args: %08x %08x %08x %08x %08x",
                    ni->call_site, ni->callee, args[0], args[1], args[2],
                    args[3], args[4], args[5]);
        ImGui::SameLine();
        if (ImGui::SmallButton("copy callee")) {
            char hex[16];
            snprintf(hex, sizeof(hex), "0x%08x", ni->callee);
            ImGui::SetClipboardText(hex);
        }
        ImGui::PopID();
        ImGui::Indent();
        indents++;
        node = ni->parent;
        depth++;
    }
    for (int i = 0; i < indents; i++) {
        ImGui::Unindent();
    }
    ImGui::PopFont();
}

/* Groups the selected batch's method records by writer_node and renders a
 * one-click list of the distinct guest functions that emitted them. A single
 * draw batch is usually built by several functions -- e.g. render-state setup
 * writes a handful of methods while a separate glyph/geometry routine emits
 * the bulk of the inline vertex data (method 0x1818). The Origin tab otherwise
 * defaults to the batch's first record (the state write), so this surfaces the
 * dominant geometry writer directly. Clicking a writer points m_selected_rec
 * at one of its records so the chain below (and the Copy button) follow. */
static void fi_render_batch_writers(FrameInspectorWindow *w,
                                    const FICapture *cap,
                                    const FIMethodBatch *batch)
{
    if (!batch || batch->rec_count == 0) {
        return;
    }
    struct Writer {
        uint32_t node;
        uint32_t count;
        int first_rec;
        uint32_t method;
        bool partial;
    };
    enum { MAX_WRITERS = 64 };
    Writer writers[MAX_WRITERS];
    int n_writers = 0;
    bool truncated = false;
    for (uint32_t i = batch->first_rec;
         i < batch->first_rec + batch->rec_count; i++) {
        const FIMethodRec *r = &cap->methods.recs[i];
        if (r->confidence != FI_ORIG_ATTRIBUTED &&
            r->confidence != FI_ORIG_PARTIAL) {
            continue; /* no resolvable writer node */
        }
        int found = -1;
        for (int k = 0; k < n_writers; k++) {
            if (writers[k].node == r->writer_node) {
                found = k;
                break;
            }
        }
        if (found < 0) {
            if (n_writers >= MAX_WRITERS) {
                truncated = true;
                continue;
            }
            found = n_writers++;
            writers[found] = { r->writer_node, 0, (int)i, r->method, false };
        }
        writers[found].count++;
        if (r->confidence == FI_ORIG_PARTIAL) {
            writers[found].partial = true;
        }
    }
    if (n_writers <= 1) {
        return; /* only one writer (or none) -- the chain below is enough */
    }
    /* Dominant contributor first (usually the vertex/geometry builder), then
     * by first appearance. */
    for (int a = 0; a < n_writers; a++) {
        for (int b = a + 1; b < n_writers; b++) {
            if (writers[b].count > writers[a].count ||
                (writers[b].count == writers[a].count &&
                 writers[b].first_rec < writers[a].first_rec)) {
                Writer t = writers[a];
                writers[a] = writers[b];
                writers[b] = t;
            }
        }
    }

    uint32_t cur_node = FI_NODE_INVALID;
    if (w->m_selected_rec >= 0 &&
        (uint32_t)w->m_selected_rec < cap->methods.num_recs) {
        cur_node = cap->methods.recs[w->m_selected_rec].writer_node;
    }

    ImGui::TextUnformatted("Writers in this batch");
    ImGui::SameLine();
    HelpMarker("A draw batch is often emitted by several functions (render-"
               "state setup vs. the code that builds the vertex/geometry data). "
               "Ranked by method count -- select a writer to show its call "
               "chain below.");
    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    for (int k = 0; k < n_writers; k++) {
        char method_ex[16];
        if (writers[k].method == FI_METHOD_RAW_WORD) {
            snprintf(method_ex, sizeof(method_ex), "raw");
        } else {
            snprintf(method_ex, sizeof(method_ex), "0x%04x", writers[k].method);
        }
        /* Lead with the function this node represents (its callee) so the
         * writer reads as "GlyphBuilder - 448 methods" rather than a bare id. */
        char fn[128];
        const FIOriginNode *ni =
            fi_origin_snapshot_node(&cap->origins, writers[k].node);
        if (ni) {
            fi_sym(ni->callee, fn, sizeof(fn));
        } else {
            snprintf(fn, sizeof(fn), "node %u", writers[k].node);
        }
        char label[192];
        snprintf(label, sizeof(label),
                 "%s  -  %u method%s  (e.g. %s)  [node %u]%s", fn,
                 writers[k].count, writers[k].count == 1 ? "" : "s", method_ex,
                 writers[k].node, writers[k].partial ? "  partial" : "");
        ImGui::PushID(k);
        if (ImGui::Selectable(label, writers[k].node == cur_node)) {
            w->m_selected_rec = writers[k].first_rec;
        }
        ImGui::PopID();
    }
    ImGui::PopFont();
    if (truncated) {
        ImGui::TextDisabled("(writer list truncated at %d)", (int)MAX_WRITERS);
    }
    ImGui::Separator();
}

/* Resolves the selected Methods-tab record's writer_node and renders its
 * call chain via fi_render_call_chain(). */
static void fi_origin_tab(FrameInspectorWindow *w, const FICapture *cap,
                          const FIMethodBatch *batch)
{
    ImGui::TextUnformatted("Call-path origin");
    ImGui::SameLine();
    HelpMarker("Origin is an immutable snapshot owned by this capture and "
               "remains valid after re-arming or resuming.");
    ImGui::Separator();

    fi_render_batch_writers(w, cap, batch);

    int selected_rec = w->m_selected_rec;
    if (selected_rec < 0 || (uint32_t)selected_rec >= cap->methods.num_recs) {
        ImGui::TextDisabled("Origin unavailable (unattributed).");
        return;
    }
    const FIMethodRec *rec = &cap->methods.recs[selected_rec];
    if (rec->confidence != FI_ORIG_ATTRIBUTED &&
        rec->confidence != FI_ORIG_PARTIAL) {
        ImGui::TextDisabled("Origin unavailable (unattributed).");
        return;
    }
    if (rec->confidence == FI_ORIG_PARTIAL) {
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1),
                           "(partial attribution)");
    }

    if (ImGui::Button("Copy origin")) {
        std::string text;
        char line[256];
        snprintf(line, sizeof(line),
                 "method_record=%d writer_node=%u confidence=%s "
                 "origin_generation=%u\n",
                 selected_rec, rec->writer_node,
                 fi_confidence_name(rec->confidence),
                 cap->origin_generation);
        text.append(line);
        uint32_t node = rec->writer_node;
        uint32_t depth = 0;
        while (node != FI_NODE_ROOT && node != FI_NODE_INVALID &&
               depth < 256) {
            const FIOriginNode *ni =
                fi_origin_snapshot_node(&cap->origins, node);
            if (!ni) {
                text.append("origin unavailable (not captured or truncated)\n");
                break;
            }
            const uint32_t *args =
                fi_origin_snapshot_argset(&cap->origins, ni, 0);
            static const uint32_t no_args[6] = {};
            if (!args) {
                args = no_args;
            }
            snprintf(
                line, sizeof(line),
                "[%u] node=%u call_site=0x%08x callee=0x%08x "
                "ecx/this=0x%08x args=%08x %08x %08x %08x %08x\n",
                depth, node, ni->call_site, ni->callee, args[0], args[1],
                args[2], args[3], args[4], args[5]);
            text.append(line);
            node = ni->parent;
            depth++;
        }
        if (node == FI_NODE_ROOT) {
            text.append("[root]\n");
        } else if (depth == 256) {
            text.append("[truncated at 256 frames]\n");
        }
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    HelpMarker("Copies the selected method's writer call chain as plain text.");

    fi_render_call_chain(cap, rec->writer_node);
}

static bool fi_command_matches_method(const FICommandRec *command,
                                      const FIMethodRec *method)
{
    return command->kind == FI_CMD_METHOD_PARAM &&
           command->data.parameter.method == method->method &&
           command->data.parameter.subchannel == method->subchannel &&
           command->raw == method->param &&
           command->phys_addr == method->phys_addr &&
           command->writer_node == method->writer_node &&
           command->confidence == method->confidence;
}

static bool fi_batch_command_rows(const FICapture *cap,
                                  const FIMethodBatch *batch,
                                  std::vector<uint32_t> *rows,
                                  uint32_t *raw_methods)
{
    rows->clear();
    *raw_methods = 0;
    if (!batch || batch->first_rec > cap->methods.num_recs ||
        batch->rec_count > cap->methods.num_recs - batch->first_rec) {
        return false;
    }

    uint32_t batch_end = batch->first_rec + batch->rec_count;
    uint32_t method_idx = 0;
    uint32_t first_param = FI_COMMAND_INVALID;
    uint32_t last_param = FI_COMMAND_INVALID;
    for (uint32_t i = 0; i < cap->commands.num_recs && method_idx < batch_end;
         i++) {
        const FICommandRec *command = &cap->commands.recs[i];
        if (command->kind != FI_CMD_METHOD_PARAM) {
            continue;
        }
        while (method_idx < batch_end &&
               cap->methods.recs[method_idx].method == FI_METHOD_RAW_WORD) {
            if (method_idx >= batch->first_rec) {
                (*raw_methods)++;
            }
            method_idx++;
        }
        if (method_idx >= batch_end) {
            break;
        }
        if (!fi_command_matches_method(command,
                                       &cap->methods.recs[method_idx])) {
            return false;
        }
        if (method_idx >= batch->first_rec) {
            uint32_t packet = command->data.parameter.packet;
            if (packet != FI_COMMAND_INVALID) {
                if (packet >= i || packet >= cap->commands.num_recs ||
                    cap->commands.recs[packet].kind !=
                        FI_CMD_METHOD_HEADER) {
                    return false;
                }
                rows->push_back(packet);
            }
            rows->push_back(i);
            if (first_param == FI_COMMAND_INVALID) {
                first_param = i;
            }
            last_param = i;
        }
        method_idx++;
    }
    while (method_idx < batch_end &&
           cap->methods.recs[method_idx].method == FI_METHOD_RAW_WORD) {
        if (method_idx >= batch->first_rec) {
            (*raw_methods)++;
        }
        method_idx++;
    }
    if (method_idx != batch_end) {
        return false;
    }

    if (first_param != FI_COMMAND_INVALID) {
        for (uint32_t i = first_param; i <= last_param; i++) {
            if (cap->commands.recs[i].kind != FI_CMD_METHOD_PARAM) {
                rows->push_back(i);
            }
        }
    }
    std::sort(rows->begin(), rows->end());
    rows->erase(std::unique(rows->begin(), rows->end()), rows->end());
    return true;
}

static void fi_commands_tab(FrameInspectorWindow *w, const FICapture *cap,
                            const FIMethodBatch *batch)
{
    if (!batch) {
        ImGui::TextDisabled("No command records for this event.");
        return;
    }

    std::vector<uint32_t> rows;
    uint32_t raw_methods = 0;
    if (!fi_batch_command_rows(cap, batch, &rows, &raw_methods)) {
        ImGui::TextColored(
            ImVec4(1, 0.6f, 0, 1),
            "Command/method logs do not align; batch filtering is unavailable.");
        return;
    }

    ImGui::Text("Batch event #%u: %u command records (%u frame-wide)%s",
                batch->batch_event, (uint32_t)rows.size(),
                cap->commands.num_recs,
                cap->commands.truncated ? " [TRUNCATED]" : "");
    ImGui::SameLine();
    HelpMarker("Shows parameters in the selected batch, their method packet "
               "headers, and control words executed between those parameters. "
               "The join is validated against the method log before display.");
    if (raw_methods) {
        ImGui::TextDisabled("%u raw lookahead method record(s) have no typed "
                            "command row.", raw_methods);
    }

    if (rows.empty()) {
        ImGui::TextDisabled("No typed PFIFO commands matched this batch.");
        return;
    }
    if (w->m_selected_command < 0 ||
        !std::binary_search(rows.begin(), rows.end(),
                            (uint32_t)w->m_selected_command)) {
        w->m_selected_command = -1;
    }

    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollX |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("fi_commands_tbl", 7, flags,
                          ImVec2(0, 260.0f * g_viewport_mgr.m_scale))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Seq");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("DMA Instance:GET");
        ImGui::TableSetupColumn("Phys Addr");
        ImGui::TableSetupColumn("Raw");
        ImGui::TableSetupColumn("Decoded", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Origin");
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int)rows.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd;
                 row++) {
                uint32_t i = rows[row];
                const FICommandRec *rec = &cap->commands.recs[i];
                char seq[24];
                char decoded[160];
                snprintf(seq, sizeof(seq), "#%u", rec->seq);
                fi_format_command(rec, decoded, sizeof(decoded));

                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(
                        seq, (int)i == w->m_selected_command,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    w->m_selected_command = (int)i;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(fi_command_kind_name(rec->kind));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("0x%08x:0x%08x", rec->dma_instance, rec->dma_get);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("0x%llx", (unsigned long long)rec->phys_addr);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("0x%08x", rec->raw);
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(decoded);
                ImGui::TableSetColumnIndex(6);
                ImGui::TextUnformatted(fi_confidence_name(rec->confidence));
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();

    if (w->m_selected_command < 0) {
        ImGui::TextDisabled("Select a command for provenance details.");
        return;
    }
    const FICommandRec *rec = &cap->commands.recs[w->m_selected_command];
    char decoded[160];
    fi_format_command(rec, decoded, sizeof(decoded));
    if (ImGui::Button("Copy command")) {
        char text[512];
        snprintf(text, sizeof(text),
                 "command=#%u kind=%s dma_instance=0x%08x dma_get=0x%08x "
                 "phys=0x%llx raw=0x%08x %s confidence=%s writer_node=%u",
                 rec->seq, fi_command_kind_name(rec->kind), rec->dma_instance,
                 rec->dma_get, (unsigned long long)rec->phys_addr, rec->raw,
                 decoded, fi_confidence_name(rec->confidence),
                 rec->writer_node);
        ImGui::SetClipboardText(text);
    }
    if (rec->kind == FI_CMD_METHOD_PARAM &&
        rec->data.parameter.packet != FI_COMMAND_INVALID) {
        ImGui::SameLine();
        uint32_t packet = rec->data.parameter.packet;
        if (packet < cap->commands.num_recs &&
            cap->commands.recs[packet].kind == FI_CMD_METHOD_HEADER) {
            if (ImGui::Button("Select packet header")) {
                w->m_selected_command = (int)packet;
            }
        } else {
            ImGui::TextColored(ImVec4(1, 0.6f, 0, 1),
                               "Invalid packet reference #%u", packet);
        }
    }
    if (rec->confidence == FI_ORIG_ATTRIBUTED ||
        rec->confidence == FI_ORIG_PARTIAL) {
        ImGui::Separator();
        ImGui::Text("Writer node %u (%s)", rec->writer_node,
                    fi_confidence_name(rec->confidence));
        fi_render_call_chain(cap, rec->writer_node);
    } else {
        ImGui::TextDisabled("Writer origin unavailable (%s).",
                            fi_confidence_name(rec->confidence));
    }
}

static void fi_readable_commands_tab(FrameInspectorWindow *w,
                                     const FICapture *cap,
                                     const FIMethodBatch *batch)
{
    if (!batch) {
        ImGui::TextDisabled("No decoded operations for this event.");
        return;
    }

    std::vector<uint32_t> rows;
    uint32_t raw_methods = 0;
    if (!fi_batch_command_rows(cap, batch, &rows, &raw_methods)) {
        ImGui::TextColored(
            ImVec4(1, 0.6f, 0, 1),
            "Command/method logs do not align; decoding is unavailable.");
        return;
    }

    std::vector<uint32_t> operations;
    operations.reserve(rows.size());
    for (uint32_t row : rows) {
        if (cap->commands.recs[row].kind != FI_CMD_METHOD_HEADER) {
            operations.push_back(row);
        }
    }

    ImGui::Text("Batch event #%u: %u readable operations",
                batch->batch_event, (uint32_t)operations.size());
    ImGui::SameLine();
    HelpMarker("One row represents one executed method parameter or PFIFO "
               "control-flow operation. Packet headers are intentionally "
               "hidden. Writer symbols use the loaded IDA map and the "
               "capture's immutable guest call paths.");
    if (raw_methods) {
        ImGui::TextDisabled("%u raw lookahead record(s) could not be decoded.",
                            raw_methods);
    }
    if (operations.empty()) {
        ImGui::TextDisabled("No readable operations matched this batch.");
        return;
    }
    if (w->m_selected_command < 0 ||
        !std::binary_search(operations.begin(), operations.end(),
                            (uint32_t)w->m_selected_command)) {
        w->m_selected_command = -1;
    }

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollX |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("fi_readable_commands_tbl", 5, flags,
                          ImVec2(0, 280.0f * g_viewport_mgr.m_scale))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Seq");
        ImGui::TableSetupColumn("Operation");
        ImGui::TableSetupColumn("Meaning", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Writer Symbol");
        ImGui::TableSetupColumn("Origin");
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int)operations.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd;
                 row++) {
                uint32_t command_idx = operations[row];
                const FICommandRec *rec = &cap->commands.recs[command_idx];
                char seq[24];
                char operation[160];
                char meaning[192];
                char writer[192];
                snprintf(seq, sizeof(seq), "#%u", rec->seq);
                fi_format_readable_command(rec, operation, sizeof(operation),
                                           meaning, sizeof(meaning));
                fi_command_writer_symbol(cap, rec, writer, sizeof(writer));

                ImGui::PushID((int)command_idx);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(
                        seq, (int)command_idx == w->m_selected_command,
                        ImGuiSelectableFlags_SpanAllColumns)) {
                    w->m_selected_command = (int)command_idx;
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(operation);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(meaning);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(writer);
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(fi_confidence_name(rec->confidence));
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    if (w->m_selected_command < 0) {
        ImGui::TextDisabled(
            "Select an operation to inspect its guest writer call path.");
        return;
    }
    const FICommandRec *rec = &cap->commands.recs[w->m_selected_command];
    char operation[160];
    char meaning[192];
    char writer[192];
    fi_format_readable_command(rec, operation, sizeof(operation), meaning,
                               sizeof(meaning));
    fi_command_writer_symbol(cap, rec, writer, sizeof(writer));
    if (ImGui::Button("Copy decoded operation")) {
        char text[768];
        snprintf(text, sizeof(text),
                 "command=#%u operation=%s %s writer=%s confidence=%s "
                 "phys=0x%llx raw=0x%08x",
                 rec->seq, operation, meaning, writer,
                 fi_confidence_name(rec->confidence),
                 (unsigned long long)rec->phys_addr, rec->raw);
        ImGui::SetClipboardText(text);
    }
    if (rec->confidence == FI_ORIG_ATTRIBUTED ||
        rec->confidence == FI_ORIG_PARTIAL) {
        ImGui::Separator();
        ImGui::Text("Guest writer: %s", writer);
        fi_render_call_chain(cap, rec->writer_node);
    } else {
        ImGui::TextDisabled("Guest writer unavailable (%s).",
                            fi_confidence_name(rec->confidence));
    }
}

static void fi_state_tab(const FICapture *cap, int event_idx)
{
    const FIResource *regs = fi_find_batch_resource(cap, event_idx, FI_RESK_REGS);
    if (!regs) {
        ImGui::TextDisabled("No register-file resource for this event.");
        return;
    }
    ImGui::Text("Registers: %u bytes (raw dword dump; named-register "
               "decoding is out of scope)", regs->len);
    ImGui::BeginChild("fi_state_regs", ImVec2(0, 0), true);
    ImGui::PushFont(g_font_mgr.m_fixed_width_font);
    const uint8_t *base = cap->resources.blob + regs->off;
    uint32_t nwords = regs->len / 4;
    uint32_t nrows = (nwords + 7) / 8;
    ImGuiListClipper clipper;
    clipper.Begin((int)nrows);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            uint32_t first = (uint32_t)row * 8;
            char line[128];
            int p = snprintf(line, sizeof(line), "0x%04x:", first * 4);
            for (uint32_t k = 0; k < 8 && first + k < nwords; k++) {
                uint32_t word;
                memcpy(&word, base + (uint64_t)(first + k) * 4, 4);
                p += snprintf(line + p, sizeof(line) - p, " %08x", word);
            }
            ImGui::TextUnformatted(line);
        }
    }
    ImGui::PopFont();
    ImGui::EndChild();
}

static void fi_resources_tab(const FICapture *cap, int event_idx)
{
    ImGui::TextDisabled(
        "Texture/palette bytes are shown as metadata only; decoded inline "
        "previews are a follow-up.");
    ImGui::Separator();

    if (event_idx < 0) {
        ImGui::TextDisabled("Select an event.");
        return;
    }
    bool any = false;
    for (uint32_t i = 0; i < cap->num_batch_res; i++) {
        const FIBatchResRef *ref = &cap->batch_res[i];
        if (ref->event != (uint32_t)event_idx) {
            continue;
        }
        if (ref->res_id >= cap->resources.num_res) {
            continue;
        }
        const FIResource *r = &cap->resources.res[ref->res_id];
        any = true;
        ImGui::PushID((int)i);
        switch (r->kind) {
        case FI_RESK_REGS:
            ImGui::TextUnformatted("Registers (32 KiB) -- see State tab");
            break;
        case FI_RESK_TEXTURE: {
            /* meta pack from pgraph_gl_bind_textures() (draw.c): color_format
             * in the high 32 bits, width in bits 16-31, height in bits 0-15. */
            uint32_t color_format = (uint32_t)(r->meta >> 32);
            uint32_t width = (uint32_t)((r->meta >> 16) & 0xFFFF);
            uint32_t height = (uint32_t)(r->meta & 0xFFFF);
            ImGui::Text("Texture: %u bytes, format=0x%02x %ux%u", r->len,
                       color_format, width, height);
            break;
        }
        case FI_RESK_PALETTE:
            ImGui::Text("Palette: %u bytes", r->len);
            break;
        case FI_RESK_TEXTURE_RTREF:
            ImGui::TextColored(
                ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                "Render-target texture -> surface @ 0x%llx (dependency)",
                (unsigned long long)r->meta);
            break;
        default:
            ImGui::Text("Unknown resource kind %u, %u bytes", r->kind, r->len);
            break;
        }
        ImGui::PopID();
    }
    if (!any) {
        ImGui::TextDisabled("No resources referenced by this event.");
    }
}

static int fi_pixel_history_until(const FIColorHist *ch, uint32_t pixel_index,
                                  uint32_t max_event_id,
                                  std::vector<FIColorTouch> *touches)
{
    touches->resize(ch->num_events);
    int n = fi_colorhist_pixel_history(ch, pixel_index, touches->data(),
                                       (int)touches->size());
    while (n > 0 && (*touches)[n - 1].event_id > max_event_id) {
        n--;
    }
    touches->resize(n);
    return n;
}

static void fi_pixels_tab(const FICapture *cap, int pinned_gen,
                          int pinned_pixel,
                          uint32_t max_event_id)
{
    if (pinned_pixel < 0) {
        ImGui::TextDisabled(
            "Hover + click a pixel in the frame to pin its history.");
        return;
    }
    if (pinned_gen < 0 || !cap->hist ||
        (uint32_t)pinned_gen >= cap->hist_count) {
        ImGui::TextDisabled("No colour history available for this surface.");
        return;
    }
    const FIColorHist *ch = &cap->hist[pinned_gen];
    if ((uint32_t)pinned_pixel >= ch->npix) {
        ImGui::TextDisabled("Pinned pixel is out of range for this capture.");
        return;
    }

    std::vector<FIColorTouch> touches;
    int n = fi_pixel_history_until(ch, (uint32_t)pinned_pixel, max_event_id,
                                   &touches);
    ImGui::Text("Pixel index %d: %d touch(es)", pinned_pixel, n);
    if (n == 0) {
        ImGui::TextDisabled("No colour-change history for this pixel.");
        return;
    }
    if (ImGui::BeginTable("fi_pixels_tbl", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Event");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Before -> After");
        ImGui::TableHeadersRow();
        ImVec2 sw(16.0f * g_viewport_mgr.m_scale, 16.0f * g_viewport_mgr.m_scale);
        for (int i = 0; i < n; i++) {
            const FIColorTouch *t = &touches[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("#%u", t->event_id);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(t->event_id < cap->events.count ?
                                   ev_kind_name(cap->events.events[t->event_id].kind) :
                                   "?");
            ImGui::TableSetColumnIndex(2);
            ImGui::ColorButton("before", fi_rgba_to_imvec4(t->before),
                              ImGuiColorEditFlags_NoTooltip |
                                  ImGuiColorEditFlags_NoBorder,
                              sw);
            ImGui::SameLine();
            ImGui::TextUnformatted("->");
            ImGui::SameLine();
            ImGui::ColorButton("after", fi_rgba_to_imvec4(t->after),
                              ImGuiColorEditFlags_NoTooltip |
                                  ImGuiColorEditFlags_NoBorder,
                              sw);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

/* Task 6: manual "who wrote this guest physical address" lookup. The address
 * tag still comes from the live tag map, so generation matching is required;
 * the resulting node is resolved from this capture's immutable origins. */
static void fi_address_lookup_panel(FrameInspectorWindow *w,
                                    const FICapture *cap)
{
    if (xemu_frameinspect_generation() != cap->origin_generation) {
        ImGui::TextDisabled(
            "Address origins unavailable: the inspector has been re-armed.");
        return;
    }
    ImGui::TextUnformatted("Who wrote a guest physical address");
    ImGui::SameLine();
    HelpMarker("The tag lookup uses the live tag map and is valid only before "
               "re-arming. Its call chain comes from this capture. Address is "
               "guest physical (RAM-relative).");

    bool submit = ImGui::InputText(
        "Guest addr (hex)", w->m_lookup_addr, sizeof(w->m_lookup_addr),
        ImGuiInputTextFlags_CharsHexadecimal |
            ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Lookup")) {
        submit = true;
    }

    if (submit) {
        uint64_t addr = strtoull(w->m_lookup_addr, nullptr, 16);
        w->m_lookup_result_addr = addr;
        w->m_lookup_tag = xemu_frameinspect_lookup_tag(addr);
        w->m_lookup_done = true;
    }

    if (!w->m_lookup_done) {
        return;
    }

    ImGui::Text("Address: 0x%llx",
               (unsigned long long)w->m_lookup_result_addr);
    if (w->m_lookup_tag == 0) {
        ImGui::TextDisabled(
            "unattributed (no tagged writer, or written before arming)");
        return;
    }

    bool partial = (w->m_lookup_tag & FI_TAG_PARTIAL_BIT) != 0;
    uint32_t node_id = fi_tag_to_node(w->m_lookup_tag);
    ImGui::TextColored(partial ? ImVec4(0.9f, 0.9f, 0.2f, 1) :
                                 ImVec4(0.3f, 0.9f, 0.3f, 1),
                       "%s (node %u)", partial ? "partial" : "attributed",
                       node_id);
    fi_render_call_chain(cap, node_id);
}

static GLuint fi_upload_rgba_texture(const uint32_t *image, uint32_t width,
                                     uint32_t height, GLuint tex = 0)
{
    if (!tex) {
        glGenTextures(1, &tex);
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width,
                 (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

/* Builds the composed scanout texture, falling back to cap->hist[gen].
 * Always deletes any existing texture first (no per-capture/per-scrub
 * leak). Leaves m_frame_tex == 0 on any failure (gen out of range, colour
 * history not initialized, event_index out of range, or reconstruct failure). */
void FrameInspectorWindow::UploadFrame(const FICapture *cap, int gen,
                                        uint32_t event_index)
{
    ReleaseTexture();

    const uint32_t *image = nullptr;
    uint32_t image_w = 0, image_h = 0;
    const FIEvent *scanout = fi_find_scanout_event(cap);
    if (scanout && scanout->a3 < cap->resources.num_res) {
        const FIResource *res = &cap->resources.res[scanout->a3];
        uint32_t width = (uint32_t)(res->meta >> 32);
        uint32_t height = (uint32_t)res->meta;
        uint64_t bytes = (uint64_t)width * height * sizeof(uint32_t);
        if (res->kind == FI_RESK_SCANOUT_RGBA && width && height &&
            bytes == res->len && res->off <= cap->resources.blob_used &&
            res->len <= cap->resources.blob_used - res->off) {
            image = (const uint32_t *)(cap->resources.blob + res->off);
            image_w = width;
            image_h = height;
        }
    }

    if (gen < 0 || !cap->hist || (uint32_t)gen >= cap->hist_count) {
        if (!image) {
            return;
        }
    }

    std::vector<uint32_t> pixels;
    if (!image) {
        const FIColorHist *ch = &cap->hist[gen];
        if (ch->width == 0 || ch->height == 0 ||
            event_index >= fi_colorhist_num_events(ch)) {
            return;
        }
        pixels.resize((size_t)ch->width * ch->height);
        if (!fi_colorhist_reconstruct(ch, event_index, pixels.data())) {
            return;
        }
        image = pixels.data();
        image_w = ch->width;
        image_h = ch->height;
    }

    m_frame_tex = fi_upload_rgba_texture(image, image_w, image_h);
    m_frame_w = (int)image_w;
    m_frame_h = (int)image_h;
    m_frame_gen = gen;
}

void FrameInspectorWindow::SetTargetEvent(const FICapture *cap, int gen,
                                           uint32_t event_index,
                                           uint32_t event_id)
{
    m_isolate_w = m_isolate_h = 0;
    m_target_w = m_target_h = 0;
    m_target_gen = -1;
    m_target_bounds_valid = false;
    m_target_changed_pixels = 0;

    if (gen < 0 || !cap->hist || (uint32_t)gen >= cap->hist_count) {
        return;
    }
    const FIColorHist *ch = &cap->hist[gen];
    if (!ch->width || !ch->height || event_index >= ch->num_events ||
        ch->events[event_index].event_id != event_id) {
        return;
    }

    const FIColorEvent *event = &ch->events[event_index];
    for (uint32_t i = 0; i < event->run_count; i++) {
        const FIColorRun *run = &ch->runs[event->run_first + i];
        uint32_t first_y = run->start / ch->width;
        uint32_t last = run->start + run->len - 1;
        uint32_t last_y = last / ch->width;
        uint32_t first_x = run->start % ch->width;
        uint32_t last_x = last % ch->width;
        uint32_t run_min_x = first_y == last_y ? first_x : 0;
        uint32_t run_max_x = first_y == last_y ? last_x : ch->width - 1;
        if (!m_target_bounds_valid) {
            m_target_min_x = run_min_x;
            m_target_max_x = run_max_x;
            m_target_min_y = first_y;
            m_target_max_y = last_y;
            m_target_bounds_valid = true;
        } else {
            if (run_min_x < m_target_min_x) m_target_min_x = run_min_x;
            if (run_max_x > m_target_max_x) m_target_max_x = run_max_x;
            if (first_y < m_target_min_y) m_target_min_y = first_y;
            if (last_y > m_target_max_y) m_target_max_y = last_y;
        }
        m_target_changed_pixels += run->len;
    }

    m_target_w = (int)ch->width;
    m_target_h = (int)ch->height;
    m_target_gen = gen;

    if (m_target_bounds_valid) {
        uint32_t crop_w = m_target_max_x - m_target_min_x + 1;
        uint32_t crop_h = m_target_max_y - m_target_min_y + 1;
        std::vector<uint32_t> isolated((size_t)crop_w * crop_h,
                                       0xff202020u);
        for (uint32_t i = 0; i < event->run_count; i++) {
            const FIColorRun *run = &ch->runs[event->run_first + i];
            for (uint32_t k = 0; k < run->len; k++) {
                uint32_t source = run->start + k;
                uint32_t x = source % ch->width;
                uint32_t y = source / ch->width;
                uint32_t dest = (y - m_target_min_y) * crop_w +
                                (x - m_target_min_x);
                isolated[dest] =
                    ch->colors[run->color_off + run->len + k] |
                    0xff000000u;
            }
        }
        m_isolate_tex = fi_upload_rgba_texture(
            isolated.data(), crop_w, crop_h, (GLuint)m_isolate_tex);
        m_isolate_w = (int)crop_w;
        m_isolate_h = (int)crop_h;
    }
    if (!m_isolate_w || !m_isolate_h) {
        m_isolate_batch = false;
    }
}

void FrameInspectorWindow::ReleaseTexture()
{
    if (m_frame_tex) {
        GLuint tex = (GLuint)m_frame_tex;
        glDeleteTextures(1, &tex);
    }
    if (m_isolate_tex) {
        GLuint tex = (GLuint)m_isolate_tex;
        glDeleteTextures(1, &tex);
    }
    m_frame_tex = 0;
    m_frame_w = 0;
    m_frame_h = 0;
    m_frame_gen = -1;
    m_target_w = m_target_h = 0;
    m_target_gen = -1;
    m_target_bounds_valid = false;
    m_target_changed_pixels = 0;
    m_isolate_tex = 0;
    m_isolate_w = m_isolate_h = 0;
}

static void fi_clear_target(FrameInspectorWindow *w)
{
    w->m_target_w = w->m_target_h = 0;
    w->m_target_gen = -1;
    w->m_view_event = -1;
    w->m_timeline_idx = -1;
    w->m_target_bounds_valid = false;
    w->m_target_changed_pixels = 0;
    w->m_isolate_w = w->m_isolate_h = 0;
    w->m_isolate_batch = false;
}

static bool fi_show_event(FrameInspectorWindow *w, const FICapture *cap,
                          uint32_t event_id)
{
    int gen = -1;
    uint32_t index = 0;
    if (!fi_find_history_event(cap, event_id, &gen, &index)) {
        fi_clear_target(w);
        return false;
    }
    w->m_timeline_idx = (int)index;
    w->m_view_event = (int)event_id;
    w->SetTargetEvent(cap, gen, index, event_id);
    return w->m_target_gen >= 0;
}

static void fi_show_final_frame(FrameInspectorWindow *w, const FICapture *cap)
{
    int gen = fi_find_scanout_gen(cap);
    uint32_t num_ev =
        gen >= 0 && cap->hist && (uint32_t)gen < cap->hist_count ?
            fi_colorhist_num_events(&cap->hist[gen]) : 0;
    w->UploadFrame(cap, gen, num_ev ? num_ev - 1 : 0);
}

/* Resolve a render-target texture reference to the latest earlier writer at
 * that VRAM address. Surface generations are capture-local and event IDs are
 * chronological, so this also handles address rebinds without guessing a
 * future generation. */
static bool fi_resolve_rtref_producer(const FICapture *cap, uint64_t address,
                                      uint32_t consumer_event,
                                      uint32_t *gen_out,
                                      uint32_t *event_out)
{
    for (uint32_t i = consumer_event; i-- > 0;) {
        uint32_t gen = cap->events.events[i].surface_gen;
        if (gen != FI_SURFGEN_INVALID && gen < cap->surfaces.num_gens &&
            cap->surfaces.gens[gen].key.color &&
            cap->surfaces.gens[gen].key.addr == address) {
            *gen_out = gen;
            *event_out = i;
            return true;
        }
    }
    return false;
}

static bool fi_surface_is_ancestor(const std::vector<uint32_t> &ancestors,
                                   uint32_t gen)
{
    for (uint32_t ancestor : ancestors) {
        if (ancestor == gen) {
            return true;
        }
    }
    return false;
}

static void fi_render_surface_writes(FrameInspectorWindow *w,
                                     const FICapture *cap, uint32_t gen,
                                     uint32_t through_event, int depth,
                                     std::vector<uint32_t> *ancestors)
{
    for (uint32_t event_id = 0;
         event_id < cap->events.count && event_id <= through_event;
         event_id++) {
        const FIEvent *event = &cap->events.events[event_id];
        if (event->surface_gen != gen) {
            continue;
        }

        std::vector<uint64_t> dependencies;
        for (uint32_t i = 0; i < cap->num_batch_res; i++) {
            const FIBatchResRef *ref = &cap->batch_res[i];
            if (ref->event != event_id ||
                ref->res_id >= cap->resources.num_res) {
                continue;
            }
            const FIResource *res = &cap->resources.res[ref->res_id];
            if (res->kind != FI_RESK_TEXTURE_RTREF) {
                continue;
            }
            bool duplicate = false;
            for (uint64_t address : dependencies) {
                duplicate |= address == res->meta;
            }
            if (!duplicate) {
                dependencies.push_back(res->meta);
            }
        }

        ImGui::PushID((int)event_id);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if ((int)event_id == w->m_selected_event) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (dependencies.empty() || depth >= 8) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        bool open = ImGui::TreeNodeEx(
            "event", flags, "#%u %s", event_id, ev_kind_name(event->kind));
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            w->m_selected_event = (int)event_id;
            w->m_pinned_pixel = -1;
            w->m_pinned_gen = -1;
            fi_show_event(w, cap, event_id);
        }

        if (open && !(flags & ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
            if (depth >= 8) {
                ImGui::TextDisabled("dependency depth limit reached");
            } else {
                for (uint32_t i = 0; i < (uint32_t)dependencies.size(); i++) {
                    uint32_t producer_gen = FI_SURFGEN_INVALID;
                    uint32_t producer_event = FI_EVENT_INVALID;
                    ImGui::PushID((int)i);
                    if (!fi_resolve_rtref_producer(
                            cap, dependencies[i], event_id, &producer_gen,
                            &producer_event)) {
                        ImGui::TextDisabled("unresolved RT @ 0x%llx",
                            (unsigned long long)dependencies[i]);
                    } else {
                        const FISurfaceKey *key =
                            &cap->surfaces.gens[producer_gen].key;
                        bool cycle = fi_surface_is_ancestor(*ancestors,
                                                            producer_gen);
                        ImGuiTreeNodeFlags dep_flags =
                            ImGuiTreeNodeFlags_SpanAvailWidth |
                            ImGuiTreeNodeFlags_OpenOnArrow;
                        if (cycle) {
                            dep_flags |= ImGuiTreeNodeFlags_Leaf |
                                         ImGuiTreeNodeFlags_NoTreePushOnOpen;
                        }
                        bool dep_open = ImGui::TreeNodeEx(
                            "surface", dep_flags,
                            "samples gen %u, %ux%u @ 0x%llx%s",
                            producer_gen, key->width, key->height,
                            (unsigned long long)key->addr,
                            cycle ? " [cycle]" : "");
                        if (ImGui::IsItemClicked() &&
                            !ImGui::IsItemToggledOpen()) {
                            w->m_selected_event = (int)producer_event;
                            w->m_pinned_pixel = -1;
                            w->m_pinned_gen = -1;
                            fi_show_event(w, cap, producer_event);
                        }
                        if (dep_open && !cycle) {
                            ancestors->push_back(producer_gen);
                            fi_render_surface_writes(
                                w, cap, producer_gen, producer_event,
                                depth + 1, ancestors);
                            ancestors->pop_back();
                            ImGui::TreePop();
                        }
                    }
                    ImGui::PopID();
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

static void fi_render_dependency_tree(FrameInspectorWindow *w,
                                      const FICapture *cap)
{
    const FIEvent *scanout = fi_find_scanout_event(cap);
    if (!scanout || scanout->surface_gen == FI_SURFGEN_INVALID ||
        scanout->surface_gen >= cap->surfaces.num_gens) {
        ImGui::TextDisabled("No tracked scanout surface.");
        return;
    }
    uint32_t scanout_event = (uint32_t)(scanout - cap->events.events);
    uint32_t gen = scanout->surface_gen;
    const FISurfaceKey *key = &cap->surfaces.gens[gen].key;
    std::vector<uint32_t> event_counts(cap->surfaces.num_gens, 0);
    uint32_t untracked_events = 0;
    for (uint32_t i = 0; i < cap->events.count; i++) {
        uint32_t event_gen = cap->events.events[i].surface_gen;
        if (event_gen != FI_SURFGEN_INVALID &&
            event_gen < cap->surfaces.num_gens) {
            event_counts[event_gen]++;
        } else if (cap->events.events[i].kind != FI_EV_SCANOUT) {
            untracked_events++;
        }
    }
    uint32_t other_targets = 0;
    for (uint32_t i = 0; i < (uint32_t)event_counts.size(); i++) {
        if (i != gen && event_counts[i]) {
            other_targets++;
        }
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen |
        ImGuiTreeNodeFlags_SpanAvailWidth;
    if (ImGui::TreeNodeEx("final", flags,
                          "Presented target: gen %u, %ux%u", gen,
                          key->width, key->height)) {
        std::vector<uint32_t> ancestors = { gen };
        fi_render_surface_writes(w, cap, gen, scanout_event, 0, &ancestors);
        ImGui::TreePop();
    }

    if (other_targets) {
        if (ImGui::TreeNodeEx("other_targets", flags,
                              "Other captured targets (%u)",
                              other_targets)) {
            for (uint32_t other_gen = 0;
                 other_gen < (uint32_t)event_counts.size(); other_gen++) {
                if (other_gen == gen || !event_counts[other_gen]) {
                    continue;
                }
                const FISurfaceKey *other_key =
                    &cap->surfaces.gens[other_gen].key;
                ImGui::PushID((int)other_gen);
                ImGuiTreeNodeFlags target_flags =
                    ImGuiTreeNodeFlags_SpanAvailWidth;
                if (other_targets == 1) {
                    target_flags |= ImGuiTreeNodeFlags_DefaultOpen;
                }
                if (ImGui::TreeNodeEx(
                        "target", target_flags,
                        "gen %u: %ux%u, %u writes [not linked]", other_gen,
                        other_key->width, other_key->height,
                        event_counts[other_gen])) {
                    std::vector<uint32_t> ancestors = { other_gen };
                    fi_render_surface_writes(w, cap, other_gen,
                                             scanout_event, 0, &ancestors);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }

    if (untracked_events &&
        ImGui::TreeNodeEx("untracked", ImGuiTreeNodeFlags_SpanAvailWidth,
                          "No tracked colour target (%u)",
                          untracked_events)) {
        for (uint32_t event_id = 0; event_id < cap->events.count; event_id++) {
            const FIEvent *event = &cap->events.events[event_id];
            if (event->surface_gen != FI_SURFGEN_INVALID ||
                event->kind == FI_EV_SCANOUT) {
                continue;
            }
            ImGui::PushID((int)event_id);
            char label[64];
            snprintf(label, sizeof(label), "#%u %s", event_id,
                     ev_kind_name(event->kind));
            if (ImGui::Selectable(
                    label, (int)event_id == w->m_selected_event,
                    0, ImVec2(0, 0))) {
                w->m_selected_event = (int)event_id;
                w->m_pinned_pixel = -1;
                w->m_pinned_gen = -1;
                fi_show_event(w, cap, event_id);
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

/* Scrubs the colour history of whichever surface is in the viewport. Moving
 * it also selects the corresponding global event so the image and detail
 * tabs cannot silently describe different draws. */
static void fi_timeline_scrubber(FrameInspectorWindow *w, const FICapture *cap)
{
    if (w->m_target_gen < 0 ||
        (uint32_t)w->m_target_gen >= cap->hist_count) {
        return;
    }
    const FIColorHist *ch = &cap->hist[w->m_target_gen];
    uint32_t num_ev = fi_colorhist_num_events(ch);
    if (num_ev == 0) {
        return;
    }

    int idx = (w->m_timeline_idx < 0) ? (int)(num_ev - 1) : w->m_timeline_idx;
    if (num_ev > 1 &&
        ImGui::SliderInt("Surface timeline", &idx, 0, (int)num_ev - 1)) {
        uint32_t event_id = ch->events[idx].event_id;
        w->m_selected_event = (int)event_id;
        fi_show_event(w, cap, event_id);
    }
    uint32_t shown =
        (w->m_timeline_idx < 0) ? num_ev - 1 : (uint32_t)w->m_timeline_idx;
    /* `shown` is a dense index into this surface's colour history, not a
     * global event-log index -- map it through the colorhist event's
     * event_id before indexing cap->events, else this names the wrong
     * event's kind. */
    const char *kind = "?";
    if (shown < ch->num_events) {
        uint32_t global_ev = ch->events[shown].event_id;
        if (global_ev < cap->events.count) {
            kind = ev_kind_name(cap->events.events[global_ev].kind);
        }
    }
    uint32_t global_ev = shown < ch->num_events ?
                             ch->events[shown].event_id : FI_EVENT_INVALID;
    ImGui::Text("surface event %u / %u: #%u %s", shown, num_ev - 1,
                global_ev, kind);
}

static void fi_draw_image_bounds(ImVec2 origin, ImVec2 size,
                                  uint32_t width, uint32_t height,
                                  uint32_t min_x, uint32_t min_y,
                                  uint32_t max_x, uint32_t max_y, bool flip_y,
                                  ImU32 color)
{
    if (!width || !height) {
        return;
    }
    float x1 = origin.x + size.x * min_x / width;
    float x2 = origin.x + size.x * (max_x + 1) / width;
    float y1;
    float y2;
    if (flip_y) {
        y1 = origin.y + size.y * (height - (max_y + 1)) / height;
        y2 = origin.y + size.y * (height - min_y) / height;
    } else {
        y1 = origin.y + size.y * min_y / height;
        y2 = origin.y + size.y * (max_y + 1) / height;
    }
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2),
                        IM_COL32(40, 150, 255, 55));
    draw->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), color, 0.0f, 0, 3.0f);
}

void FrameInspectorWindow::Draw()
{
    const FICapture *cap = xemu_frameinspect_capture_acquire();
    if (!cap) {
        return; /* nothing published */
    }

    /* Auto-open once when a new capture publishes. Identified by pointer,
     * not event count: two captures can have identical event counts (e.g.
     * the same scene re-captured), which an event-count comparison would
     * miss, leaving the old frame image/selection displayed alongside the
     * new capture's metadata. The window holds a ref to `cap` for the rest
     * of Draw(), so this pointer is stable for that duration, and a
     * different published capture is a distinct malloc'd object. */
    bool new_capture = cap->capture_id != m_last_seen_capture;
    if (new_capture) {
        m_last_seen_capture = cap->capture_id;
        m_is_open = true;
    }

    if (!m_is_open) {
        xemu_frameinspect_capture_release(cap);
        return;
    }

    /* A new/smaller capture may have fewer events than the prior selection. */
    if (m_selected_event >= (int)cap->events.count) {
        m_selected_event = -1;
    }

    if (new_capture) {
        m_selected_event = -1;
        m_pinned_pixel = -1; /* stale index into a possibly-differently-sized image */
        m_pinned_gen = -1;
        m_selected_rec = -1; /* stale index into the old capture's methods log */
        m_selected_command = -1;
        m_timeline_idx = -1;
        m_view_event = -1;
        m_isolate_batch = false;
        m_lookup_done = false;
    }
    if (new_capture) {
        fi_show_final_frame(this, cap);
    } else if (m_frame_tex == 0) {
        fi_show_final_frame(this, cap);
    }
    if (!new_capture && m_view_event >= 0 && m_target_gen < 0) {
        fi_show_event(this, cap, (uint32_t)m_view_event);
    }

    ImGui::SetNextWindowSize(ImVec2(900.0f * g_viewport_mgr.m_scale,
                                    620.0f * g_viewport_mgr.m_scale),
                             ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Frame Inspector", &m_is_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("Captured frame: %u events, %u surfaces, %u commands, "
                    "%u methods, %u resources%s", cap->events.count,
                    cap->surfaces.num_gens, cap->commands.num_recs,
                    cap->methods.num_recs, cap->resources.num_res,
                    (cap->truncated || cap->events.truncated ||
                     cap->surfaces.truncated || cap->resources.truncated ||
                     cap->methods.truncated || cap->commands.truncated) ?
                        " [TRUNCATED]" : "");
        ImGui::Separator();
        ImGui::Text("Events: %u   Surfaces: %u   Resources: %u",
                    cap->events.count, cap->surfaces.num_gens,
                    cap->resources.num_res);
        ImGui::Text("Origins: %u nodes, %u argument sets%s",
                    cap->origins.num_nodes, cap->origins.num_argsets,
                    cap->origins.truncation ? " [TRUNCATED]" : "");
        if (cap->truncated || cap->events.truncated ||
            cap->surfaces.truncated || cap->resources.truncated ||
            cap->methods.truncated || cap->commands.truncated) {
            ImGui::TextColored(
                ImVec4(1, 0.6f, 0, 1),
                "[TRUNCATED] capture hit a cap/budget; some data is missing.");
        }
        /* Symbols: resolve guest addresses to IDA function names. Auto-loaded
         * once from the persisted path; reloadable via the picker. */
        if (!g_fi_symbols_autoload_tried) {
            g_fi_symbols_autoload_tried = true;
            const char *sp = g_config.general.frameinspect_symbols;
            if (sp && sp[0]) {
                fi_symbols_load(&g_fi_symbols, sp, NULL);
            }
        }
        if (g_fi_symbols.count) {
            ImGui::Text("Symbols: %u loaded", g_fi_symbols.count);
        } else {
            ImGui::TextDisabled(
                "Symbols: none (addresses shown as hex). Load an IDA symbol "
                "map (addr [size] name per line).");
        }
        FilePicker(
            "Load symbols", g_config.general.frameinspect_symbols, nullptr, 0,
            false, [](const char *path) {
                xemu_settings_set_string(&g_config.general.frameinspect_symbols,
                                         path);
                fi_symbols_load(&g_fi_symbols, path, NULL);
            });
        ImGui::Separator();
        const FIEvent *scanout = fi_find_scanout_event(cap);
        if (scanout && (scanout->a2 & FI_SCANOUT_PVIDEO)) {
            ImGui::TextColored(
                ImVec4(1, 0.6f, 0, 1),
                "PVIDEO is present in the composed frame; covered pixels "
                "cannot be attributed to PGRAPH draws.");
        }
        if (scanout && (scanout->a2 & FI_SCANOUT_TRANSFORMED)) {
            ImGui::TextColored(
                ImVec4(1, 0.6f, 0, 1),
                "Interlaced/non-1:1 scanout: composed pixels cannot be mapped "
                "exactly to source history.");
        }
        ImGui::BeginChild("fi_events", ImVec2(260.0f * g_viewport_mgr.m_scale, 0),
                          true);
        ImGui::TextUnformatted("Render dependencies");
        ImGui::SameLine();
        HelpMarker("The final scanout surface contains its writes. A draw "
                   "that samples an earlier render target contains that "
                   "producer surface as a child. Targets with no proven "
                   "path to scanout are listed separately so every captured "
                   "draw remains available.");
        ImGui::Separator();
        fi_render_dependency_tree(this, cap);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("fi_detail", ImVec2(0, 0), true);
        int selected_gen = -1;
        uint32_t selected_hist_idx = 0;
        bool selected_has_image =
            m_selected_event >= 0 &&
            fi_find_history_event(cap, (uint32_t)m_selected_event,
                                  &selected_gen, &selected_hist_idx);
        if (m_view_event >= 0 && m_target_gen >= 0) {
            ImGui::TextColored(
                ImVec4(0.3f, 0.9f, 1.0f, 1.0f),
                "Inspecting #%d %s: surface gen %d, %llu changed pixels",
                m_view_event,
                ev_kind_name(cap->events.events[m_view_event].kind),
                m_target_gen,
                (unsigned long long)m_target_changed_pixels);
        } else {
            ImGui::TextDisabled(
                "Select a write in the render tree to inspect its target.");
        }
        if (m_selected_event >= 0 && !selected_has_image) {
            ImGui::TextColored(
                ImVec4(1, 0.6f, 0, 1),
                "Selected event #%d has no captured colour output.",
                m_selected_event);
        }
        uint32_t visible_event_id = UINT32_MAX;
        bool have_target = m_view_event >= 0 && m_target_gen >= 0 &&
                           m_target_w > 0 && m_target_h > 0;
        unsigned view_tex = m_frame_tex;
        int view_w = m_frame_w;
        int view_h = m_frame_h;
        int view_gen = m_frame_gen;
        bool isolate = have_target && m_isolate_batch && m_isolate_tex &&
                       m_isolate_w > 0 && m_isolate_h > 0;
        if (have_target) {
            ImGui::Text(isolate ? "Isolated #%d changed pixels" :
                                  "Final frame with projected gen %d bounds",
                        isolate ? m_view_event : m_target_gen);
            if (m_isolate_tex && m_isolate_w > 0 && m_isolate_h > 0) {
                ImGui::SameLine();
                if (ImGui::SmallButton(isolate ? "Show final frame" :
                                                 "Isolate batch")) {
                    m_isolate_batch = !m_isolate_batch;
                    isolate = m_isolate_batch;
                }
            }
        } else {
            ImGui::TextUnformatted("Composed scanout");
        }
        if (isolate) {
            view_tex = m_isolate_tex;
            view_w = m_isolate_w;
            view_h = m_isolate_h;
            view_gen = -1;
        }

        if (view_tex && view_w > 0 && view_h > 0) {
            float avail_w = ImGui::GetContentRegionAvail().x;
            float canvas_w = std::min(avail_w,
                                      640.0f * g_viewport_mgr.m_scale);
            ImVec2 canvas_size(canvas_w, canvas_w * 480.0f / 640.0f);
            ImGui::InvisibleButton("fi_frame_canvas", canvas_size);
            ImVec2 canvas_min = ImGui::GetItemRectMin();
            ImVec2 canvas_max = ImGui::GetItemRectMax();
            ImDrawList *draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(canvas_min, canvas_max,
                                IM_COL32(24, 24, 24, 255));
            draw->AddRect(canvas_min, canvas_max,
                          IM_COL32(80, 80, 80, 255));

            float image_scale = std::min(canvas_size.x / view_w,
                                         canvas_size.y / view_h);
            ImVec2 image_size(view_w * image_scale, view_h * image_scale);
            ImVec2 image_min(canvas_min.x + (canvas_size.x - image_size.x) * 0.5f,
                             canvas_min.y + (canvas_size.y - image_size.y) * 0.5f);
            ImVec2 image_max(image_min.x + image_size.x,
                             image_min.y + image_size.y);
            draw->AddImage((ImTextureID)(intptr_t)view_tex, image_min,
                           image_max, ImVec2(0, 1), ImVec2(1, 0));
            if (have_target && m_target_bounds_valid && !isolate) {
                fi_draw_image_bounds(
                    image_min, image_size,
                    (uint32_t)m_target_w, (uint32_t)m_target_h,
                    m_target_min_x, m_target_min_y, m_target_max_x,
                    m_target_max_y, false, IM_COL32(255, 210, 40, 255));
            }

            ImVec2 mp = ImGui::GetMousePos();
            bool img_hovered = ImGui::IsItemHovered() &&
                               mp.x >= image_min.x && mp.x < image_max.x &&
                               mp.y >= image_min.y && mp.y < image_max.y;
            bool img_clicked = img_hovered && ImGui::IsItemClicked();
            if (img_hovered && view_gen >= 0 &&
                (uint32_t)view_gen < cap->hist_count) {
                float u = ImClamp((mp.x - image_min.x) / image_size.x,
                                  0.0f, 0.9999f);
                float v = ImClamp((mp.y - image_min.y) / image_size.y,
                                  0.0f, 0.9999f);
                const FIColorHist *ch = &cap->hist[view_gen];
                bool can_attribute = ch->width && ch->height &&
                    view_w == (int)ch->width &&
                    view_h == (int)ch->height && scanout &&
                    !(scanout->a2 &
                      (FI_SCANOUT_PVIDEO | FI_SCANOUT_TRANSFORMED));
                int px = (int)(u * ch->width);
                float source_v = v;
                source_v = ImClamp(source_v, 0.0f, 0.9999f);
                int py = (int)(source_v * ch->height);
                uint32_t pixel_index = (uint32_t)py * ch->width +
                                       (uint32_t)px;
                std::vector<FIColorTouch> touches;
                int n = can_attribute ?
                    fi_pixel_history_until(ch, pixel_index,
                                           visible_event_id, &touches) : 0;

                ImGui::BeginTooltip();
                if (!can_attribute) {
                    ImGui::TextDisabled(
                        "final-to-source pixel mapping was not captured");
                } else {
                    ImGui::Text("surface px (%d, %d)", px, py);
                    if (n == 0) {
                        ImGui::TextDisabled(
                            "no colour-change history for this pixel");
                    }
                    int first = n > 16 ? n - 16 : 0;
                    for (int i = first; i < n; i++) {
                        const FIColorTouch *t = &touches[i];
                        ImGui::Text("#%u %s", t->event_id,
                            t->event_id < cap->events.count ?
                                ev_kind_name(cap->events.events[
                                    t->event_id].kind) : "?");
                    }
                }
                ImGui::EndTooltip();

                if (img_clicked && can_attribute) {
                    if (n > 0) {
                        m_selected_event = (int)touches[n - 1].event_id;
                        fi_show_event(this, cap, touches[n - 1].event_id);
                    }
                    m_pinned_gen = view_gen;
                    m_pinned_pixel = (int)pixel_index;
                }
            }
        } else {
            ImGui::TextDisabled("No captured image for this target.");
        }

        if (have_target) {
            if (isolate) {
                ImGui::TextDisabled(
                    "Changed colour pixels only; same-colour and depth-only writes are absent.");
            }
            if (m_target_bounds_valid) {
                ImGui::Text("projected bounds from target: (%u,%u)-(%u,%u)",
                            m_target_min_x, m_target_min_y,
                            m_target_max_x, m_target_max_y);
            } else {
                ImGui::TextDisabled(
                    "No colour change; no pixel bounds for this event.");
            }
            fi_timeline_scrubber(this, cap);
        }
        ImGui::Separator();
        if (m_selected_event >= 0 &&
            m_selected_event < (int)cap->events.count) {
            const FIEvent *selected = &cap->events.events[m_selected_event];
            if (selected->kind == FI_EV_BLIT &&
                selected->surface_gen == FI_SURFGEN_INVALID) {
                ImGui::TextColored(
                    ImVec4(1, 0.6f, 0, 1),
                    "Blit destination was not a tracked colour surface; pixel "
                    "history is unavailable.");
            }
            HelpMarker("Pixel history records colour changes only; zeta/depth "
                       "writes are listed as event targets but have no image "
                       "history.");
            /* Anchor the Methods-tab selection to the current event's
             * batch, defaulting to its first ATTRIBUTED record whenever the
             * selection doesn't belong to it (new event/capture, no batch,
             * or first display) -- an in-range selection means the user
             * already picked a row in the Methods tab, so leave it alone. */
            const FIMethodBatch *cur_batch =
                fi_find_method_batch(cap, m_selected_event);
            bool rec_in_range =
                cur_batch && m_selected_rec >= (int)cur_batch->first_rec &&
                m_selected_rec <
                    (int)(cur_batch->first_rec + cur_batch->rec_count);
            if (!rec_in_range) {
                m_selected_rec = -1;
                if (cur_batch) {
                    for (uint32_t i = cur_batch->first_rec;
                         i < cur_batch->first_rec + cur_batch->rec_count;
                         i++) {
                        if (cap->methods.recs[i].confidence ==
                            FI_ORIG_ATTRIBUTED) {
                            m_selected_rec = (int)i;
                            break;
                        }
                    }
                    if (m_selected_rec < 0 && cur_batch->rec_count > 0) {
                        m_selected_rec = (int)cur_batch->first_rec;
                    }
                }
            }

            if (ImGui::BeginTabBar("fi_tabs")) {
                if (ImGui::BeginTabItem("Origin")) {
                    fi_origin_tab(this, cap, cur_batch);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Methods")) {
                    fi_methods_tab(this, cap, cur_batch);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Commands")) {
                    fi_commands_tab(this, cap, cur_batch);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Readable")) {
                    fi_readable_commands_tab(this, cap, cur_batch);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("State")) {
                    fi_state_tab(cap, m_selected_event);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Resources")) {
                    fi_resources_tab(cap, m_selected_event);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Pixels")) {
                    fi_pixels_tab(cap, m_pinned_gen, m_pinned_pixel,
                                  visible_event_id);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        } else {
            ImGui::TextDisabled("Select an event");
        }
        ImGui::EndChild();

        if (ImGui::CollapsingHeader("Address Lookup")) {
            fi_address_lookup_panel(this, cap);
        }
    }
    ImGui::End();

    xemu_frameinspect_capture_release(cap);
}

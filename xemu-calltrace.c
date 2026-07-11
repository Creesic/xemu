/*
 * xemu guest call tracing
 *
 * Copyright (C) 2026 xemu contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "cpu.h"
#include "exec/tb-flush.h"
#include "xemu-xbe.h"
#include "xemu-calltrace.h"
#include "xemu-calltrace-map.h"
#include "xemu-calltrace-events.h"
#include "xemu-calltrace-argsets.h"
#include "xemu-calltrace-kexports.h"
#include <zlib.h>

#define CT_KERNEL_SPACE 0x80000000u

bool xemu_calltrace_armed;
bool xemu_calltrace_capture_args;

static CTMap ct_map;
static bool ct_truncated;

/* Data mode: per-edge arg-set tables (indexed by edge index) and the
 * parallel per-event arg-set index stream. */
static CTEdgeArgs **ct_edge_args;
static uint32_t ct_edge_args_cap;
static GByteArray *ct_arg_index;

static void ct_args_reset(void)
{
    if (ct_edge_args) {
        for (uint32_t i = 0; i < ct_edge_args_cap; i++) {
            g_free(ct_edge_args[i]);
        }
        g_free(ct_edge_args);
        ct_edge_args = NULL;
    }
    ct_edge_args_cap = 0;
    if (ct_arg_index) {
        g_byte_array_free(ct_arg_index, TRUE);
        ct_arg_index = NULL;
    }
}

/* Intern a snapshot for the given edge index; grows the store lazily. */
static uint8_t ct_args_intern_edge(uint32_t edge_index,
                                   const uint32_t args[6])
{
    if (edge_index >= ct_edge_args_cap) {
        uint32_t nc = ct_edge_args_cap ? ct_edge_args_cap : 4096;
        while (edge_index >= nc) {
            nc *= 2;
        }
        ct_edge_args = g_renew(CTEdgeArgs *, ct_edge_args, nc);
        for (uint32_t i = ct_edge_args_cap; i < nc; i++) {
            ct_edge_args[i] = NULL;
        }
        ct_edge_args_cap = nc;
    }
    if (!ct_edge_args[edge_index]) {
        ct_edge_args[edge_index] = g_malloc0(sizeof(CTEdgeArgs));
    }
    return ct_argset_intern(ct_edge_args[edge_index], args);
}

#define CT_THROTTLE_FULL 256   /* log every call up to this many per edge */
#define CT_THROTTLE_EVERY 64   /* then 1-in-this-many afterwards          */

static CalltraceMode ct_mode;
static CTEvents ct_events;
static bool ct_events_trunc;

/* Open-addressing set of callee addresses to ignore (0 = empty slot). */
static uint32_t *ct_ignore;
static uint32_t ct_ignore_cap;
static uint32_t ct_ignore_n;

static bool ct_ignore_has(uint32_t addr)
{
    if (!ct_ignore_cap || addr == 0) {
        return false;
    }
    uint32_t i = (addr * 2654435761u) & (ct_ignore_cap - 1);
    while (ct_ignore[i]) {
        if (ct_ignore[i] == addr) {
            return true;
        }
        i = (i + 1) & (ct_ignore_cap - 1);
    }
    return false;
}

static void ct_ignore_put(uint32_t addr)
{
    if (addr == 0) {
        return;
    }
    /* grow/rehash at 50% load */
    if ((ct_ignore_n + 1) * 2 > ct_ignore_cap) {
        uint32_t newcap = ct_ignore_cap ? ct_ignore_cap * 2 : 1024;
        uint32_t *old = ct_ignore;
        uint32_t oldcap = ct_ignore_cap;
        ct_ignore = g_malloc0(newcap * sizeof(uint32_t));
        ct_ignore_cap = newcap;
        ct_ignore_n = 0;
        for (uint32_t k = 0; k < oldcap; k++) {
            if (old && old[k]) {
                ct_ignore_put(old[k]);
            }
        }
        g_free(old);
    }
    uint32_t i = (addr * 2654435761u) & (ct_ignore_cap - 1);
    while (ct_ignore[i]) {
        if (ct_ignore[i] == addr) {
            return;
        }
        i = (i + 1) & (ct_ignore_cap - 1);
    }
    ct_ignore[i] = addr;
    ct_ignore_n++;
}

void xemu_calltrace_start_mode(CalltraceMode mode)
{
    if (xemu_calltrace_armed || mode == CT_OFF) {
        return;
    }
    if (ct_map.slots) {
        ct_map_free(&ct_map);
    }
    if (!ct_map_init(&ct_map)) {
        return; /* allocation failed; stay disarmed */
    }
    ct_events_free(&ct_events);
    ct_events_init(&ct_events);
    ct_args_reset();
    if (mode == CT_DATA) {
        ct_arg_index = g_byte_array_new();
    }
    xemu_calltrace_capture_args = (mode == CT_DATA);
    ct_truncated = false;
    ct_events_trunc = false;
    ct_mode = mode;
    /* Arm before flushing so retranslated code is instrumented. */
    xemu_calltrace_armed = true;
    queue_tb_flush(qemu_get_cpu(0));
}

void xemu_calltrace_start(void)
{
    xemu_calltrace_start_mode(CT_EDGES);
}

void xemu_calltrace_stop(void)
{
    if (!xemu_calltrace_armed) {
        return;
    }
    xemu_calltrace_armed = false;
    xemu_calltrace_capture_args = false;
    /* Map data intentionally retained until save or next start. */
    queue_tb_flush(qemu_get_cpu(0));
}

CalltraceMode xemu_calltrace_mode(void)
{
    return ct_mode;
}

uint64_t xemu_calltrace_edge_count(void)
{
    return ct_map.slots ? ct_map.num_entries : 0;
}

bool xemu_calltrace_truncated(void)
{
    return ct_truncated;
}

uint64_t xemu_calltrace_event_count(void)
{
    return ct_events.count;
}

bool xemu_calltrace_events_truncated(void)
{
    return ct_events_trunc;
}

uint32_t xemu_calltrace_ignore_count(void)
{
    return ct_ignore_n;
}

void xemu_calltrace_clear_ignore(void)
{
    g_free(ct_ignore);
    ct_ignore = NULL;
    ct_ignore_cap = 0;
    ct_ignore_n = 0;
}

void xemu_calltrace_load_ignore(const char *path, int *added, int *skipped)
{
    int a = 0, s = 0;
    FILE *f = qemu_fopen(path, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' ||
                *p == '\0') {
                continue;
            }
            uint32_t addr = (uint32_t)strtoul(p, NULL, 16);
            if (addr) {
                ct_ignore_put(addr);
                a++;
            } else {
                s++;
            }
        }
        fclose(f);
    }
    if (added) {
        *added = a;
    }
    if (skipped) {
        *skipped = s;
    }
}

void xemu_calltrace_record(uint32_t call_site, uint32_t callee)
{
    /* Stale TB may fire between disarm and flush completion. */
    if (!xemu_calltrace_armed) {
        return;
    }
    /* Skip kernel-internal calls; keep game<->kernel boundary edges. */
    if (call_site >= CT_KERNEL_SPACE && callee >= CT_KERNEL_SPACE) {
        return;
    }
    if (ct_ignore_n && ct_ignore_has(callee)) {
        return;
    }
    CTEdge *e = ct_map_add_indexed(&ct_map, call_site, callee);
    if (!e) {
        if (ct_map.num_entries >= CT_MAP_MAX_ENTRIES) {
            ct_truncated = true;
        }
        return;
    }
    if (ct_mode == CT_TIMED || ct_mode == CT_DATA) {
        if (e->count <= CT_THROTTLE_FULL ||
            (e->count % CT_THROTTLE_EVERY) == 0) {
            if (!ct_events_append(&ct_events, e->index)) {
                ct_events_trunc = true;
            }
        }
    }
}

void xemu_calltrace_record_data(uint32_t call_site, uint32_t callee,
                                const uint32_t args[6])
{
    if (!xemu_calltrace_armed) {
        return;
    }
    if (call_site >= CT_KERNEL_SPACE && callee >= CT_KERNEL_SPACE) {
        return;
    }
    if (ct_ignore_n && ct_ignore_has(callee)) {
        return;
    }
    CTEdge *e = ct_map_add_indexed(&ct_map, call_site, callee);
    if (!e) {
        if (ct_map.num_entries >= CT_MAP_MAX_ENTRIES) {
            ct_truncated = true;
        }
        return;
    }
    if (e->count <= CT_THROTTLE_FULL || (e->count % CT_THROTTLE_EVERY) == 0) {
        /* Append the event and its arg-set index together so the two
         * streams stay exactly parallel. */
        if (ct_events_append(&ct_events, e->index)) {
            uint8_t ai = ct_args_intern_edge(e->index, args);
            g_byte_array_append(ct_arg_index, &ai, 1);
        } else {
            ct_events_trunc = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* .xct writer                                                        */
/* ------------------------------------------------------------------ */

#define XBOX_KERNEL_BASE 0x80010000u
#define XBE_ENTRY_XOR_RETAIL 0xA8FC57ABu
#define XBE_ENTRY_XOR_DEBUG 0x94859D4Bu

/* XBE section header layout (http://www.caustik.com/cxbx/download/xbe.htm) */
#pragma pack(1)
struct xbe_section_header {
    uint32_t flags;
    uint32_t virtual_addr;
    uint32_t virtual_size;
    uint32_t raw_addr;
    uint32_t raw_size;
    uint32_t section_name_addr;
    uint32_t section_name_ref_count;
    uint32_t head_shared_page_ref_count_addr;
    uint32_t tail_shared_page_ref_count_addr;
    uint8_t section_digest[20];
};
#pragma pack()

typedef struct SectionRec {
    uint32_t name_off;
    uint32_t start;
    uint32_t size;
} SectionRec;

typedef struct KImport {
    uint32_t addr;
    uint32_t name_off;
} KImport;

static uint32_t strtab_add(GByteArray *st, const char *s)
{
    if (!s || !*s) {
        return 0;
    }
    uint32_t off = st->len;
    g_byte_array_append(st, (const guint8 *)s, strlen(s) + 1);
    return off;
}

static bool read_guest_u32(uint32_t va, uint32_t *val)
{
    uint32_t v;
    if (xemu_virt_dma_memory_read(va, &v, 4) != 4) {
        return false;
    }
    *val = le32_to_cpu(v);
    return true;
}

static GArray *collect_sections(struct xbe *xbe, GByteArray *strtab)
{
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(SectionRec));
    uint32_t base = ldl_le_p(&xbe->header->m_base);
    uint32_t nsec = MIN(ldl_le_p(&xbe->header->m_sections), 256u);
    uint32_t shdr_addr = ldl_le_p(&xbe->header->m_section_headers_addr);

    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t off =
            shdr_addr + i * sizeof(struct xbe_section_header) - base;
        if (off + sizeof(struct xbe_section_header) > xbe->headers_len) {
            break; /* section headers outside the copied header blob */
        }
        struct xbe_section_header *sh =
            (struct xbe_section_header *)(xbe->headers + off);
        char name[32] = "";
        uint32_t name_off = ldl_le_p(&sh->section_name_addr) - base;
        if (name_off < xbe->headers_len) {
            g_strlcpy(name, (const char *)xbe->headers + name_off,
                      MIN(sizeof(name),
                          (size_t)(xbe->headers_len - name_off)));
        }
        SectionRec s = { strtab_add(strtab, name),
                         ldl_le_p(&sh->virtual_addr),
                         ldl_le_p(&sh->virtual_size) };
        g_array_append_val(arr, s);
    }
    return arr;
}

/*
 * Resolve kernel export addresses by walking the guest xboxkrnl.exe PE
 * export directory (exports are by ordinal; names come from the static
 * table in xemu-calltrace-kexports.h).
 */
static GArray *collect_kernel_exports(GByteArray *strtab)
{
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(KImport));
    uint16_t mz;
    uint32_t e_lfanew, pesig, exp_rva, ord_base, nfuncs, aof_rva;

    if (xemu_virt_dma_memory_read(XBOX_KERNEL_BASE, &mz, 2) != 2 ||
        mz != 0x5A4D) {
        return arr;
    }
    if (!read_guest_u32(XBOX_KERNEL_BASE + 0x3C, &e_lfanew) ||
        e_lfanew > 0x1000) {
        return arr;
    }
    if (!read_guest_u32(XBOX_KERNEL_BASE + e_lfanew, &pesig) ||
        pesig != 0x00004550) {
        return arr;
    }
    /* PE32 optional header: export directory RVA at PE + 0x78 */
    if (!read_guest_u32(XBOX_KERNEL_BASE + e_lfanew + 0x78, &exp_rva) ||
        !exp_rva) {
        return arr;
    }
    uint32_t exp = XBOX_KERNEL_BASE + exp_rva;
    if (!read_guest_u32(exp + 0x10, &ord_base) ||
        !read_guest_u32(exp + 0x14, &nfuncs) ||
        !read_guest_u32(exp + 0x1C, &aof_rva)) {
        return arr;
    }
    nfuncs = MIN(nfuncs, 512u);
    for (uint32_t i = 0; i < nfuncs; i++) {
        uint32_t frva;
        if (!read_guest_u32(XBOX_KERNEL_BASE + aof_rva + 4 * i, &frva) ||
            !frva) {
            continue;
        }
        uint32_t ord = ord_base + i;
        const char *name = NULL;
        char fallback[32];
        if (ord < ARRAY_SIZE(xbox_kernel_export_names)) {
            name = xbox_kernel_export_names[ord];
        }
        if (!name) {
            snprintf(fallback, sizeof(fallback), "kernel_ordinal_%u", ord);
            name = fallback;
        }
        KImport ki = { XBOX_KERNEL_BASE + frva, strtab_add(strtab, name) };
        g_array_append_val(arr, ki);
    }
    return arr;
}

/* Deflate the event log (u32 indices, in order) into a GByteArray. */
static GByteArray *ct_deflate_events(void)
{
    GByteArray *out = g_byte_array_new();
    z_stream zs = { 0 };
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        return out;
    }
    guint8 obuf[65536];
    for (CTEventChunk *c = ct_events.head; c; c = c->next) {
        zs.next_in = (Bytef *)c->data;
        zs.avail_in = c->fill * sizeof(uint32_t);
        while (zs.avail_in) {
            zs.next_out = obuf;
            zs.avail_out = sizeof(obuf);
            deflate(&zs, Z_NO_FLUSH);
            g_byte_array_append(out, obuf, sizeof(obuf) - zs.avail_out);
        }
    }
    int rc;
    do {
        zs.next_out = obuf;
        zs.avail_out = sizeof(obuf);
        rc = deflate(&zs, Z_FINISH);
        g_byte_array_append(out, obuf, sizeof(obuf) - zs.avail_out);
    } while (rc == Z_OK);
    deflateEnd(&zs);
    return out;
}

/* Deflate a raw byte range into a GByteArray. */
static GByteArray *ct_deflate_bytes(const uint8_t *data, size_t len)
{
    GByteArray *out = g_byte_array_new();
    z_stream zs = { 0 };
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        return out;
    }
    guint8 obuf[65536];
    zs.next_in = (Bytef *)data;
    zs.avail_in = len;
    int rc;
    do {
        zs.next_out = obuf;
        zs.avail_out = sizeof(obuf);
        rc = deflate(&zs, Z_FINISH);
        g_byte_array_append(out, obuf, sizeof(obuf) - zs.avail_out);
    } while (rc == Z_OK);
    deflateEnd(&zs);
    return out;
}

/* Build the uncompressed arg-set table blob in edge-index order:
 * per edge: u8 nsets, then nsets * CT_ARGSET_DWORDS * u32. */
static GByteArray *ct_build_argtable(uint32_t nedges)
{
    GByteArray *t = g_byte_array_new();
    for (uint32_t i = 0; i < nedges; i++) {
        CTEdgeArgs *ea = (i < ct_edge_args_cap) ? ct_edge_args[i] : NULL;
        uint8_t nsets = ea ? ea->nsets : 0;
        g_byte_array_append(t, &nsets, 1);
        if (nsets) {
            g_byte_array_append(t, (const guint8 *)ea->sets,
                                nsets * CT_ARGSET_DWORDS * sizeof(uint32_t));
        }
    }
    return t;
}

char *xemu_calltrace_save(const char *dir, char **err_msg)
{
    *err_msg = NULL;

    if (!ct_map.slots || ct_map.num_entries == 0) {
        *err_msg = g_strdup("No call trace data recorded");
        return NULL;
    }
    struct xbe *xbe = xemu_get_xbe_info();
    if (!xbe) {
        *err_msg = g_strdup("No XBE is running");
        return NULL;
    }

    GByteArray *strtab = g_byte_array_new();
    guint8 zero = 0;
    g_byte_array_append(strtab, &zero, 1); /* offset 0 = "" */

    GArray *sections = collect_sections(xbe, strtab);
    GArray *kimports = collect_kernel_exports(strtab);

    /* Title name: UTF-16LE -> UTF-8 (pattern: ui/xui/main-menu.cc:1306) */
    char title8[88] = { 0 };
    char *title_utf8 =
        g_utf16_to_utf8(xbe->cert->m_title_name, 40, NULL, NULL, NULL);
    if (title_utf8) {
        g_strlcpy(title8, title_utf8, sizeof(title8));
    }

    /*
     * The XBE entry point is normally XOR-obfuscated, but xemu's loader
     * de-obfuscates it in guest memory for some titles, so the stored
     * value may already be plaintext. Pick whichever candidate lands
     * inside the image: raw, retail-keyed, or debug-keyed.
     */
    uint32_t base = ldl_le_p(&xbe->header->m_base);
    uint32_t image_size = ldl_le_p(&xbe->header->m_sizeof_image);
    uint32_t raw_entry = ldl_le_p(&xbe->header->m_entry);
    uint32_t entry_candidates[3] = {
        raw_entry,
        raw_entry ^ XBE_ENTRY_XOR_RETAIL,
        raw_entry ^ XBE_ENTRY_XOR_DEBUG,
    };
    uint32_t entry = entry_candidates[1]; /* default: retail-keyed */
    for (int i = 0; i < 3; i++) {
        if (entry_candidates[i] >= base &&
            entry_candidates[i] < base + image_size) {
            entry = entry_candidates[i];
            break;
        }
    }

    /* Output path: <dir>/<SanitizedTitle>-<timestamp>.xct */
    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
    char fname_title[48];
    g_strlcpy(fname_title, title8[0] ? title8 : "recording",
              sizeof(fname_title));
    for (char *p = fname_title; *p; p++) {
        if (!g_ascii_isalnum(*p)) {
            *p = '_';
        }
    }
    char *path = g_strdup_printf("%s/%s-%s.xct", (dir && *dir) ? dir : ".",
                                 fname_title, stamp);

    FILE *f = qemu_fopen(path, "wb");
    bool ok = f != NULL;
    if (ok) {
        /* Host is little-endian on all supported platforms. */
        bool timed = ct_mode == CT_TIMED;
        bool data = ct_mode == CT_DATA;
        bool has_events = timed || data;
        uint32_t hdr[10] = { 0x52544358u,
                             data ? 3u : (timed ? 2u : 1u),
                             ct_truncated ? 1u : 0u,
                             ldl_le_p(&xbe->cert->m_titleid),
                             base,
                             entry,
                             sections->len,
                             kimports->len,
                             ct_map.num_entries,
                             strtab->len };
        ok = fwrite(hdr, sizeof(hdr), 1, f) == 1 &&
             fwrite(title8, sizeof(title8), 1, f) == 1 &&
             fwrite(strtab->data, strtab->len, 1, f) == 1;
        if (ok && sections->len) {
            ok = fwrite(sections->data, sizeof(SectionRec), sections->len,
                        f) == sections->len;
        }
        if (ok && kimports->len) {
            ok = fwrite(kimports->data, sizeof(KImport), kimports->len,
                        f) == kimports->len;
        }
        /* Emit edges in index order so events[k] indexes edges[k]. */
        CTEdge **ordered = g_new(CTEdge *, ct_map.num_entries);
        for (uint32_t i = 0; i < CT_MAP_CAPACITY; i++) {
            if (ct_map.slots[i].key) {
                ordered[ct_map.slots[i].index] = &ct_map.slots[i];
            }
        }
        for (uint32_t i = 0; ok && i < ct_map.num_entries; i++) {
            CTEdge *e = ordered[i];
            uint32_t addrs[2] = { (uint32_t)(e->key >> 32),
                                  (uint32_t)e->key };
            ok = fwrite(addrs, 8, 1, f) == 1 &&
                 fwrite(&e->count, 8, 1, f) == 1;
        }
        g_free(ordered);

        /* v2/v3: trailing compressed event block. */
        if (ok && has_events) {
            GByteArray *blob = ct_deflate_events();
            uint64_t event_count = ct_events.count;
            uint32_t event_flags = ct_events_trunc ? 1u : 0u;
            uint32_t tfull = CT_THROTTLE_FULL, tevery = CT_THROTTLE_EVERY;
            uint64_t raw_bytes = event_count * sizeof(uint32_t);
            uint64_t comp_bytes = blob->len;
            ok = fwrite(&event_count, 8, 1, f) == 1 &&
                 fwrite(&event_flags, 4, 1, f) == 1 &&
                 fwrite(&tfull, 4, 1, f) == 1 &&
                 fwrite(&tevery, 4, 1, f) == 1 &&
                 fwrite(&raw_bytes, 8, 1, f) == 1 &&
                 fwrite(&comp_bytes, 8, 1, f) == 1 &&
                 (comp_bytes == 0 ||
                  fwrite(blob->data, blob->len, 1, f) == 1);
            g_byte_array_free(blob, TRUE);
        }

        /* v3: trailing Data block (arg-set table + per-event index). */
        if (ok && data) {
            GByteArray *table = ct_build_argtable(ct_map.num_entries);
            GByteArray *tcomp = ct_deflate_bytes(table->data, table->len);
            GByteArray *icomp = ct_deflate_bytes(
                ct_arg_index ? ct_arg_index->data : NULL,
                ct_arg_index ? ct_arg_index->len : 0);
            uint32_t argset_dwords = CT_ARGSET_DWORDS;
            uint32_t argset_cap = CT_ARGSET_CAP;
            uint64_t table_raw = table->len;
            uint64_t table_comp = tcomp->len;
            uint64_t index_raw = ct_arg_index ? ct_arg_index->len : 0;
            uint64_t index_comp = icomp->len;
            ok = fwrite(&argset_dwords, 4, 1, f) == 1 &&
                 fwrite(&argset_cap, 4, 1, f) == 1 &&
                 fwrite(&table_raw, 8, 1, f) == 1 &&
                 fwrite(&table_comp, 8, 1, f) == 1 &&
                 fwrite(&index_raw, 8, 1, f) == 1 &&
                 fwrite(&index_comp, 8, 1, f) == 1 &&
                 (table_comp == 0 ||
                  fwrite(tcomp->data, tcomp->len, 1, f) == 1) &&
                 (index_comp == 0 ||
                  fwrite(icomp->data, icomp->len, 1, f) == 1);
            g_byte_array_free(table, TRUE);
            g_byte_array_free(tcomp, TRUE);
            g_byte_array_free(icomp, TRUE);
        }
        fclose(f);
    }

    g_free(title_utf8);
    g_byte_array_free(strtab, TRUE);
    g_array_free(sections, TRUE);
    g_array_free(kimports, TRUE);

    if (!ok) {
        *err_msg = g_strdup_printf("Failed to write %s", path);
        g_free(path);
        return NULL;
    }
    return path;
}

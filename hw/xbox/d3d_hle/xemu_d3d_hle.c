/*
 * Runtime XBE entry interception for the opt-in Xbox D3D8 -> Plume path.
 *
 * This deliberately does not patch guest code. Exact compatibility profiles
 * are checked first; every other loaded XBE is scanned for its linked XDK D3D8
 * implementation. A TCG block-boundary callback then completes the safely
 * bound calls using the title-neutral HLE frontend imported from xrecomp.
 */
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "exec/target_page.h"
#include "exec/tb-flush.h"
#include "hw/core/cpu.h"
#include "system/memory.h"
#include "cpu.h"

#include <Xbe.h>

#include "d3d8_internal.h"
#include "d3d_frontend.h"
#include "d3d_hle_guest.h"
#include "kernel/xbox_memory_layout.h"
#include "hw/xbox/nv2a/debug.h"
#include "ui/xemu-settings.h"
#include "platform/host_time.h"
#include "plume/plume_f2_capture.h"
#include "plume/plume_host.h"
#include "xemu_d3d_hle.h"
#include "xemu_d3d_hle_discovery.h"
#include "xemu_d3d_hle_profile.h"
#include "xemu_d3d_hle_spy.h"

typedef enum XemuD3DHlePendingKind {
    XEMU_D3D_PENDING_NONE,
    XEMU_D3D_PENDING_DEVICE,
    XEMU_D3D_PENDING_BACK_BUFFER,
    XEMU_D3D_PENDING_RESOURCE,
    XEMU_D3D_PENDING_SURFACE_LOCK,
    XEMU_D3D_PENDING_LOCK_3D,
    XEMU_D3D_PENDING_VERTEX_BUFFER,
    XEMU_D3D_PENDING_INDEX_BUFFER,
    XEMU_D3D_PENDING_VERTEX_SHADER,
    XEMU_D3D_PENDING_PIXEL_SHADER,
} XemuD3DHlePendingKind;

typedef struct XemuD3DHlePending {
    XemuD3DHlePendingKind kind;
    uint32_t return_pc;
    uint32_t args[XEMU_D3D_HLE_MAX_ABI_ARGS];
    uint32_t device_parameters_va;
    uint32_t entry_eax;
    uint32_t output_handle;
    uint32_t vertex_shader_declaration[NV2A_VS_MAX_DECLARATION_DWORDS];
    uint32_t vertex_shader_program[1u + NV2A_VS_MAX_INSTRUCTIONS * 4u];
    uint32_t pixel_shader_definition[60];
    uint32_t vertex_shader_program_dwords;
    bool output_handle_valid;
    bool vertex_shader_declaration_present;
    bool vertex_shader_snapshot_valid;
    bool pixel_shader_definition_valid;
} XemuD3DHlePending;

typedef struct XemuD3DHleDeferred {
    XemuD3DHlePending pending;
    uint32_t guest_result;
} XemuD3DHleDeferred;

typedef struct XemuD3DHleCoverage {
    uint32_t image_base;
    uint32_t image_size;
    uint8_t *page_bits;
    size_t page_count;
    uint32_t d3d_needed;
    uint32_t d3d_covered;
    bool has_d3d_section;
    bool ready;
    uint32_t reported_covered;
    uint32_t coverage_epoch;
} XemuD3DHleCoverage;

enum { XEMU_D3D_HLE_TRACE_RING_SIZE = 256 };

typedef struct XemuD3DHleTraceEntry {
    uint64_t hook;
    uint32_t pc;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t esi;
    uint32_t edi;
    uint32_t esp;
    uint32_t return_pc;
    uint32_t stack1;
    uint32_t resource_common;
    uint32_t resource_data;
    uint32_t resource_lock;
    uint32_t resource_format;
    uint32_t resource_size;
    bool resource_snapshot_valid;
    const char *name;
    bool hle;
} XemuD3DHleTraceEntry;

uint32_t g_eax, g_ebx, g_ecx, g_edx, g_ebp, g_esi, g_edi, g_esp;
ptrdiff_t g_xbox_mem_offset;
uint32_t xrecomp_d3d_hle_dirty_flags_va;
uint32_t xrecomp_d3d_hle_deferred_texture_state_va;
uint32_t xrecomp_d3d_hle_fog_state_va;

static CPUState *s_cpu;
static uint8_t *s_ram;
static uint64_t s_ram_size;
static bool s_requested;
static bool s_environment_override;
static XemuD3DHleStatus s_status = XEMU_D3D_HLE_STATUS_DISABLED;
static bool s_profile_checked;
static bool s_profile_valid;
static const XemuD3DHleProfile *s_profile;
static bool s_identity_valid;
static uint32_t s_identity_title_id;
static uint32_t s_identity_timedate;
static uint32_t s_identity_image_size;
static uint32_t s_generation;
static XemuD3DHleCoverage s_coverage;
static char s_status_detail[256];
static bool s_discovery_job_queued;
static uint32_t s_discovery_job_generation;
static uint32_t s_discovery_job_pc;
static uint32_t s_discovery_scanned_epoch;
static bool s_header_valid;
static uint64_t s_header_valid_ms;
static bool s_loader_resolved;
static bool s_loader_call_active;
static bool loader_entry_span_mapped;
static uint32_t s_loader_section_va;
static uint32_t s_loader_section_size;
static bool s_host_ready;
static bool s_trace_entries;
static bool s_trace_dumped;
static bool s_diagnostics;
static bool s_overlay_seen;
static bool s_post_fmv;
static uint64_t s_hook_entry_count;
static XemuD3DHleTraceEntry
    s_trace_ring[XEMU_D3D_HLE_TRACE_RING_SIZE];
static uint32_t s_last_hook_pc;
static const char *s_last_hook_name;
static XemuD3DHlePending s_pending;
static XemuD3DHlePending s_device_pending;
static bool s_device_pending_active;
static XemuD3DHleDeferred *s_bootstrap_deferred;
static size_t s_bootstrap_deferred_count;
static size_t s_bootstrap_deferred_capacity;
static const char *s_active_hook_name;
static bool s_vblank_queued;
static bool s_session_reset_queued;
static uint32_t s_vblank_pcrtc_start;

static QemuMutex s_overlay_mutex;
static bool s_overlay_initialized;
static bool s_overlay_visible;
static uint64_t s_overlay_version;
static uint64_t s_overlay_provider_version;
static uint8_t *s_overlay_published;
static uint8_t *s_overlay_provider;
static size_t s_overlay_size;
static size_t s_overlay_provider_capacity;
static uint32_t s_overlay_x;
static uint32_t s_overlay_y;
static uint32_t s_overlay_width;
static uint32_t s_overlay_height;
static uint32_t s_overlay_pitch;

extern uintptr_t xemu_get_native_window_handle(void);

static void xemu_d3d_hle_dump_trace_ring(void);
static HRESULT xemu_d3d_hle_activate_host_device(uint32_t parameters_va);
static bool xemu_d3d_hle_read_identity(
    uint32_t *title_id, uint32_t *timedate, uint32_t *image_size);
static bool xemu_d3d_hle_update_coverage(void);
static void xemu_d3d_hle_reset_coverage(void);
static bool xemu_d3d_hle_try_resolve_kernel_loader(void);
static void xemu_d3d_hle_queue_discovery(uint32_t pc);
static void xemu_d3d_hle_queue_session_reset(void);

static int xemu_d3d_hle_overlay_provider(
    XgpuPlumeDebugOverlayFrame *frame)
{
    bool visible;

    if (!frame || !s_overlay_initialized || !s_host_ready)
        return 0;
    qemu_mutex_lock(&s_overlay_mutex);
    visible = s_overlay_visible;
    if (visible && s_overlay_provider_version != s_overlay_version) {
        if (s_overlay_provider_capacity < s_overlay_size) {
            s_overlay_provider = g_realloc(s_overlay_provider,
                                           s_overlay_size);
            s_overlay_provider_capacity = s_overlay_size;
        }
        memcpy(s_overlay_provider, s_overlay_published, s_overlay_size);
        s_overlay_provider_version = s_overlay_version;
    }
    if (visible) {
        frame->pixels = s_overlay_provider;
        frame->x = s_overlay_x;
        frame->y = s_overlay_y;
        frame->width = s_overlay_width;
        frame->height = s_overlay_height;
        frame->pitch = s_overlay_pitch;
        frame->version = s_overlay_provider_version;
        frame->space = XGPU_PLUME_DEBUG_OVERLAY_SPACE_HOST;
    }
    qemu_mutex_unlock(&s_overlay_mutex);
    return visible ? 1 : 0;
}

static bool xemu_d3d_hle_translate(uint32_t va, hwaddr *physical)
{
    MemTxAttrs attrs;
    hwaddr page;

    if (!s_cpu || !physical)
        return false;
    page = cpu_get_phys_page_attrs_debug(
        s_cpu, (vaddr)va & TARGET_PAGE_MASK, &attrs);
    if (page == (hwaddr)-1)
        return false;
    page += va & ~TARGET_PAGE_MASK;
    if (page >= s_ram_size)
        return false;
    *physical = page;
    return true;
}

uint8_t *xbox_guest_ptr(uint32_t va)
{
    hwaddr physical;
    if (!xemu_d3d_hle_translate(va, &physical)) {
        fprintf(stderr, "[D3D-HLE] unmapped guest pointer %08X\n", va);
        abort();
    }
    return s_ram + physical;
}

static bool xemu_d3d_hle_read(uint32_t va, void *output, size_t size)
{
    uint8_t *out = output;
    while (size) {
        hwaddr physical;
        size_t chunk;
        if (!xemu_d3d_hle_translate(va, &physical))
            return false;
        chunk = MIN(size, TARGET_PAGE_SIZE - (physical & ~TARGET_PAGE_MASK));
        if (physical + chunk > s_ram_size)
            return false;
        memcpy(out, s_ram + physical, chunk);
        out += chunk;
        va += (uint32_t)chunk;
        size -= chunk;
    }
    return true;
}

static int xemu_d3d_hle_read_range(
    uint32_t va, void *output, size_t size)
{
    return xemu_d3d_hle_read(va, output, size) ? 1 : 0;
}

static bool xemu_d3d_hle_read_u32(uint32_t va, uint32_t *value)
{
    return xemu_d3d_hle_read(va, value, sizeof(*value));
}

static uint16_t xemu_d3d_hle_pe_u16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t xemu_d3d_hle_pe_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static bool xemu_d3d_hle_try_resolve_kernel_loader(void)
{
    enum { KERNEL_BASE = 0x80010000u };
    uint8_t dos[64] = { 0 };
    uint8_t nt[24] = { 0 };
    uint8_t optional[512] = { 0 };
    uint8_t exports[40] = { 0 };
    uint32_t pe_offset;
    uint32_t export_rva;
    uint32_t export_size;
    uint32_t ordinal_base;
    uint32_t function_count;
    uint32_t name_count;
    uint32_t functions_rva;
    uint32_t names_rva;
    uint32_t ordinals_rva;
    uint32_t load_rva = 0;
    uint32_t unload_rva = 0;
    uint16_t optional_size;
    uint16_t optional_magic;
    uint32_t directory_offset;
    uint32_t i;

    if (s_loader_resolved)
        return true;
    if (!s_cpu || !xemu_d3d_hle_read(KERNEL_BASE, dos, sizeof(dos)) ||
        dos[0] != 'M' || dos[1] != 'Z')
        return false;
    pe_offset = xemu_d3d_hle_pe_u32(dos + 0x3Cu);
    if (pe_offset > 0x00100000u ||
        !xemu_d3d_hle_read(KERNEL_BASE + pe_offset,
                           nt, sizeof(nt)) ||
        memcmp(nt, "PE\0\0", 4) != 0)
        return false;
    optional_size = xemu_d3d_hle_pe_u16(nt + 4u + 16u);
    if (!optional_size || optional_size > sizeof(optional) ||
        !xemu_d3d_hle_read(KERNEL_BASE + pe_offset + sizeof(nt),
                           optional, optional_size))
        return false;
    optional_magic = xemu_d3d_hle_pe_u16(optional);
    directory_offset = optional_magic == 0x10Bu ? 96u :
                       optional_magic == 0x20Bu ? 112u : UINT32_MAX;
    if (directory_offset == UINT32_MAX ||
        directory_offset + 8u > optional_size)
        return false;
    export_rva = xemu_d3d_hle_pe_u32(optional + directory_offset);
    export_size = xemu_d3d_hle_pe_u32(optional + directory_offset + 4u);
    if (!export_rva || export_size < sizeof(exports) ||
        export_rva > UINT32_MAX - KERNEL_BASE ||
        !xemu_d3d_hle_read(KERNEL_BASE + export_rva,
                           exports, sizeof(exports)))
        return false;

    ordinal_base = xemu_d3d_hle_pe_u32(exports + 16u);
    function_count = xemu_d3d_hle_pe_u32(exports + 20u);
    name_count = xemu_d3d_hle_pe_u32(exports + 24u);
    functions_rva = xemu_d3d_hle_pe_u32(exports + 28u);
    names_rva = xemu_d3d_hle_pe_u32(exports + 32u);
    ordinals_rva = xemu_d3d_hle_pe_u32(exports + 36u);
    if (!function_count || function_count > 65536u ||
        name_count > function_count ||
        functions_rva > UINT32_MAX - KERNEL_BASE ||
        names_rva > UINT32_MAX - KERNEL_BASE ||
        ordinals_rva > UINT32_MAX - KERNEL_BASE)
        return false;

    for (i = 0; i < name_count; ++i) {
        uint8_t name_rva_bytes[4];
        uint8_t ordinal_bytes[2];
        uint8_t function_bytes[4];
        char name[32] = { 0 };
        uint32_t name_rva;
        uint16_t ordinal;
        uint32_t function_rva;

        if (!xemu_d3d_hle_read(
                KERNEL_BASE + names_rva + i * 4u,
                name_rva_bytes, sizeof(name_rva_bytes)) ||
            !xemu_d3d_hle_read(
                KERNEL_BASE + ordinals_rva + i * 2u,
                ordinal_bytes, sizeof(ordinal_bytes)))
            return false;
        name_rva = xemu_d3d_hle_pe_u32(name_rva_bytes);
        ordinal = xemu_d3d_hle_pe_u16(ordinal_bytes);
        if (ordinal >= function_count ||
            name_rva > UINT32_MAX - KERNEL_BASE ||
            !xemu_d3d_hle_read(KERNEL_BASE + name_rva,
                               name, sizeof(name) - 1u))
            continue;
        if (strcmp(name, "XeLoadSection") != 0 &&
            strcmp(name, "XeUnloadSection") != 0)
            continue;
        if (!xemu_d3d_hle_read(
                KERNEL_BASE + functions_rva + ordinal * 4u,
                function_bytes, sizeof(function_bytes)))
            return false;
        function_rva = xemu_d3d_hle_pe_u32(function_bytes);
        if (strcmp(name, "XeLoadSection") == 0)
            load_rva = function_rva;
        else
            unload_rva = function_rva;
    }

    if (!load_rva && ordinal_base <= 0x0147u &&
        0x0147u - ordinal_base < function_count) {
        uint8_t function_bytes[4];
        if (!xemu_d3d_hle_read(
                KERNEL_BASE + functions_rva +
                    (0x0147u - ordinal_base) * 4u,
                function_bytes, sizeof(function_bytes)))
            return false;
        load_rva = xemu_d3d_hle_pe_u32(function_bytes);
    }
    if (!unload_rva && ordinal_base <= 0x0148u &&
        0x0148u - ordinal_base < function_count) {
        uint8_t function_bytes[4];
        if (!xemu_d3d_hle_read(
                KERNEL_BASE + functions_rva +
                    (0x0148u - ordinal_base) * 4u,
                function_bytes, sizeof(function_bytes)))
            return false;
        unload_rva = xemu_d3d_hle_pe_u32(function_bytes);
    }
    if (!load_rva || !unload_rva ||
        load_rva > UINT32_MAX - KERNEL_BASE ||
        unload_rva > UINT32_MAX - KERNEL_BASE)
        return false;

    s_cpu->exec_loader_pc[0] = KERNEL_BASE + load_rva;
    s_cpu->exec_loader_pc[1] = KERNEL_BASE + unload_rva;
    s_cpu->exec_loader_return_pc = 0;
    queue_tb_flush(s_cpu);
    s_loader_resolved = true;
    fprintf(stderr,
            "[D3D-HLE] kernel loader base=%08X XeLoadSection=%08X "
            "XeUnloadSection=%08X\n",
            KERNEL_BASE, (uint32_t)s_cpu->exec_loader_pc[0],
            (uint32_t)s_cpu->exec_loader_pc[1]);
    return true;
}

static void xemu_d3d_hle_reset_coverage(void)
{
    g_free(s_coverage.page_bits);
    memset(&s_coverage, 0, sizeof(s_coverage));
}

static bool xemu_d3d_hle_section_name_is(
    const char name[16], const char *expected)
{
    return g_ascii_strcasecmp(name, expected) == 0 ||
           (expected[0] == 'D' && name[0] == '.' &&
            g_ascii_strncasecmp(name + 1, expected, 15) == 0);
}

static uint32_t xemu_d3d_hle_covered_span(
    uint32_t address, uint32_t size)
{
    uint32_t covered = 0;

    while (size) {
        uint32_t page = address & TARGET_PAGE_MASK;
        uint32_t page_offset = address & ~TARGET_PAGE_MASK;
        uint32_t chunk = MIN(size, TARGET_PAGE_SIZE - page_offset);
        hwaddr physical;

        if (xemu_d3d_hle_translate(page, &physical)) {
            uint64_t page_index =
                ((uint64_t)page - s_coverage.image_base) /
                TARGET_PAGE_SIZE;
            if (page >= s_coverage.image_base &&
                page_index < s_coverage.page_count)
                s_coverage.page_bits[page_index] = 1;
            covered += chunk;
        }
        address += chunk;
        size -= chunk;
    }
    return covered;
}

static bool xemu_d3d_hle_span_fully_mapped(
    uint32_t address, uint32_t size)
{
    while (size) {
        uint32_t page = address & TARGET_PAGE_MASK;
        uint32_t page_offset = address & ~TARGET_PAGE_MASK;
        uint32_t chunk = MIN(size, TARGET_PAGE_SIZE - page_offset);
        hwaddr physical;

        if (!xemu_d3d_hle_translate(page, &physical))
            return false;
        address += chunk;
        size -= chunk;
    }
    return true;
}

static bool xemu_d3d_hle_update_coverage(void)
{
    xbe_header header;
    xbe_section_header *sections = NULL;
    size_t section_bytes;
    uint32_t image_end;
    uint32_t d3d_needed = 0;
    uint32_t d3d_covered = 0;
    uint32_t fallback_needed = 0;
    uint32_t fallback_covered = 0;
    bool has_d3d = false;
    bool has_text = false;
    bool have_section_names = true;
    size_t i;

    if (!xemu_d3d_hle_read(0x00010000u, &header, sizeof(header)) ||
        header.dwMagic != 0x48454258u ||
        header.dwBaseAddr != 0x00010000u ||
        !header.dwSizeofImage ||
        header.dwSizeofImage > 0x08000000u ||
        header.dwSizeofHeaders < sizeof(header) ||
        header.dwSections == 0 || header.dwSections > 256u ||
        header.pSectionHeadersAddr < header.dwBaseAddr ||
        header.pSectionHeadersAddr - header.dwBaseAddr >
            header.dwSizeofHeaders)
        return false;

    section_bytes = (size_t)header.dwSections * sizeof(xbe_section_header);
    if (section_bytes > header.dwSizeofHeaders ||
        header.pSectionHeadersAddr - header.dwBaseAddr >
            header.dwSizeofHeaders - section_bytes)
        return false;
    image_end = header.dwBaseAddr + header.dwSizeofImage;

    if (s_coverage.image_base != header.dwBaseAddr ||
        s_coverage.image_size != header.dwSizeofImage ||
        !s_coverage.page_bits) {
        xemu_d3d_hle_reset_coverage();
        s_coverage.image_base = header.dwBaseAddr;
        s_coverage.image_size = header.dwSizeofImage;
        s_coverage.page_count =
            ((uint64_t)header.dwSizeofImage + TARGET_PAGE_SIZE - 1u) /
            TARGET_PAGE_SIZE;
        s_coverage.page_bits = g_new0(uint8_t, s_coverage.page_count);
        if (!s_coverage.page_bits)
            return false;
    }
    memset(s_coverage.page_bits, 0, s_coverage.page_count);

    sections = g_new0(xbe_section_header, header.dwSections);
    if (!sections || !xemu_d3d_hle_read(
            header.pSectionHeadersAddr, sections, section_bytes)) {
        g_free(sections);
        return false;
    }

    for (i = 0; i < header.dwSections; ++i) {
        char name[16] = { 0 };
        const xbe_section_header *section = &sections[i];

        if (!section->dwSizeofRaw ||
            section->dwVirtualAddr < header.dwBaseAddr ||
            section->dwVirtualAddr >= image_end ||
            section->dwSizeofRaw > image_end - section->dwVirtualAddr)
            continue;
        if (!section->SectionNameAddr ||
            !xemu_d3d_hle_read(section->SectionNameAddr,
                               name, sizeof(name) - 1u)) {
            have_section_names = false;
            continue;
        }
        has_d3d = has_d3d ||
            xemu_d3d_hle_section_name_is(name, "D3D");
        has_text = has_text ||
            xemu_d3d_hle_section_name_is(name, ".text");
    }

    for (i = 0; i < header.dwSections; ++i) {
        char name[16] = { 0 };
        const xbe_section_header *section = &sections[i];
        bool is_d3d;
        bool is_text;
        bool fallback_exec;
        bool required;

        if (!section->dwSizeofRaw ||
            section->dwVirtualAddr < header.dwBaseAddr ||
            section->dwVirtualAddr >= image_end ||
            section->dwSizeofRaw > image_end - section->dwVirtualAddr ||
            !section->SectionNameAddr ||
            !xemu_d3d_hle_read(section->SectionNameAddr,
                               name, sizeof(name) - 1u))
            continue;
        is_d3d = xemu_d3d_hle_section_name_is(name, "D3D");
        is_text = xemu_d3d_hle_section_name_is(name, ".text");
        fallback_exec = !has_d3d && !has_text &&
            (section->dwFlags_value & XBE_SECTION_HEADER_FLAGS_EXECUTABLE);
        required = has_d3d ? is_d3d : (has_text ? is_text : fallback_exec);
        if (!required)
            continue;
        if (has_d3d) {
            d3d_needed += section->dwSizeofRaw;
            d3d_covered += xemu_d3d_hle_covered_span(
                section->dwVirtualAddr, section->dwSizeofRaw);
        } else {
            fallback_needed += section->dwSizeofRaw;
            fallback_covered += xemu_d3d_hle_covered_span(
                section->dwVirtualAddr, section->dwSizeofRaw);
        }
    }
    g_free(sections);

    if (!have_section_names)
        return false;
    {
        uint32_t needed = has_d3d ? d3d_needed : fallback_needed;
        uint32_t covered = has_d3d ? d3d_covered : fallback_covered;
        bool changed = s_coverage.has_d3d_section != has_d3d ||
                       s_coverage.d3d_needed != needed ||
                       s_coverage.d3d_covered != covered;

        s_coverage.has_d3d_section = has_d3d;
        s_coverage.d3d_needed = needed;
        s_coverage.d3d_covered = covered;
        if (changed)
            ++s_coverage.coverage_epoch;
    }
    s_coverage.ready = s_coverage.d3d_needed != 0 &&
                       s_coverage.d3d_covered >= s_coverage.d3d_needed;
    if (s_coverage.d3d_covered != s_coverage.reported_covered) {
        s_coverage.reported_covered = s_coverage.d3d_covered;
        g_snprintf(s_status_detail, sizeof(s_status_detail),
                   "scanning mapped D3D pages (covered=%u/%u)",
                   s_coverage.d3d_covered, s_coverage.d3d_needed);
        fprintf(stderr, "[D3D-HLE] %s\n", s_status_detail);
    }
    return s_coverage.ready;
}

static bool xemu_d3d_hle_sha1(uint32_t va, uint32_t size,
                              const char *expected)
{
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA1);
    uint8_t buffer[4096];
    const char *actual;
    uint32_t done = 0;
    bool match = false;

    if (!checksum)
        return false;
    while (done < size) {
        uint32_t chunk = MIN((uint32_t)sizeof(buffer), size - done);
        if (!xemu_d3d_hle_read(va + done, buffer, chunk))
            goto out;
        g_checksum_update(checksum, buffer, chunk);
        done += chunk;
    }
    actual = g_checksum_get_string(checksum);
    match = actual && g_ascii_strcasecmp(actual, expected) == 0;
out:
    g_checksum_free(checksum);
    return match;
}

static bool xemu_d3d_hle_validate_profile(
    const XemuD3DHleProfile *profile)
{
    uint32_t magic, base, headers, image, timedate, cert, title_id, sections;

    if (!xemu_d3d_hle_read_u32(0x00010000u, &magic) ||
        !xemu_d3d_hle_read_u32(0x00010104u, &base) ||
        !xemu_d3d_hle_read_u32(0x00010108u, &headers) ||
        !xemu_d3d_hle_read_u32(0x0001010Cu, &image) ||
        !xemu_d3d_hle_read_u32(0x00010114u, &timedate) ||
        !xemu_d3d_hle_read_u32(0x00010118u, &cert) ||
        !xemu_d3d_hle_read_u32(0x0001011Cu, &sections) ||
        !xemu_d3d_hle_read_u32(cert + 8u, &title_id))
        return false;
    if (!profile || magic != 0x48454258u || base != profile->xbe_base ||
        headers != profile->xbe_headers_size ||
        image != profile->xbe_image_size ||
        timedate != profile->xbe_timestamp ||
        sections != profile->xbe_section_count ||
        title_id != profile->xbe_title_id)
        return false;
    return xemu_d3d_hle_sha1(
        profile->d3d_section_va, profile->d3d_section_size,
        profile->d3d_section_sha1);
}

static const XemuD3DHleProfile *xemu_d3d_hle_select_profile(void)
{
    const XemuD3DHleProfile *const *profiles;
    char error[256];
    size_t count;
    size_t i;

    profiles = xemu_d3d_hle_profiles(&count);
    for (i = 0; i < count; ++i) {
        if (!xemu_d3d_hle_profile_validate(
                profiles[i], error, sizeof(error))) {
            fprintf(stderr,
                    "[D3D-HLE] exact-profile invalid: %s; "
                    "skipping that override\n",
                    error);
            continue;
        }
        if (profiles[i]->bootstrap == XEMU_D3D_HLE_BOOTSTRAP_DIRECT &&
            !d3d_hle_guest_synthetic_allocator_available()) {
            fprintf(stderr,
                    "[D3D-HLE] exact-profile invalid: %s requires "
                    "synthetic guest allocation; skipping that override\n",
                    profiles[i]->name);
            continue;
        }
        if (xemu_d3d_hle_validate_profile(profiles[i]))
            return profiles[i];
    }
    return NULL;
}

static bool xemu_d3d_hle_read_identity(
    uint32_t *title_id, uint32_t *timedate, uint32_t *image_size)
{
    uint32_t magic;
    uint32_t base;
    uint32_t headers;
    uint32_t image;
    uint32_t timestamp;
    uint32_t certificate;
    uint32_t title;

    if (!title_id || !timedate || !image_size ||
        !xemu_d3d_hle_read_u32(0x00010000u, &magic) ||
        !xemu_d3d_hle_read_u32(0x00010104u, &base) ||
        !xemu_d3d_hle_read_u32(0x00010108u, &headers) ||
        !xemu_d3d_hle_read_u32(0x0001010Cu, &image) ||
        !xemu_d3d_hle_read_u32(0x00010114u, &timestamp) ||
        !xemu_d3d_hle_read_u32(0x00010118u, &certificate) ||
        !xemu_d3d_hle_read_u32(certificate + 8u, &title) ||
        magic != 0x48454258u || base != 0x00010000u ||
        headers < 0x178u || headers > image ||
        image > 0x08000000u || image > UINT32_MAX - base)
        return false;
    *title_id = title;
    *timedate = timestamp;
    *image_size = image;
    return true;
}

void xemu_d3d_hle_session_reset(const char *why)
{
    fprintf(stderr, "[D3D-HLE] session reset begin: %s\n",
            why && why[0] ? why : "unspecified");
    s_host_ready = false;
    qatomic_set(&s_session_reset_queued, false);
    qemu_mutex_lock(&s_overlay_mutex);
    s_overlay_visible = false;
    ++s_overlay_version;
    qemu_mutex_unlock(&s_overlay_mutex);
    if (s_requested)
        d3d_hle_guest_reset_session();
    fprintf(stderr, "[D3D-HLE] session reset teardown complete\n");
    g_free(s_bootstrap_deferred);
    s_bootstrap_deferred = NULL;
    s_bootstrap_deferred_count = 0;
    s_bootstrap_deferred_capacity = 0;
    memset(&s_pending, 0, sizeof(s_pending));
    memset(&s_device_pending, 0, sizeof(s_device_pending));
    s_device_pending_active = false;
    if (xemu_d3d_hle_spy_enabled())
        xemu_d3d_hle_spy_dump("reset");
    xemu_d3d_hle_spy_reset();
    s_profile = NULL;
    s_profile_checked = false;
    s_profile_valid = false;
    s_discovery_job_queued = false;
    s_discovery_job_generation = 0;
    s_discovery_job_pc = 0;
    s_discovery_scanned_epoch = 0;
    s_header_valid = false;
    s_header_valid_ms = 0;
    s_loader_call_active = false;
    loader_entry_span_mapped = false;
    s_loader_section_va = 0;
    s_loader_section_size = 0;
    if (s_cpu)
        s_cpu->exec_loader_return_pc = 0;
    s_identity_valid = false;
    s_identity_title_id = 0;
    s_identity_timedate = 0;
    s_identity_image_size = 0;
    xemu_d3d_hle_reset_coverage();
    xrecomp_d3d_hle_dirty_flags_va = 0;
    xrecomp_d3d_hle_deferred_texture_state_va = 0;
    xrecomp_d3d_hle_fog_state_va = 0;
    s_vblank_queued = false;
    s_vblank_pcrtc_start = 0;
    s_trace_dumped = false;
    s_hook_entry_count = 0;
    s_last_hook_pc = 0;
    s_last_hook_name = NULL;
    s_active_hook_name = NULL;
    memset(s_trace_ring, 0, sizeof(s_trace_ring));
    s_overlay_seen = false;
    s_post_fmv = false;
    if (s_cpu)
        s_cpu->exec_entry_return_pc = 0;
    ++s_generation;
    g_snprintf(s_status_detail, sizeof(s_status_detail),
               "session reset: %s", why && why[0] ? why : "unspecified");
    qatomic_set(&s_status, s_requested
        ? XEMU_D3D_HLE_STATUS_ARMED
        : XEMU_D3D_HLE_STATUS_DISABLED);
    fprintf(stderr,
            "[D3D-HLE] session reset: gen=%u (%s)\n",
            s_generation, why && why[0] ? why : "unspecified");
}

static void xemu_d3d_hle_load_registers(CPUX86State *env)
{
    g_eax = (uint32_t)env->regs[R_EAX];
    g_ebx = (uint32_t)env->regs[R_EBX];
    g_ecx = (uint32_t)env->regs[R_ECX];
    g_edx = (uint32_t)env->regs[R_EDX];
    g_ebp = (uint32_t)env->regs[R_EBP];
    g_esi = (uint32_t)env->regs[R_ESI];
    g_edi = (uint32_t)env->regs[R_EDI];
    g_esp = (uint32_t)env->regs[R_ESP];
}

static bool xemu_d3d_hle_pc_is_in_loaded_xbe(uint32_t pc)
{
    uint32_t magic, base, headers, image;

    if (!xemu_d3d_hle_read_u32(0x00010000u, &magic) ||
        !xemu_d3d_hle_read_u32(0x00010104u, &base) ||
        !xemu_d3d_hle_read_u32(0x00010108u, &headers) ||
        !xemu_d3d_hle_read_u32(0x0001010Cu, &image) ||
        magic != 0x48454258u || base != 0x00010000u ||
        headers < 0x178u || headers > image || image > 0x08000000u ||
        image > UINT32_MAX - base) {
        return false;
    }

    /* Seeing a valid header only proves that the kernel loader has started.
     * Wait until TCG is translating code from the image itself: preload
     * section mappings are established by then.  Scanning at header arrival
     * races the loader on large XBEs such as Forza Motorsport. */
    return pc >= base + headers && pc < base + image;
}

static bool xemu_d3d_hle_resolve_loaded_xbe(uint32_t pc)
{
    char error[256];
    uint32_t magic;
    bool retryable = false;

    if (s_profile_checked)
        return s_profile_valid;
    if (!xemu_d3d_hle_pc_is_in_loaded_xbe(pc))
        return false;
    if (!xemu_d3d_hle_read_u32(0x00010000u, &magic) ||
        magic != 0x48454258u)
        return false;

    s_profile_checked = true;
    /* Preserve the two extensively runtime-validated contracts.  Every
     * other XBE falls through to title-neutral signature and ABI discovery. */
    s_profile = xemu_d3d_hle_spy_enabled()
        ? NULL
        : xemu_d3d_hle_select_profile();
    if (!s_profile) {
        s_profile = xemu_d3d_hle_discover(
            xemu_d3d_hle_read, &retryable, error, sizeof(error));
        if (!s_profile) {
            if (retryable) {
                s_profile_checked = false;
                g_snprintf(s_status_detail, sizeof(s_status_detail),
                           "%s; waiting for another mapped section",
                           error);
                qatomic_set(&s_status, XEMU_D3D_HLE_STATUS_ARMED);
                fprintf(stderr,
                        "[D3D-HLE] loaded XBE is not scan-ready: %s; "
                        "waiting for coverage growth\n",
                        error);
                return false;
            }
            g_strlcpy(s_status_detail, error, sizeof(s_status_detail));
            qatomic_set(&s_status, XEMU_D3D_HLE_STATUS_PROFILE_REJECTED);
            fprintf(stderr,
                    "[D3D-HLE] automatic XDK D3D8 discovery rejected "
                    "the loaded XBE: %s\n",
                    error);
            return false;
        }
    }

    if (!xemu_d3d_hle_spy_enabled() &&
        (s_profile->discovery_mutating_uncovered_count ||
         s_profile->discovery_ambiguous_count ||
         s_profile->discovery_uncovered_abi_count)) {
        g_snprintf(s_status_detail, sizeof(s_status_detail),
                   "automatic D3D discovery left %u mutating functions "
                   "and %u ABI holes uncovered",
                   s_profile->discovery_mutating_uncovered_count,
                   s_profile->discovery_uncovered_abi_count);
        qatomic_set(&s_status, XEMU_D3D_HLE_STATUS_PROFILE_REJECTED);
        fprintf(stderr,
                "[D3D-HLE] automatic profile refused: mutating=%u "
                "uncovered-abi=%u total-uncovered=%u; leaving title on NV2A\n",
                s_profile->discovery_mutating_uncovered_count,
                s_profile->discovery_uncovered_abi_count,
                s_profile->discovery_unsupported_count);
        s_profile = NULL;
        s_profile_valid = false;
        return false;
    }

    s_status_detail[0] = '\0';
    s_profile_valid = true;
    xrecomp_d3d_hle_dirty_flags_va = s_profile->dirty_flags_va;
    xrecomp_d3d_hle_deferred_texture_state_va =
        s_profile->deferred_texture_state_va;
    xrecomp_d3d_hle_fog_state_va = s_profile->fog_state_va;
    fprintf(stderr,
            "[D3D-HLE] selected %s with %zu D3D entry hooks\n",
            s_profile->name, s_profile->hook_count);
    if (xemu_d3d_hle_spy_enabled()) {
        xemu_d3d_hle_spy_bind(s_profile);
        g_snprintf(s_status_detail, sizeof(s_status_detail),
                   "D3D8 spy on NV2A: %u symbols, %u called holes",
                   xemu_d3d_hle_spy_symbol_count(),
                   xemu_d3d_hle_spy_called_holes());
        queue_tb_flush(s_cpu);
        qatomic_set(&s_status, XEMU_D3D_HLE_STATUS_PROFILE_VERIFIED);
        return true;
    }
    qatomic_set(&s_status, XEMU_D3D_HLE_STATUS_PROFILE_VERIFIED);
    return true;
}

static const XemuD3DHleHook *xemu_d3d_hle_find_any_hook(uint32_t pc)
{
    return s_profile
        ? xemu_d3d_hle_profile_find_hook(s_profile, pc)
        : NULL;
}

static uint32_t xemu_d3d_hle_stack(CPUX86State *env, unsigned index)
{
    uint32_t value = 0;
    (void)xemu_d3d_hle_read_u32(
        (uint32_t)env->regs[R_ESP] + index * 4u, &value);
    return value;
}

static uint32_t xemu_d3d_hle_public_argument(
    CPUX86State *env, const XemuD3DHleHook *hook, unsigned index)
{
    uint32_t value;

    if (hook && hook->automatic &&
        xemu_d3d_hle_discovered_argument(hook, index, &value)) {
        return value;
    }
    return xemu_d3d_hle_stack(env, index + 1u);
}

static void xemu_d3d_hle_begin_pending(CPUX86State *env,
                                       XemuD3DHlePendingKind kind,
                                       const XemuD3DHleHook *hook)
{
    unsigned i;

    if (s_pending.kind == XEMU_D3D_PENDING_DEVICE) {
        g_assert(!s_device_pending_active);
        s_device_pending = s_pending;
        s_device_pending_active = true;
    } else {
        g_assert(s_pending.kind == XEMU_D3D_PENDING_NONE);
        if (kind == XEMU_D3D_PENDING_DEVICE)
            s_bootstrap_deferred_count = 0;
    }
    memset(&s_pending, 0, sizeof(s_pending));
    s_pending.kind = kind;
    s_pending.return_pc = xemu_d3d_hle_stack(env, 0);
    s_pending.entry_eax = (uint32_t)env->regs[R_EAX];
    s_cpu->exec_entry_return_pc = s_pending.return_pc;
    for (i = 0; i < G_N_ELEMENTS(s_pending.args); ++i) {
        if (!hook || !hook->automatic ||
            !xemu_d3d_hle_discovered_argument(
                hook, i, &s_pending.args[i])) {
            s_pending.args[i] = xemu_d3d_hle_stack(env, i + 1u);
        }
    }
    if (kind == XEMU_D3D_PENDING_DEVICE) {
        unsigned parameters_arg = s_profile->create_device_parameters_arg;

        if (hook && hook->automatic)
            parameters_arg = hook->source_param_count == 3u ? 1u : 4u;
        if (parameters_arg < G_N_ELEMENTS(s_pending.args)) {
            s_pending.device_parameters_va =
                s_pending.args[parameters_arg];
        }
    }

    if (kind == XEMU_D3D_PENDING_PIXEL_SHADER) {
        /* The native XDK call may mutate or reuse the caller's temporary
         * definition before it returns. Snapshot it at the guest ABI boundary
         * so the Plume mirror sees the same definition as a direct HLE call. */
        s_pending.pixel_shader_definition_valid = xemu_d3d_hle_read(
            s_pending.args[0], s_pending.pixel_shader_definition,
            sizeof(s_pending.pixel_shader_definition));
    } else if (kind == XEMU_D3D_PENDING_VERTEX_SHADER) {
        uint32_t program_header;
        uint32_t instruction_count;
        bool declaration_valid = true;

        s_pending.vertex_shader_declaration_present =
            s_pending.args[0] != 0;
        if (s_pending.vertex_shader_declaration_present) {
            declaration_valid = xemu_d3d_hle_read(
                s_pending.args[0], s_pending.vertex_shader_declaration,
                sizeof(s_pending.vertex_shader_declaration));
        }
        if (declaration_valid && s_pending.args[1] &&
            xemu_d3d_hle_read_u32(s_pending.args[1], &program_header)) {
            instruction_count = program_header >> 16;
            if (instruction_count &&
                instruction_count <= NV2A_VS_MAX_INSTRUCTIONS) {
                s_pending.vertex_shader_program_dwords =
                    1u + instruction_count * 4u;
                s_pending.vertex_shader_snapshot_valid =
                    xemu_d3d_hle_read(
                        s_pending.args[1], s_pending.vertex_shader_program,
                        s_pending.vertex_shader_program_dwords *
                            sizeof(uint32_t));
            }
        }
    }
}

static bool xemu_d3d_hle_pending_succeeded(
    XemuD3DHlePendingKind kind, uint32_t guest_result)
{
    if (kind == XEMU_D3D_PENDING_BACK_BUFFER ||
        kind == XEMU_D3D_PENDING_RESOURCE ||
        kind == XEMU_D3D_PENDING_VERTEX_BUFFER ||
        kind == XEMU_D3D_PENDING_INDEX_BUFFER)
        return guest_result != 0;
    if (kind == XEMU_D3D_PENDING_SURFACE_LOCK ||
        kind == XEMU_D3D_PENDING_LOCK_3D)
        return true;
    return (HRESULT)guest_result == S_OK;
}

static HRESULT xemu_d3d_hle_mirror_pending(
    const XemuD3DHlePending *pending, uint32_t guest_result)
{
    HRESULT host_result = E_FAIL;

    if (!pending ||
        !xemu_d3d_hle_pending_succeeded(pending->kind, guest_result))
        return S_FALSE;
    switch (pending->kind) {
        case XEMU_D3D_PENDING_DEVICE:
            host_result = xemu_d3d_hle_activate_host_device(
                pending->device_parameters_va);
            break;
        case XEMU_D3D_PENDING_BACK_BUFFER:
            host_result = d3d_hle_guest_mirror_back_buffer(guest_result);
            break;
        case XEMU_D3D_PENDING_RESOURCE:
            host_result = d3d_hle_guest_register_native_resource(
                guest_result);
            break;
        case XEMU_D3D_PENDING_SURFACE_LOCK:
            d3d_hle_guest_note_surface_cpu_write(
                pending->args[0], pending->args[3]);
            host_result = S_OK;
            break;
        case XEMU_D3D_PENDING_LOCK_3D:
            d3d_hle_guest_note_resource_cpu_write(pending->args[0]);
            host_result = S_OK;
            break;
        case XEMU_D3D_PENDING_VERTEX_BUFFER:
            host_result = d3d_hle_guest_register_vertex_buffer(
                guest_result, pending->args[0]);
            break;
        case XEMU_D3D_PENDING_INDEX_BUFFER:
            host_result = d3d_hle_guest_register_index_buffer(
                guest_result, pending->entry_eax);
            break;
        case XEMU_D3D_PENDING_VERTEX_SHADER:
            if (pending->output_handle_valid &&
                pending->vertex_shader_snapshot_valid) {
                host_result =
                    d3d_hle_guest_register_vertex_shader_snapshot(
                    pending->vertex_shader_declaration_present
                        ? pending->vertex_shader_declaration : NULL,
                    pending->vertex_shader_declaration_present
                        ? NV2A_VS_MAX_DECLARATION_DWORDS : 0u,
                    pending->vertex_shader_program,
                    pending->vertex_shader_program_dwords,
                    pending->args[1],
                    pending->output_handle,
                    pending->args[3]);
            } else if (pending->output_handle_valid) {
                host_result = d3d_hle_guest_register_vertex_shader(
                    pending->args[0], pending->args[1],
                    pending->output_handle, pending->args[3]);
            }
            break;
        case XEMU_D3D_PENDING_PIXEL_SHADER:
            if (pending->pixel_shader_definition_valid &&
                pending->output_handle_valid)
                host_result = d3d_hle_guest_register_pixel_shader(
                    pending->pixel_shader_definition,
                    pending->output_handle);
            break;
        default:
            break;
    }
    return host_result;
}

static void xemu_d3d_hle_defer_bootstrap_pending(
    const XemuD3DHlePending *pending, uint32_t guest_result)
{
    if (!xemu_d3d_hle_pending_succeeded(pending->kind, guest_result))
        return;
    if (s_bootstrap_deferred_count == s_bootstrap_deferred_capacity) {
        size_t capacity = s_bootstrap_deferred_capacity
            ? s_bootstrap_deferred_capacity * 2u : 32u;
        s_bootstrap_deferred = g_renew(
            XemuD3DHleDeferred, s_bootstrap_deferred, capacity);
        s_bootstrap_deferred_capacity = capacity;
    }
    s_bootstrap_deferred[s_bootstrap_deferred_count++] =
        (XemuD3DHleDeferred) { *pending, guest_result };
}

static void xemu_d3d_hle_replay_bootstrap(void)
{
    size_t i;

    for (i = 0; i < s_bootstrap_deferred_count; ++i) {
        XemuD3DHleDeferred *deferred = &s_bootstrap_deferred[i];
        HRESULT result = xemu_d3d_hle_mirror_pending(
            &deferred->pending, deferred->guest_result);
        if (result != S_OK && result != S_FALSE) {
            qatomic_set(&s_status, XEMU_D3D_HLE_STATUS_FAILED);
            fprintf(stderr,
                    "[D3D-HLE] bootstrap mirror failed for pending kind %u "
                    "(HRESULT=%08X)\n",
                    (unsigned)deferred->pending.kind, (unsigned)result);
        }
    }
    if (s_bootstrap_deferred_count) {
        fprintf(stderr,
                "[D3D-HLE] replayed %zu native CreateDevice bootstrap "
                "object%s\n",
                s_bootstrap_deferred_count,
                s_bootstrap_deferred_count == 1u ? "" : "s");
    }
    s_bootstrap_deferred_count = 0;
}

static void xemu_d3d_hle_finish_pending(CPUX86State *env)
{
    XemuD3DHlePending completed = s_pending;
    uint32_t guest_result = (uint32_t)env->regs[R_EAX];
    HRESULT host_result;

    if (completed.kind == XEMU_D3D_PENDING_VERTEX_SHADER) {
        completed.output_handle_valid = xemu_d3d_hle_read_u32(
            completed.args[2], &completed.output_handle);
    } else if (completed.kind == XEMU_D3D_PENDING_PIXEL_SHADER) {
        completed.output_handle_valid = xemu_d3d_hle_read_u32(
            completed.args[1], &completed.output_handle);
    }

    if (s_device_pending_active) {
        xemu_d3d_hle_defer_bootstrap_pending(&completed, guest_result);
        s_pending = s_device_pending;
        memset(&s_device_pending, 0, sizeof(s_device_pending));
        s_device_pending_active = false;
        s_cpu->exec_entry_return_pc = s_pending.return_pc;
        return;
    }

    host_result = xemu_d3d_hle_mirror_pending(&completed, guest_result);
    if (completed.kind != XEMU_D3D_PENDING_NONE &&
        host_result != S_OK && host_result != S_FALSE) {
        if (completed.kind == XEMU_D3D_PENDING_DEVICE)
            qatomic_set(&s_status, XEMU_D3D_HLE_STATUS_FAILED);
        fprintf(stderr,
                "[D3D-HLE] host mirror failed for pending kind %u "
                "(HRESULT=%08X)\n",
                (unsigned)completed.kind, (unsigned)host_result);
    }
    if (completed.kind == XEMU_D3D_PENDING_DEVICE &&
        host_result == S_OK)
        xemu_d3d_hle_replay_bootstrap();
    else if (completed.kind == XEMU_D3D_PENDING_DEVICE)
        s_bootstrap_deferred_count = 0;
    memset(&s_pending, 0, sizeof(s_pending));
    s_cpu->exec_entry_return_pc = 0;
}

static HRESULT xemu_d3d_hle_activate_host_device(uint32_t parameters_va)
{
    HRESULT result;

    fprintf(stderr,
            "[D3D-HLE] CreateDevice: parameters=%08X window=%p\n",
            parameters_va, (void *)xemu_get_native_window_handle());
    result = d3d_hle_guest_start_host_device(
        parameters_va, xemu_get_native_window_handle());
    if (result != S_OK)
        return result;
    if (!xgpu_plume_register_debug_overlay_provider(
            xemu_d3d_hle_overlay_provider,
            XGPU_PLUME_DEBUG_OVERLAY_LAYER_MENU)) {
        fprintf(stderr,
                "[D3D-HLE] unable to register xemu HUD overlay; "
                "remaining on the LLE display path\n");
        return E_FAIL;
    }
    s_host_ready = true;
    qatomic_set(&s_status, XEMU_D3D_HLE_STATUS_ACTIVE);
    fprintf(stderr,
            "[D3D-HLE] %s profile active: guest D3D calls now target "
            "Plume\n"
            "[D3D-HLE] xemu HUD overlay provider registered\n",
            s_profile ? s_profile->name : "reviewed");
    return S_OK;
}

static bool xemu_d3d_hle_exec(void *opaque, CPUState *cpu, vaddr linear_pc)
{
    X86CPU *x86 = X86_CPU(cpu);
    CPUX86State *env = &x86->env;
    uint32_t pc = (uint32_t)linear_pc;
    const XemuD3DHleHook *hook;
    uint32_t return_pc;

    (void)opaque;
    if (!s_requested)
        return false;

    if (pc == s_cpu->exec_loader_pc[0] ||
        pc == s_cpu->exec_loader_pc[1] ||
        (s_loader_call_active &&
         pc == s_cpu->exec_loader_return_pc)) {
        if (s_loader_call_active &&
            pc == s_cpu->exec_loader_return_pc) {
            const bool success = (int32_t)env->regs[R_EAX] >= 0;

            s_cpu->exec_loader_return_pc = 0;
            s_loader_call_active = false;
            if (success && !loader_entry_span_mapped &&
                s_loader_section_size) {
                fprintf(stderr,
                        "[D3D-HLE] section commit: va=%08X size=%X\n",
                        s_loader_section_va, s_loader_section_size);
                if (s_host_ready) {
                    xemu_d3d_hle_queue_session_reset();
                } else if (!s_profile_checked &&
                           xemu_d3d_hle_update_coverage()) {
                    xemu_d3d_hle_queue_discovery(s_loader_section_va);
                }
            }
            return false;
        }
        if (!s_loader_call_active) {
            xbe_section_header section;
            uint32_t section_va = xemu_d3d_hle_stack(env, 1);

            memset(&section, 0, sizeof(section));
            s_loader_section_va = 0;
            s_loader_section_size = 0;
            loader_entry_span_mapped = false;
            if (section_va && xemu_d3d_hle_read(
                    section_va, &section, sizeof(section))) {
                s_loader_section_va = section.dwVirtualAddr;
                s_loader_section_size = section.dwSizeofRaw;
                loader_entry_span_mapped =
                    s_loader_section_size &&
                    xemu_d3d_hle_span_fully_mapped(
                        s_loader_section_va, s_loader_section_size);
            }
            s_cpu->exec_loader_return_pc = xemu_d3d_hle_stack(env, 0);
            s_loader_call_active = s_cpu->exec_loader_return_pc != 0;
        }
        return false;
    }

    if (s_pending.kind != XEMU_D3D_PENDING_NONE) {
        if (pc == s_pending.return_pc)
            xemu_d3d_hle_finish_pending(env);
        else if (s_pending.kind != XEMU_D3D_PENDING_DEVICE)
            return false;
    }

    if (!xemu_d3d_hle_resolve_loaded_xbe(pc))
        return false;
    hook = xemu_d3d_hle_profile_find_hook(s_profile, pc);
    if (!hook)
        return false;

    ++s_hook_entry_count;
    s_last_hook_pc = pc;
    s_last_hook_name = hook->name;
    xemu_d3d_hle_load_registers(env);

    if (xemu_d3d_hle_spy_enabled()) {
        unsigned i;
        char args[160];
        size_t used = 0;

        xemu_d3d_hle_spy_note(hook);
        args[0] = '\0';
        if (xgpu_plume_f2_active()) {
            for (i = 0; i < hook->source_param_count && i < 8u; ++i) {
                uint32_t value = 0;
                int n;

                (void)xemu_d3d_hle_discovered_argument(hook, i, &value);
                n = g_snprintf(args + used, sizeof(args) - used,
                               "%sa%u=%08X", used ? " " : "", i, value);
                if (n < 0 || (size_t)n >= sizeof(args) - used)
                    break;
                used += (size_t)n;
            }
            xgpu_plume_f2_log("call %s class=%s %s",
                              hook->name ? hook->name : "?",
                              xemu_d3d_hle_spy_class_name(hook),
                              args);
            if (hook->name &&
                (strcmp(hook->name, "D3DDevice_Swap") == 0 ||
                 strcmp(hook->name, "D3DDevice_Present") == 0))
                xgpu_plume_f2_present(1, "spy-swap", 0, 0);
        }
        return false;
    }

    if (!s_host_ready &&
        strcmp(hook->name, "D3DDevice_LoadVertexShaderProgram") == 0) {
        HRESULT result = d3d_hle_guest_load_vertex_shader_program(
            xemu_d3d_hle_public_argument(env, hook, 0),
            xemu_d3d_hle_public_argument(env, hook, 1));
        if (result != S_OK) {
            fprintf(stderr,
                    "[D3D-HLE] could not capture native vertex program "
                    "during bootstrap (HRESULT=%08X)\n",
                    (unsigned)result);
        }
        return false;
    }

    if (!s_host_ready &&
        s_pending.kind == XEMU_D3D_PENDING_DEVICE) {
        XemuD3DHlePendingKind kind = XEMU_D3D_PENDING_NONE;

        if (pc == s_profile->special.get_back_buffer)
            kind = XEMU_D3D_PENDING_BACK_BUFFER;
        else if (pc == s_profile->special.get_render_target ||
                 pc == s_profile->special.get_depth_stencil ||
                 pc == s_profile->special.create_texture ||
                 pc == s_profile->special.create_surface ||
                 pc == s_profile->special.texture_get_surface_level ||
                 pc == s_profile->special.cube_get_surface_level)
            kind = XEMU_D3D_PENDING_RESOURCE;
        else if (pc == s_profile->special.texture_lock_rect ||
                 pc == s_profile->special.cube_texture_lock_rect ||
                 pc == s_profile->special.volume_texture_lock_box ||
                 pc == s_profile->special.lock_3d_surface)
            kind = XEMU_D3D_PENDING_LOCK_3D;
        else if (pc == s_profile->special.create_vertex_buffer)
            kind = XEMU_D3D_PENDING_VERTEX_BUFFER;
        else if (pc == s_profile->special.create_index_buffer)
            kind = XEMU_D3D_PENDING_INDEX_BUFFER;
        else if (pc == s_profile->special.create_vertex_shader)
            kind = XEMU_D3D_PENDING_VERTEX_SHADER;
        else if (pc == s_profile->special.create_pixel_shader)
            kind = XEMU_D3D_PENDING_PIXEL_SHADER;

        if (kind != XEMU_D3D_PENDING_NONE)
            xemu_d3d_hle_begin_pending(env, kind, hook);
        return false;
    }

    if (s_trace_entries) {
        XemuD3DHleTraceEntry *entry =
            &s_trace_ring[(s_hook_entry_count - 1u) %
                          XEMU_D3D_HLE_TRACE_RING_SIZE];
        entry->hook = s_hook_entry_count;
        entry->pc = pc;
        entry->eax = (uint32_t)env->regs[R_EAX];
        entry->ecx = (uint32_t)env->regs[R_ECX];
        entry->edx = (uint32_t)env->regs[R_EDX];
        entry->esi = (uint32_t)env->regs[R_ESI];
        entry->edi = (uint32_t)env->regs[R_EDI];
        entry->esp = (uint32_t)env->regs[R_ESP];
        entry->return_pc = xemu_d3d_hle_stack(env, 0);
        entry->stack1 = xemu_d3d_hle_stack(env, 1);
        entry->resource_snapshot_valid = false;
        if (pc == s_profile->special.resource_release) {
            uint32_t resource_va =
                xemu_d3d_hle_public_argument(env, hook, 0);
            entry->resource_snapshot_valid =
                xemu_d3d_hle_read_u32(resource_va + 0u,
                                      &entry->resource_common) &&
                xemu_d3d_hle_read_u32(resource_va + 4u,
                                      &entry->resource_data) &&
                xemu_d3d_hle_read_u32(resource_va + 8u,
                                      &entry->resource_lock) &&
                xemu_d3d_hle_read_u32(resource_va + 12u,
                                      &entry->resource_format) &&
                xemu_d3d_hle_read_u32(resource_va + 16u,
                                      &entry->resource_size);
        }
        entry->name = hook->name;
        entry->hle = hook->entry != NULL;
    }

    if (!s_host_ready) {
        HRESULT result;
        bool is_create_device =
            pc == s_profile->special.create_device ||
            (hook->automatic &&
             strcmp(hook->name, "Direct3D_CreateDevice") == 0);

        if (!is_create_device)
            return false;
        if (s_profile->bootstrap ==
            XEMU_D3D_HLE_BOOTSTRAP_MIRROR_NATIVE) {
            xemu_d3d_hle_begin_pending(
                env, XEMU_D3D_PENDING_DEVICE, hook);
            return false;
        }
        result = xemu_d3d_hle_activate_host_device(
            xemu_d3d_hle_stack(
                env, s_profile->create_device_parameters_arg + 1u));
        if (result != S_OK) {
            s_profile_valid = false;
            qatomic_set(&s_status, XEMU_D3D_HLE_STATUS_FAILED);
            fprintf(stderr,
                    "[D3D-HLE] direct CreateDevice bootstrap failed "
                    "(HRESULT=%08X); leaving title on NV2A\n",
                    (unsigned)result);
            return false;
        }
    }

    if (pc == s_profile->special.get_back_buffer) {
        xemu_d3d_hle_begin_pending(
            env, XEMU_D3D_PENDING_BACK_BUFFER, hook);
        return false;
    }

    if (pc == s_profile->special.get_render_target ||
        pc == s_profile->special.get_depth_stencil ||
        pc == s_profile->special.create_texture ||
        pc == s_profile->special.create_surface ||
        pc == s_profile->special.texture_get_surface_level ||
        pc == s_profile->special.cube_get_surface_level) {
        /* Automatic profiles must preserve objects allocated by the title's
         * native XDK heap. Replacing these calls with direct HLE wrappers
         * manufactures incompatible guest pointers and eventually exhausts
         * xemu's small compatibility allocation range. Mirror the returned
         * native resource after the XDK call completes instead. */
        xemu_d3d_hle_begin_pending(
            env, XEMU_D3D_PENDING_RESOURCE, hook);
        return false;
    }

    if (pc == s_profile->special.surface_lock_rect) {
        /* Synchronize any hosted render target before the guest XDK computes
         * the exact Xbox surface address and pitch. */
        xemu_d3d_hle_begin_pending(
            env, XEMU_D3D_PENDING_SURFACE_LOCK, hook);
        d3d_hle_guest_prepare_surface_cpu_lock(s_pending.args[0]);
        return false;
    }

    if (pc == s_profile->special.texture_lock_rect ||
        pc == s_profile->special.cube_texture_lock_rect ||
        pc == s_profile->special.volume_texture_lock_box ||
        pc == s_profile->special.lock_3d_surface) {
        /* Preserve the XDK's exact texture face/mip/volume layout calculation.
         * The HLE 2D lock helper obtains a temporary surface, which would
         * require manufacturing a guest pointer. Plume only needs notification
         * after the native XDK has exposed the writable storage. */
        xemu_d3d_hle_begin_pending(
            env, XEMU_D3D_PENDING_LOCK_3D, hook);
        return false;
    }

    if (pc == s_profile->special.create_vertex_buffer) {
        /* Preserve the XDK's native object and allocation, but retain the
         * caller's requested extent for Plume's bounds checks. */
        xemu_d3d_hle_begin_pending(
            env, XEMU_D3D_PENDING_VERTEX_BUFFER, hook);
        return false;
    }
    if (pc == s_profile->special.create_index_buffer) {
        /* This leaf receives Length in EAX rather than on the stack. */
        xemu_d3d_hle_begin_pending(
            env, XEMU_D3D_PENDING_INDEX_BUFFER, hook);
        return false;
    }

    if (pc == s_profile->special.create_vertex_shader) {
        xemu_d3d_hle_begin_pending(
            env, XEMU_D3D_PENDING_VERTEX_SHADER, hook);
        return false;
    }
    if (pc == s_profile->special.create_pixel_shader) {
        xemu_d3d_hle_begin_pending(
            env, XEMU_D3D_PENDING_PIXEL_SHADER, hook);
        return false;
    }
    if (pc == s_profile->special.delete_vertex_shader) {
        d3d_hle_guest_unregister_vertex_shader(
            xemu_d3d_hle_public_argument(env, hook, 0));
        return false;
    }
    if (pc == s_profile->special.delete_pixel_shader) {
        d3d_hle_guest_unregister_pixel_shader(
            xemu_d3d_hle_public_argument(env, hook, 0));
        return false;
    }
    if (pc == s_profile->special.switch_texture &&
        !d3d_hle_guest_adopt_switch_texture(
            xemu_d3d_hle_public_argument(env, hook, 0),
            xemu_d3d_hle_public_argument(env, hook, 1),
            xemu_d3d_hle_public_argument(env, hook, 2))) {
        fprintf(stderr,
                "[D3D-HLE] SwitchTexture caller did not expose a valid "
                "PixelContainer: stage=%08X data=%08X format=%08X\n",
                xemu_d3d_hle_public_argument(env, hook, 0),
                xemu_d3d_hle_public_argument(env, hook, 1),
                xemu_d3d_hle_public_argument(env, hook, 2));
    }
    if (pc == s_profile->special.resource_release)
        d3d_hle_guest_note_native_resource_release(
            xemu_d3d_hle_public_argument(env, hook, 0));
    if (!hook->entry)
        return false;

    return_pc = xemu_d3d_hle_stack(env, 0);
    s_active_hook_name = hook->name;
    if (hook->automatic) {
        if (!xemu_d3d_hle_invoke_discovered(hook)) {
            s_active_hook_name = NULL;
            fprintf(stderr,
                    "[D3D-HLE] ABI marshal failed for %s at %08X; "
                    "executing the native XDK body\n",
                    hook->name, pc);
            return false;
        }
    } else {
        hook->entry();
    }
    s_active_hook_name = NULL;
    env->regs[R_EAX] = g_eax;
    env->regs[R_EBX] = g_ebx;
    env->regs[R_ECX] = g_ecx;
    env->regs[R_EDX] = g_edx;
    env->regs[R_EBP] = g_ebp;
    env->regs[R_ESI] = g_esi;
    env->regs[R_EDI] = g_edi;
    env->regs[R_ESP] = g_esp;
    env->eip = return_pc - env->segs[R_CS].base;
    return true;
}

static void xemu_d3d_hle_discovery_on_cpu(
    CPUState *cpu, run_on_cpu_data data)
{
    uint32_t generation;
    uint32_t pc;

    (void)cpu;
    (void)data;
    if (!s_discovery_job_queued)
        return;
    generation = s_discovery_job_generation;
    pc = s_discovery_job_pc;
    s_discovery_job_queued = false;
    if (generation != s_generation || !s_identity_valid ||
        s_profile_checked || !xemu_d3d_hle_update_coverage())
        return;
    s_discovery_scanned_epoch = s_coverage.coverage_epoch;
    (void)xemu_d3d_hle_resolve_loaded_xbe(pc);
}

static void xemu_d3d_hle_queue_discovery(uint32_t pc)
{
    if (!s_cpu || s_discovery_job_queued ||
        s_discovery_scanned_epoch == s_coverage.coverage_epoch)
        return;
    s_discovery_job_queued = true;
    s_discovery_job_generation = s_generation;
    s_discovery_job_pc = pc;
    async_run_on_cpu(s_cpu, xemu_d3d_hle_discovery_on_cpu,
                     RUN_ON_CPU_NULL);
}

static bool xemu_d3d_hle_is_entry(void *opaque, vaddr linear_pc)
{
    uint32_t title_id;
    uint32_t timedate;
    uint32_t image_size;

    (void)opaque;
    if (xemu_d3d_hle_read_identity(
            &title_id, &timedate, &image_size)) {
        if (!s_identity_valid) {
            s_identity_title_id = title_id;
            s_identity_timedate = timedate;
            s_identity_image_size = image_size;
            s_identity_valid = true;
            if (!s_generation)
                s_generation = 1;
        } else if (s_identity_title_id != title_id ||
                   s_identity_timedate != timedate ||
                   s_identity_image_size != image_size) {
            xemu_d3d_hle_session_reset("XBE identity changed");
            s_identity_title_id = title_id;
            s_identity_timedate = timedate;
            s_identity_image_size = image_size;
            s_identity_valid = true;
        }
    }
    if (!s_header_valid &&
        xemu_d3d_hle_pc_is_in_loaded_xbe((uint32_t)linear_pc)) {
        s_header_valid = true;
        s_header_valid_ms = xrecomp_host_monotonic_ms();
    }
    if (!s_profile_checked) {
        if (!xemu_d3d_hle_update_coverage()) {
            uint64_t now = xrecomp_host_monotonic_ms();
            if (s_header_valid &&
                now - s_header_valid_ms >= 5000u) {
                s_profile_checked = true;
                s_profile_valid = false;
                g_strlcpy(s_status_detail,
                          "D3D section never fully mapped",
                          sizeof(s_status_detail));
                qatomic_set(&s_status,
                            XEMU_D3D_HLE_STATUS_PROFILE_REJECTED);
                fprintf(stderr,
                        "[D3D-HLE] refusing Plume: D3D section never "
                        "fully mapped; leaving title on NV2A\n");
            } else {
                qatomic_set(&s_status, XEMU_D3D_HLE_STATUS_ARMED);
            }
            return false;
        }
        xemu_d3d_hle_queue_discovery((uint32_t)linear_pc);
        return false;
    }
    return xemu_d3d_hle_find_any_hook((uint32_t)linear_pc) != NULL;
}

void xemu_d3d_hle_install(CPUState *cpu, MemoryRegion *ram)
{
    char error[256];
    const char *request = getenv("XEMU_D3D_FRONTEND");

    if (!cpu || !ram)
        return;
    s_environment_override = request && request[0];
    if (!s_environment_override) {
        request = g_config.display.d3d_frontend ==
                          CONFIG_DISPLAY_D3D_FRONTEND_PLUME
                      ? "hle"
                      : "nv2a";
    }
    s_requested = request && g_ascii_strcasecmp(request, "hle") == 0;
    xemu_d3d_hle_spy_init(s_requested);
    if (!s_requested) {
        qatomic_set(&s_status, XEMU_D3D_HLE_STATUS_DISABLED);
        return;
    }
    qatomic_set(&s_status, XEMU_D3D_HLE_STATUS_ARMED);
    s_trace_entries = getenv("XEMU_D3D_HLE_TRACE_ENTRIES") != NULL;
    s_diagnostics = getenv("XEMU_D3D_HLE_DIAGNOSTICS") != NULL;
    d3d_hle_guest_set_fatal_diagnostic(
        xemu_d3d_hle_dump_trace_ring);
    if (!xrecomp_d3d_frontend_initialize_value(
            request, error, sizeof(error))) {
        qatomic_set(&s_status, XEMU_D3D_HLE_STATUS_FAILED);
        fprintf(stderr, "[D3D-HLE] %s\n", error);
        return;
    }
    s_cpu = cpu;
    s_ram = memory_region_get_ram_ptr(ram);
    s_ram_size = memory_region_size(ram);
    d3d_hle_guest_set_read_range(xemu_d3d_hle_read_range);
    qemu_mutex_init(&s_overlay_mutex);
    s_overlay_initialized = true;
    g_xbox_mem_offset = (ptrdiff_t)s_ram;
    /* All retail title code lives in the user region.  This wide translation
     * gate is needed once at startup so an arbitrary XBE can trigger runtime
     * discovery; the check itself becomes a binary hook lookup afterwards. */
    cpu->exec_entry_min_pc = 0x00010000u;
    cpu->exec_entry_max_pc = 0x07FFFFFFu;
    cpu->exec_entry_check = xemu_d3d_hle_is_entry;
    cpu->exec_entry_callback = xemu_d3d_hle_exec;
    cpu->exec_entry_callback_opaque = NULL;
    cpu->exec_loader_pc[0] = 0;
    cpu->exec_loader_pc[1] = 0;
    cpu->exec_loader_return_pc = 0;
    fprintf(stderr,
            "[D3D-HLE] opt-in armed; waiting for a loaded XBE, then "
            "discovering its linked XDK D3D8 runtime automatically\n");
}

bool xemu_d3d_hle_owns_window(void)
{
    return qatomic_read(&s_host_ready);
}

XemuD3DHleStatus xemu_d3d_hle_status(void)
{
    return qatomic_read(&s_status);
}

bool xemu_d3d_hle_requested(void)
{
    return qatomic_read(&s_requested);
}

bool xemu_d3d_hle_environment_override(void)
{
    return qatomic_read(&s_environment_override);
}

const char *xemu_d3d_hle_active_backend_name(void)
{
    return xemu_d3d_hle_owns_window()
               ? xgpu_plume_get_active_backend_name()
               : NULL;
}

const char *xemu_d3d_hle_active_profile_name(void)
{
    return s_profile_valid && s_profile ? s_profile->name : NULL;
}

const char *xemu_d3d_hle_status_detail(void)
{
    return s_status_detail[0] ? s_status_detail : NULL;
}

void xemu_d3d_hle_overlay_state_changed(bool enabled)
{
    if (enabled) {
        s_overlay_seen = true;
        s_post_fmv = false;
        return;
    }
    if (!s_overlay_seen)
        return;

    s_post_fmv = true;
    s_trace_dumped = false;
    if (s_diagnostics) {
        fprintf(stderr,
                "[D3D-HLE-DIAG] movie overlay retired; post-FMV tracing "
                "armed\n");
    }
}

static void xemu_d3d_hle_dump_trace_ring(void)
{
    uint64_t first;
    uint64_t hook;
    const XemuD3DHleTraceEntry *last_entry;

    if (!s_trace_entries || s_trace_dumped)
        return;
    s_trace_dumped = true;
    first = s_hook_entry_count > XEMU_D3D_HLE_TRACE_RING_SIZE
        ? s_hook_entry_count - XEMU_D3D_HLE_TRACE_RING_SIZE + 1u
        : 1u;
    fprintf(stderr,
            "[D3D-HLE-RING] dumping hooks %llu..%llu after guest "
            "halt/fatal\n",
            (unsigned long long)first,
            (unsigned long long)s_hook_entry_count);
    last_entry = s_hook_entry_count
        ? &s_trace_ring[(s_hook_entry_count - 1u) %
                        XEMU_D3D_HLE_TRACE_RING_SIZE]
        : NULL;
    if (last_entry && last_entry->hook == s_hook_entry_count &&
        s_profile && last_entry->pc == s_profile->special.set_texture) {
        uint32_t common, data, lock, format, size, parent;
        uint32_t resource_va = last_entry->stack1;
        if (xemu_d3d_hle_read_u32(resource_va + 0u, &common) &&
            xemu_d3d_hle_read_u32(resource_va + 4u, &data) &&
            xemu_d3d_hle_read_u32(resource_va + 8u, &lock) &&
            xemu_d3d_hle_read_u32(resource_va + 12u, &format) &&
            xemu_d3d_hle_read_u32(resource_va + 16u, &size) &&
            xemu_d3d_hle_read_u32(resource_va + 20u, &parent)) {
            fprintf(stderr,
                    "[D3D-HLE-FATAL-RESOURCE] hook=%llu obj=%08X "
                    "common=%08X data=%08X lock=%08X format=%08X "
                    "size=%08X parent=%08X\n",
                    (unsigned long long)last_entry->hook, resource_va,
                    common, data, lock, format, size, parent);
        }
    }
    for (hook = first; hook <= s_hook_entry_count; ++hook) {
        const XemuD3DHleTraceEntry *entry =
            &s_trace_ring[(hook - 1u) % XEMU_D3D_HLE_TRACE_RING_SIZE];
        if (entry->hook != hook)
            continue;
        fprintf(stderr,
                "[D3D-HLE-RING] hook=%llu pc=%08X name=%s hle=%u "
                "eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
                "esp=%08X ret=%08X stack1=%08X",
                (unsigned long long)entry->hook, entry->pc,
                entry->name ? entry->name : "none", entry->hle ? 1u : 0u,
                entry->eax, entry->ecx, entry->edx, entry->esi,
                entry->edi, entry->esp, entry->return_pc, entry->stack1);
        if (entry->resource_snapshot_valid) {
            fprintf(stderr,
                    " resource={common=%08X data=%08X lock=%08X "
                    "format=%08X size=%08X}",
                    entry->resource_common, entry->resource_data,
                    entry->resource_lock, entry->resource_format,
                    entry->resource_size);
        }
        fputc('\n', stderr);
    }
}

static void xemu_d3d_hle_log_diagnostics(uint64_t now)
{
    static uint64_t last_log_tick;
    static uint64_t last_hook_entry_count;
    CPUX86State *env;
    uint32_t linear_pc;
    uint64_t hook_delta;

    if (!s_diagnostics || !s_post_fmv || !s_cpu ||
        (last_log_tick && now - last_log_tick < 1000u))
        return;

    env = &X86_CPU(s_cpu)->env;
    linear_pc = (uint32_t)(env->eip + env->segs[R_CS].base);
    hook_delta = s_hook_entry_count - last_hook_entry_count;
    last_hook_entry_count = s_hook_entry_count;
    last_log_tick = now;
    fprintf(stderr,
            "[D3D-HLE-DIAG] pc=%08X esp=%08X eax=%08X eflags=%08X "
            "halted=%u stopped=%u exception=%d irq=%08X pending=%u "
            "return=%08X hooks=%llu (+%llu) last=%08X:%s "
            "nv2a-frames=%u fps=%u native-current="
            "{clear=%d,begin=%d,arrays=%d,inline-buf=%d,"
            "inline-arr=%d,inline-elem=%d}\n",
            linear_pc, (uint32_t)env->regs[R_ESP],
            (uint32_t)env->regs[R_EAX], (uint32_t)env->eflags,
            s_cpu->halted ? 1u : 0u, s_cpu->stopped ? 1u : 0u,
            s_cpu->exception_index, s_cpu->interrupt_request,
            (unsigned)s_pending.kind, s_pending.return_pc,
            (unsigned long long)s_hook_entry_count,
            (unsigned long long)hook_delta, s_last_hook_pc,
            s_last_hook_name ? s_last_hook_name : "none",
            g_nv2a_stats.frame_count, g_nv2a_stats.increment_fps,
            g_nv2a_stats.frame_working.counters[NV2A_PROF_CLEAR],
            g_nv2a_stats.frame_working.counters[NV2A_PROF_BEGIN_ENDS],
            g_nv2a_stats.frame_working.counters[NV2A_PROF_DRAW_ARRAYS],
            g_nv2a_stats.frame_working.counters[NV2A_PROF_INLINE_BUFFERS],
            g_nv2a_stats.frame_working.counters[NV2A_PROF_INLINE_ARRAYS],
            g_nv2a_stats.frame_working.counters[NV2A_PROF_INLINE_ELEMENTS]);
    if (s_cpu->halted)
        xemu_d3d_hle_dump_trace_ring();
}

static void xemu_d3d_hle_service_vblank(uint32_t pcrtc_start)
{
    static uint32_t last_present_count;
    static uint32_t last_draw_count;
    static uint32_t fallback_present_count;
    static uint32_t fallback_draw_count;
    static uint64_t last_present_tick;
    static uint64_t last_draw_tick;
    uint32_t present_count;
    uint32_t draw_count;
    uint64_t now;
    bool eligible;
    bool new_activity;
    int selected = 0;

    if (!qatomic_read(&s_host_ready))
        return;

    /* This is independent of guest presents, so captures remain triggerable
     * while the title is in a no-Swap scanout phase. */
    xgpu_plume_f2_poll();
    (void)d3d8_VblankScanout();

    present_count = d3d8_HlePresentCount();
    draw_count = d3d8_HleDrawCount();
    now = xrecomp_host_monotonic_ms();
    xemu_d3d_hle_log_diagnostics(now);
    if (present_count != last_present_count) {
        last_present_count = present_count;
        last_present_tick = now;
    }
    if (draw_count != last_draw_count) {
        last_draw_count = draw_count;
        last_draw_tick = now;
    }

    /* A title may stop issuing Swap and update the PCRTC scanout surface via
     * CPU locks instead. Refresh that surface only after both explicit presents
     * and draws have been idle long enough. The draw guard prevents a slow
     * in-game frame from being exposed halfway through construction. */
    eligible = pcrtc_start != 0 &&
        (!last_present_tick || now - last_present_tick >= 250u) &&
        (!last_draw_tick || now - last_draw_tick >= 250u);
    new_activity = present_count != fallback_present_count ||
                   draw_count != fallback_draw_count;
    if (eligible) {
        selected = new_activity
            ? d3d8_PgraphPresentSurface(pcrtc_start)
            : d3d8_PgraphRefreshSurface(pcrtc_start);
        fallback_present_count = present_count;
        fallback_draw_count = draw_count;
        if (selected)
            d3d8_PresentFrameVblank();
    }
    xgpu_plume_f2_log(
        "xemu vblank pcrtc=%08X presents=%u draws=%u present-idle=%llu "
        "draw-idle=%llu eligible=%u activity=%u selected=%u",
        pcrtc_start, present_count, draw_count,
        (unsigned long long)(last_present_tick
            ? now - last_present_tick : UINT64_MAX),
        (unsigned long long)(last_draw_tick
            ? now - last_draw_tick : UINT64_MAX),
        eligible ? 1u : 0u, new_activity ? 1u : 0u, selected ? 1u : 0u);
}

static void xemu_d3d_hle_vblank_on_cpu(CPUState *cpu, run_on_cpu_data data)
{
    uint32_t pcrtc_start;

    (void)cpu;
    (void)data;
    /* Clear first so a VBlank arriving while this callback runs can enqueue
     * the next service. All renderer calls remain serialized on this vCPU. */
    qatomic_set(&s_vblank_queued, false);
    if (qatomic_read(&s_session_reset_queued))
        return;
    pcrtc_start = qatomic_read(&s_vblank_pcrtc_start);
    xemu_d3d_hle_service_vblank(pcrtc_start);
}

static void xemu_d3d_hle_session_reset_on_cpu(
    CPUState *cpu, run_on_cpu_data data)
{
    (void)cpu;
    (void)data;
    if (!qatomic_read(&s_session_reset_queued))
        return;
    qatomic_set(&s_session_reset_queued, false);
    xemu_d3d_hle_session_reset("XBE header or identity changed");
}

static void xemu_d3d_hle_queue_session_reset(void)
{
    if (!s_cpu)
        return;
    /* Stop every scanout/present path before queueing the CPU0 teardown. The
     * old guest resource addresses may already be unmapped by the loader. */
    qatomic_set(&s_host_ready, false);
    if (qatomic_cmpxchg(&s_session_reset_queued, false, true))
        return;
    async_run_on_cpu(s_cpu, xemu_d3d_hle_session_reset_on_cpu,
                     RUN_ON_CPU_NULL);
}

void xemu_d3d_hle_vblank(uint32_t pcrtc_start)
{
    uint32_t title_id;
    uint32_t timedate;
    uint32_t image_size;

    if (!s_cpu)
        return;
    (void)xemu_d3d_hle_try_resolve_kernel_loader();
    if (qatomic_read(&s_host_ready)) {
        if (!xemu_d3d_hle_read_identity(
                &title_id, &timedate, &image_size) ||
            (s_identity_valid &&
             (s_identity_title_id != title_id ||
              s_identity_timedate != timedate ||
              s_identity_image_size != image_size))) {
            xemu_d3d_hle_queue_session_reset();
            return;
        }
    }
    if (xemu_d3d_hle_spy_enabled()) {
        xgpu_plume_f2_poll();
        xemu_d3d_hle_spy_on_f2_poll(xgpu_plume_f2_active());
        if (xgpu_plume_f2_active() &&
            !xemu_d3d_hle_spy_capture_seen_swap())
            xgpu_plume_f2_present(1, "spy-vblank", 0, 0);
        if (s_profile_valid) {
            g_snprintf(s_status_detail, sizeof(s_status_detail),
                       "D3D8 spy on NV2A: %u symbols, %u called holes",
                       xemu_d3d_hle_spy_symbol_count(),
                       xemu_d3d_hle_spy_called_holes());
        }
    }
    if (!qatomic_read(&s_host_ready))
        return;

    qatomic_set(&s_vblank_pcrtc_start, pcrtc_start);
    if (qatomic_cmpxchg(&s_vblank_queued, false, true))
        return;
    async_run_on_cpu(s_cpu, xemu_d3d_hle_vblank_on_cpu, RUN_ON_CPU_NULL);
}

void xemu_d3d_hle_publish_overlay(const void *pixels, uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height,
                                  uint32_t pitch, bool visible)
{
    size_t size;
    bool changed;

    if (!s_overlay_initialized || !qatomic_read(&s_host_ready))
        return;
    if (visible && (!pixels || !width || !height ||
                    width > UINT32_MAX / 4u || pitch < width * 4u ||
                    height > SIZE_MAX / pitch))
        return;
    size = visible ? (size_t)pitch * height : 0;

    qemu_mutex_lock(&s_overlay_mutex);
    changed = visible != s_overlay_visible;
    if (visible) {
        changed = changed || x != s_overlay_x || y != s_overlay_y ||
                  width != s_overlay_width || height != s_overlay_height ||
                  pitch != s_overlay_pitch || size != s_overlay_size ||
                  !s_overlay_published ||
                  memcmp(s_overlay_published, pixels, size) != 0;
        if (changed) {
            s_overlay_published = g_realloc(s_overlay_published, size);
            memcpy(s_overlay_published, pixels, size);
            s_overlay_size = size;
            s_overlay_x = x;
            s_overlay_y = y;
            s_overlay_width = width;
            s_overlay_height = height;
            s_overlay_pitch = pitch;
        }
    }
    if (changed) {
        s_overlay_visible = visible;
        ++s_overlay_version;
    }
    qemu_mutex_unlock(&s_overlay_mutex);
}

/* The xemu bridge retains guest allocation and lifetime routines. */
uint32_t xbox_HeapAllocRange(uint32_t bytes, uint32_t alignment,
                            uint32_t low, uint32_t high)
{
    (void)bytes; (void)alignment; (void)low; (void)high;
    fprintf(stderr,
            "[D3D-HLE] unexpected synthetic guest allocation in %s\n",
            s_active_hook_name ? s_active_hook_name : "host mirror setup");
    return 0;
}

void xbox_HeapFree(uint32_t address)
{
    (void)address;
}

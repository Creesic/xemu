#include "qemu/osdep.h"

#include "xemu_d3d_hle_spy.h"

#include <stdlib.h>

enum { XEMU_D3D_HLE_SPY_MAX = 384 };

typedef struct XemuD3DHleSpySymbol {
    uint32_t address;
    const char *name;
    const char *class_name;
    uint64_t hits;
} XemuD3DHleSpySymbol;

static bool s_inited;
static bool s_enabled;
static bool s_atexit;
static bool s_bound;
static bool s_f2_was_active;
static bool s_capture_seen_swap;
static uint32_t s_title_id;
static unsigned s_count;
static XemuD3DHleSpySymbol s_syms[XEMU_D3D_HLE_SPY_MAX];
static GPtrArray *s_interned;

static const char *spy_log_path(void)
{
    const char *path = getenv("XEMU_D3D_HLE_SPY_LOG");
    return path && path[0] ? path : "plume_d3d8_census.log";
}

static const char *class_from_hook(const XemuD3DHleHook *hook)
{
    if (!hook)
        return "none";
    switch (hook->policy) {
    case XEMU_D3D_HLE_HOOK_REPLACE:
        return "replace";
    case XEMU_D3D_HLE_HOOK_NATIVE_THEN_MIRROR:
        return "mirror";
    case XEMU_D3D_HLE_HOOK_NATIVE_SAFE:
        return "native-safe";
    case XEMU_D3D_HLE_HOOK_BOOTSTRAP_ONLY:
        return "bootstrap-only";
    case XEMU_D3D_HLE_HOOK_OBSERVE:
        if (hook->observe_class == XEMU_D3D_HLE_OBSERVE_SAFE)
            return "unbound-safe";
        if (hook->observe_class == XEMU_D3D_HLE_OBSERVE_ABI_HOLE)
            return "abi-hole";
        return "unbound-mutating";
    default:
        return "none";
    }
}

static bool class_is_hole(const char *class_name)
{
    return class_name &&
           (strcmp(class_name, "unbound-mutating") == 0 ||
            strcmp(class_name, "abi-hole") == 0);
}

static void spy_dump_atexit(void)
{
    xemu_d3d_hle_spy_dump("exit");
}

void xemu_d3d_hle_spy_init(bool hle_requested)
{
    const char *value;
    int parsed;

    s_inited = true;
    s_enabled = false;
    value = getenv("XEMU_D3D_HLE_SPY");
    if (!value || !value[0])
        return;
    parsed = atoi(value);
    if (parsed <= 0)
        return;
    if (!hle_requested) {
        fprintf(stderr,
                "[D3D-SPY] ignored: Plume frontend is not armed\n");
        fflush(stderr);
        return;
    }
    s_enabled = true;
    if (!s_atexit) {
        atexit(spy_dump_atexit);
        s_atexit = true;
    }
}

bool xemu_d3d_hle_spy_enabled(void)
{
    return s_enabled;
}

const char *xemu_d3d_hle_spy_intern_name(const char *name, size_t length)
{
    char *copy;
    guint i;

    if (!name || !length)
        return "";
    if (!s_interned)
        s_interned = g_ptr_array_new();
    for (i = 0; i < s_interned->len; ++i) {
        const char *existing = g_ptr_array_index(s_interned, i);
        if (strlen(existing) == length && memcmp(existing, name, length) == 0)
            return existing;
    }
    copy = g_strndup(name, length);
    g_ptr_array_add(s_interned, copy);
    return copy;
}

void xemu_d3d_hle_spy_bind(const XemuD3DHleProfile *profile)
{
    size_t i;

    s_bound = false;
    s_count = 0;
    s_title_id = 0;
    s_capture_seen_swap = false;
    if (!s_enabled || !profile || !profile->hooks)
        return;
    s_title_id = profile->xbe_title_id;
    for (i = 0; i < profile->hook_count && i < XEMU_D3D_HLE_SPY_MAX; ++i) {
        const XemuD3DHleHook *hook = &profile->hooks[i];
        s_syms[s_count].address = hook->address;
        s_syms[s_count].name = hook->name ? hook->name : "?";
        s_syms[s_count].class_name = class_from_hook(hook);
        s_syms[s_count].hits = 0;
        s_count++;
    }
    s_bound = s_count > 0;
}

void xemu_d3d_hle_spy_note(const XemuD3DHleHook *hook)
{
    unsigned i;

    if (!s_enabled || !s_bound || !hook)
        return;
    for (i = 0; i < s_count; ++i) {
        if (s_syms[i].address != hook->address)
            continue;
        s_syms[i].hits++;
        if (hook->name &&
            (strcmp(hook->name, "D3DDevice_Swap") == 0 ||
             strcmp(hook->name, "D3DDevice_Present") == 0))
            s_capture_seen_swap = true;
        return;
    }
}

void xemu_d3d_hle_spy_dump(const char *reason)
{
    FILE *log;
    unsigned i;
    unsigned wrapped = 0;
    unsigned unbound_mutating = 0;
    unsigned called_holes = 0;
    unsigned never_called_holes = 0;

    if (!s_enabled || !s_bound)
        return;
    for (i = 0; i < s_count; ++i) {
        if (class_is_hole(s_syms[i].class_name)) {
            if (s_syms[i].hits)
                ++called_holes;
            else
                ++never_called_holes;
            if (strcmp(s_syms[i].class_name, "unbound-mutating") == 0)
                ++unbound_mutating;
        } else if (strcmp(s_syms[i].class_name, "unbound-safe") != 0) {
            ++wrapped;
        }
    }
    log = fopen(spy_log_path(), "a");
    if (!log) {
        fprintf(stderr, "[D3D-SPY] cannot open %s\n", spy_log_path());
        fflush(stderr);
        return;
    }
    fprintf(log,
            "[D3D-SPY] ==== census title=0x%08X symbols=%u wrapped=%u "
            "unbound_mutating=%u called_holes=%u never_called_holes=%u "
            "reason=%s ====\n",
            s_title_id, s_count, wrapped, unbound_mutating, called_holes,
            never_called_holes, reason && reason[0] ? reason : "unspecified");
    for (i = 0; i < s_count; ++i) {
        const char *tag;
        if (class_is_hole(s_syms[i].class_name) && s_syms[i].hits)
            tag = "hole";
        else if (class_is_hole(s_syms[i].class_name) ||
                 strcmp(s_syms[i].class_name, "unbound-safe") == 0)
            tag = "skip";
        else
            tag = "ok  ";
        fprintf(log,
                "[D3D-SPY] %s %s va=%08X hits=%llu class=%s\n",
                tag, s_syms[i].name, s_syms[i].address,
                (unsigned long long)s_syms[i].hits, s_syms[i].class_name);
    }
    fflush(log);
    fclose(log);
}

void xemu_d3d_hle_spy_reset(void)
{
    s_bound = false;
    s_count = 0;
    s_title_id = 0;
    s_capture_seen_swap = false;
    s_f2_was_active = false;
    memset(s_syms, 0, sizeof(s_syms));
}

unsigned xemu_d3d_hle_spy_symbol_count(void)
{
    return s_count;
}

unsigned xemu_d3d_hle_spy_called_holes(void)
{
    unsigned i;
    unsigned holes = 0;

    for (i = 0; i < s_count; ++i) {
        if (class_is_hole(s_syms[i].class_name) && s_syms[i].hits)
            ++holes;
    }
    return holes;
}

const char *xemu_d3d_hle_spy_class_name(const XemuD3DHleHook *hook)
{
    return class_from_hook(hook);
}

void xemu_d3d_hle_spy_on_f2_poll(int active)
{
    int now = active ? 1 : 0;

    if (!s_enabled)
        return;
    if (now && !s_f2_was_active)
        s_capture_seen_swap = false;
    if (s_f2_was_active && !now)
        xemu_d3d_hle_spy_dump("f2");
    s_f2_was_active = now != 0;
}

bool xemu_d3d_hle_spy_capture_seen_swap(void)
{
    return s_capture_seen_swap;
}

/*
 * Reviewed XBE profile registry for the Xbox D3D8 -> Plume frontend.
 *
 * Profiles are generated from fail-closed xrecomp D3D HLE plans. Keep the
 * registry and lookup mechanics title-neutral; individual address sets live
 * in their own generated/reviewed translation units.
 */
#include "qemu/osdep.h"

#include "xemu_d3d_hle_profile.h"

static const XemuD3DHleProfile *const profiles[] = {
    &xemu_d3d_hle_pgr2_profile,
    &xemu_d3d_hle_mm3_profile,
};

static bool profile_error(char *error, size_t error_capacity,
                          const XemuD3DHleProfile *profile,
                          const char *reason)
{
    if (error && error_capacity) {
        g_snprintf(error, error_capacity, "%s: %s",
                   profile && profile->name ? profile->name : "unnamed profile",
                   reason);
    }
    return false;
}

const XemuD3DHleHook *xemu_d3d_hle_profile_find_hook(
    const XemuD3DHleProfile *profile, uint32_t pc)
{
    size_t first = 0;
    size_t last;

    if (!profile || !profile->hooks || !profile->hook_count)
        return NULL;
    last = profile->hook_count;

    /* This runs at every candidate TCG block boundary while HLE is armed. */
    if (pc < profile->hooks[0].address ||
        pc > profile->hooks[profile->hook_count - 1u].address)
        return NULL;
    while (first < last) {
        size_t middle = first + (last - first) / 2;
        if (profile->hooks[middle].address < pc)
            first = middle + 1u;
        else
            last = middle;
    }
    if (first < profile->hook_count && profile->hooks[first].address == pc)
        return &profile->hooks[first];
    return NULL;
}

const XemuD3DHleProfile *const *xemu_d3d_hle_profiles(size_t *count)
{
    if (count)
        *count = G_N_ELEMENTS(profiles);
    return profiles;
}

void xemu_d3d_hle_profile_range(uint32_t *first, uint32_t *last)
{
    size_t i;
    uint32_t minimum = UINT32_MAX;
    uint32_t maximum = 0;

    for (i = 0; i < G_N_ELEMENTS(profiles); ++i) {
        const XemuD3DHleProfile *profile = profiles[i];
        if (!profile->hooks || !profile->hook_count)
            continue;
        minimum = MIN(minimum, profile->hooks[0].address);
        maximum = MAX(maximum,
                      profile->hooks[profile->hook_count - 1u].address);
    }
    if (first)
        *first = minimum == UINT32_MAX ? 0 : minimum;
    if (last)
        *last = maximum;
}

bool xemu_d3d_hle_profiles_validate(char *error, size_t error_capacity)
{
    size_t i;

    for (i = 0; i < G_N_ELEMENTS(profiles); ++i) {
        const XemuD3DHleProfile *profile = profiles[i];
        uint32_t specials[13];
        size_t j;

        if (!profile || !profile->name || !profile->name[0] ||
            !profile->source_xbe_sha256 ||
            strlen(profile->source_xbe_sha256) != 64u ||
            !profile->d3d_section_sha1 ||
            strlen(profile->d3d_section_sha1) != 40u ||
            !profile->hooks || !profile->hook_count) {
            return profile_error(error, error_capacity, profile,
                                 "incomplete identity or hook metadata");
        }
        specials[0] = profile->special.get_back_buffer;
        specials[1] = profile->special.set_texture;
        specials[2] = profile->special.switch_texture;
        specials[3] = profile->special.resource_release;
        specials[4] = profile->special.surface_lock_rect;
        specials[5] = profile->special.create_device;
        specials[6] = profile->special.create_vertex_buffer;
        specials[7] = profile->special.create_index_buffer;
        specials[8] = profile->special.lock_3d_surface;
        specials[9] = profile->special.create_vertex_shader;
        specials[10] = profile->special.delete_vertex_shader;
        specials[11] = profile->special.create_pixel_shader;
        specials[12] = profile->special.delete_pixel_shader;
        if (!profile->reviewed_required_hook_count ||
            profile->reviewed_implemented_hook_count !=
                profile->reviewed_required_hook_count ||
            profile->reviewed_blocker_count != 0u ||
            profile->hook_count < profile->reviewed_implemented_hook_count) {
            return profile_error(error, error_capacity, profile,
                                 "detector readiness is not coverage-complete");
        }
        if (profile->bootstrap != XEMU_D3D_HLE_BOOTSTRAP_MIRROR_NATIVE &&
            profile->bootstrap != XEMU_D3D_HLE_BOOTSTRAP_DIRECT) {
            return profile_error(error, error_capacity, profile,
                                 "unknown CreateDevice bootstrap policy");
        }
        for (j = 0; j < profile->hook_count; ++j) {
            const XemuD3DHleHook *hook = &profile->hooks[j];
            if ((hook->address & 0xFu) != 0u || !hook->name ||
                !hook->name[0] ||
                (j && hook[-1].address >= hook->address)) {
                return profile_error(error, error_capacity, profile,
                                     "hook table is unsorted, duplicated, or unaligned");
            }
        }

        for (j = 0; j < G_N_ELEMENTS(specials); ++j) {
            if (specials[j] &&
                !xemu_d3d_hle_profile_find_hook(profile, specials[j])) {
                return profile_error(error, error_capacity, profile,
                                     "special hook is absent from the hook table");
            }
        }
        if (!profile->special.create_device) {
            return profile_error(error, error_capacity, profile,
                                 "CreateDevice hook is missing");
        }
        if (profile->bootstrap == XEMU_D3D_HLE_BOOTSTRAP_DIRECT &&
            !xemu_d3d_hle_profile_find_hook(
                 profile, profile->special.create_device)->entry) {
            return profile_error(error, error_capacity, profile,
                                 "direct CreateDevice wrapper is missing");
        }
    }
    if (error && error_capacity)
        error[0] = '\0';
    return true;
}

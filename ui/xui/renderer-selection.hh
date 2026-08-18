#pragma once

#include "common.hh"
#include "widgets.hh"
#include "hw/xbox/d3d_hle/xemu_d3d_hle.h"
#include "ui/xemu-notifications.h"

static inline const char *RendererSelectionItems()
{
    return "Null (NV2A)\0"
           "OpenGL (NV2A)\0"
#ifdef CONFIG_VULKAN
           "Vulkan (NV2A)\0"
#endif
           "Plume (D3D12 HLE)\0";
}

static inline int RendererSelectionPlumeIndex()
{
#ifdef CONFIG_VULKAN
    return 3;
#else
    return 2;
#endif
}

static inline int RendererSelectionCurrent()
{
    if (xemu_d3d_hle_environment_override()) {
        return xemu_d3d_hle_requested()
                   ? RendererSelectionPlumeIndex()
                   : g_config.display.renderer;
    }
    return g_config.display.d3d_frontend ==
                   CONFIG_DISPLAY_D3D_FRONTEND_PLUME
               ? RendererSelectionPlumeIndex()
               : g_config.display.renderer;
}

static inline void RendererSelectionApply(int selection)
{
    if (selection == RendererSelectionPlumeIndex()) {
        g_config.display.d3d_frontend =
            CONFIG_DISPLAY_D3D_FRONTEND_PLUME;
    } else {
        g_config.display.d3d_frontend =
            CONFIG_DISPLAY_D3D_FRONTEND_NV2A;
        g_config.display.renderer = selection;
    }
    xemu_queue_notification(
        "Graphics backend saved; restart xemu to activate it");
}

static inline const char *RendererSelectionStatus()
{
    static char status[384];
    const char *profile = xemu_d3d_hle_active_profile_name();
    const char *detail = xemu_d3d_hle_status_detail();

    switch (xemu_d3d_hle_status()) {
    case XEMU_D3D_HLE_STATUS_UNAVAILABLE:
        return "Active: NV2A (Plume is unavailable in this build)";
    case XEMU_D3D_HLE_STATUS_ARMED:
        if (detail) {
            snprintf(status, sizeof(status), "Plume armed: %s", detail);
            return status;
        }
        return "Plume armed: waiting to scan the loaded game's D3D8 runtime";
    case XEMU_D3D_HLE_STATUS_PROFILE_REJECTED:
        snprintf(status, sizeof(status),
                 "Active: NV2A (D3D8 discovery rejected: %s)",
                 detail ? detail : "no usable dispatch was discovered");
        return status;
    case XEMU_D3D_HLE_STATUS_PROFILE_VERIFIED:
        snprintf(status, sizeof(status), "%s dispatch ready: starting Plume",
                 profile ? profile : "Game");
        return status;
    case XEMU_D3D_HLE_STATUS_ACTIVE:
        snprintf(status, sizeof(status), "Active: Plume (D3D12), %s",
                 profile ? profile : "discovered game");
        return status;
    case XEMU_D3D_HLE_STATUS_FAILED:
        return "Active: NV2A (Plume startup failed)";
    case XEMU_D3D_HLE_STATUS_DISABLED:
        if (g_config.display.d3d_frontend ==
            CONFIG_DISPLAY_D3D_FRONTEND_PLUME) {
            return "Plume selected: restart xemu to activate it";
        }
        return "Active: NV2A";
    }
    return "Active backend: unknown";
}

static inline bool RendererSelectionMenuCombo(const char *label)
{
    int selection = RendererSelectionCurrent();
    bool environment_override = xemu_d3d_hle_environment_override();
    if (environment_override)
        ImGui::BeginDisabled();
    bool changed = ImGui::Combo(
        label, &selection, RendererSelectionItems());
    if (environment_override)
        ImGui::EndDisabled();
    if (changed)
        RendererSelectionApply(selection);
    return changed;
}

static inline bool RendererSelectionChevronCombo(const char *label,
                                                  const char *description)
{
    int selection = RendererSelectionCurrent();
    bool environment_override = xemu_d3d_hle_environment_override();
    if (environment_override)
        ImGui::BeginDisabled();
    bool changed = ChevronCombo(
        label, &selection, RendererSelectionItems(), description);
    if (environment_override)
        ImGui::EndDisabled();
    if (changed)
        RendererSelectionApply(selection);
    return changed;
}

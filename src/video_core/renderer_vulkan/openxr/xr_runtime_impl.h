// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Private to renderer_vulkan/openxr: shared by xr_runtime.cpp and xr_presenter.cpp.

#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "video_core/vulkan_common/vulkan.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "video_core/renderer_vulkan/openxr/xr_runtime.h"

namespace Vulkan::OpenXr {

struct Runtime::Impl {
    std::atomic<bool> active{false};
    std::atomic<bool> off_logged{false};
    XrInstance instance{XR_NULL_HANDLE};
    XrSystemId system{XR_NULL_SYSTEM_ID};
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    std::vector<const char*> instance_extension_ptrs;
    std::vector<const char*> device_extension_ptrs;
    /// XR_FB_display_refresh_rate was offered and enabled (the presenter asks for 120 Hz).
    bool has_refresh_rate{};

    /// "XR_ERROR_..." for a result, or its number when no instance is available.
    std::string ResultName(XrResult result) const;

    /// Asks the runtime for the system again (after a session loss). False = still unavailable.
    bool RecheckSystem();

    void DestroyInstance();
};

/// The one log prefix QA greps for.
inline constexpr std::string_view LogPrefix = "VIZR OpenXR:";

} // namespace Vulkan::OpenXr

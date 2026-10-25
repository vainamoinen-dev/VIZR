// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan::OpenXr {

/**
 * The XR side of one renderer's lifetime (one game launch). Constructed before the VkInstance:
 * it checks --no-vr and the vr_output setting, then asks the OpenXR runtime which Vulkan
 * extensions it needs (XR_KHR_vulkan_enable). Any failure turns XR off for this launch with one
 * "VIZR OpenXR: off (<reason>)" log line, and Eden runs as stock.
 */
class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] bool IsActive() const;

    /// Turns XR off for this launch and logs the reason once.
    void Disable(std::string_view reason);

    /// Turns XR off unless the runtime wants this physical device.
    void CheckPhysicalDevice(VkInstance instance, VkPhysicalDevice physical_device);

    struct Impl;
    [[nodiscard]] Impl* GetImpl() const {
        return impl.get();
    }

private:
    std::unique_ptr<Impl> impl;
};

/// Extensions the active runtime needs; empty when XR is off or not built.
/// The pointers stay valid while the Runtime that produced them is alive.
std::vector<const char*> RequiredInstanceExtensions();
std::vector<const char*> RequiredDeviceExtensions();

/// True while XR is active, so the device asks for a second graphics queue for the XR thread.
bool WantsOwnQueue();

/// Appends `extra` to `list`, skipping names already present.
void AppendUnique(std::vector<const char*>& list, const std::vector<const char*>& extra);

#ifndef HAS_VIZR_OPENXR
struct Runtime::Impl {};
inline Runtime::Runtime() = default;
inline Runtime::~Runtime() = default;
inline bool Runtime::IsActive() const {
    return false;
}
inline void Runtime::Disable(std::string_view) {}
inline void Runtime::CheckPhysicalDevice(VkInstance, VkPhysicalDevice) {}
inline std::vector<const char*> RequiredInstanceExtensions() {
    return {};
}
inline std::vector<const char*> RequiredDeviceExtensions() {
    return {};
}
inline bool WantsOwnQueue() {
    return false;
}
inline void AppendUnique(std::vector<const char*>&, const std::vector<const char*>&) {}
#endif

} // namespace Vulkan::OpenXr

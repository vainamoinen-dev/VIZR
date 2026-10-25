// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <span>

#include "common/common_types.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Tegra {
struct FramebufferConfig;
}

namespace Vulkan {

class Device;
class MemoryAllocator;
class PresentManager;
class RasterizerVulkan;
class Scheduler;
class Swapchain;

namespace OpenXr {

class Runtime;

/**
 * Shows the emulated screen on a quad in the headset.
 * Composite thread: Publish() draws the frame into a 3-image ring with its own BlitScreen, at the
 * game's resolution, before Eden's IsShown() check, and flushes it. It never waits on XR.
 * XR thread: owns the OpenXR session and frame loop, copies the latest finished ring image into
 * the XR swapchain and keeps resubmitting it, so pause and minimize hold the last frame.
 * With XR off, Publish() returns at once and nothing else runs.
 */
class Presenter {
public:
    Presenter(Runtime& runtime, const vk::Instance& instance, const Device& device,
              Scheduler& scheduler, MemoryAllocator& memory_allocator,
              PresentManager& present_manager, Swapchain& swapchain,
              Tegra::MaxwellDeviceMemoryManager& device_memory, RasterizerVulkan& rasterizer);
    ~Presenter();

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    /// Composite thread, before IsShown. `tag` is the M3 pose id.
    void Publish(std::span<const Tegra::FramebufferConfig> framebuffers, u64 tag = 0);

    /// Stops the XR thread and destroys every XR object. Called first in ~RendererVulkan.
    void Stop();

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};

#ifndef HAS_VIZR_OPENXR
struct Presenter::Impl {};
inline Presenter::Presenter(Runtime&, const vk::Instance&, const Device&, Scheduler&,
                            MemoryAllocator&, PresentManager&, Swapchain&,
                            Tegra::MaxwellDeviceMemoryManager&, RasterizerVulkan&) {}
inline Presenter::~Presenter() = default;
inline void Presenter::Publish(std::span<const Tegra::FramebufferConfig>, u64) {}
inline void Presenter::Stop() {}
#endif

} // namespace OpenXr
} // namespace Vulkan

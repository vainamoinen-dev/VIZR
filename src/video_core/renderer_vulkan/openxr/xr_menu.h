// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// VIZR MM: the in-headset mod menu. Dear ImGui drawn into its own
// XR swapchain and submitted as a quad layer; driven by the right-hand laser. XR thread only.

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "common/vizr.h"
#include "video_core/renderer_vulkan/openxr/xr_runtime_impl.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Device;
}

namespace Vulkan::OpenXr {

class Menu {
public:
    struct Input {
        std::optional<std::array<float, 2>> pointer; ///< panel pixels the laser hits, if it does
        bool click = false;                          ///< right trigger
        float scroll = 0.0f;                         ///< rows: +1 = up (one per stick push)
    };

    Menu(const vk::Instance& instance, const Device& device);
    ~Menu();

    bool IsOpen() const {
        return open;
    }
    /// Opens 1.0 m in front of the head (yaw only, upright), or closes.
    void Toggle(const XrPosef& head);
    void Close() {
        open = false;
    }

    /// The visor ring, a small panel 0.6 m in front of the head while the mod menu is closed:
    /// four labels (up, right, down, left), `highlight` 0-3 lit (-1 none). Placed where the head looks when shown.
    void ShowRing(const XrPosef& head, const std::array<std::array<char, 16>, 4>& labels, int highlight);
    void HideRing() {
        ring = false;
    }
    bool RingShown() const {
        return ring && !open;
    }

    /// The panel pixel a ray from the aim pose hits (LOCAL space; the aim looks down its -Z).
    std::optional<std::array<float, 2>> Hit(const XrPosef& aim) const;

    /// Draws one frame of the panel into the next swapchain image. `queue` and `queue_mutex` are the ones
    /// the presenter submits on (ImGui's texture uploads use the same queue, under the same lock).
    /// Returns false when nothing new could be drawn (the layer then shows the last image, if any).
    bool Render(XrSession session, const Input& input, std::mutex& queue_mutex, VkQueue queue);

    /// The quad layer for this frame; only valid after a successful Render.
    void BuildLayer(XrCompositionLayerQuad& quad, XrSpace space) const;
    bool HasImage() const {
        return has_image;
    }

    /// With the session (the swapchain belongs to it). ImGui and the Vulkan objects stay.
    void DestroySwapchain();

    static constexpr u32 Width = 1024;
    static constexpr u32 Height = 768;
    static constexpr float WidthM = 0.8f;
    static constexpr float DistanceM = 1.0f;

private:
    void EnsureSwapchain(XrSession session, std::mutex& queue_mutex, VkQueue queue);
    void InitImGui(VkFormat format, std::mutex& queue_mutex, VkQueue queue);
    void BuildUi();

    const vk::Instance& instance;
    const Device& device;
    bool open = false;
    VIZR::UserSettings launch_snapshot{}; // The settings this launch started with ("Restart VIZR to apply")
    bool launch_snapshot_taken = false;
    XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -DistanceM}};
    void BuildRing();
    bool ring = false;
    XrPosef ring_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -0.6f}};
    std::array<std::array<char, 16>, 4> ring_labels{};
    int ring_highlight = -1;
    static constexpr u32 RingPx = 512;  // the ring uses the top-left RingPx x RingPx of the panel image
    static constexpr float RingM = 0.3f;

    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::vector<XrSwapchainImageVulkanKHR> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    VkRenderPass render_pass{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkCommandPool command_pool{VK_NULL_HANDLE};
    VkCommandBuffer command_buffer{VK_NULL_HANDLE};
    VkFence fence{VK_NULL_HANDLE};
    bool fence_pending = false;
    bool has_image = false;
    bool imgui_ready = false;
    void* imgui_context = nullptr;
    bool was_click = false;
    float scroll_rest = 0.0f;
};

} // namespace Vulkan::OpenXr

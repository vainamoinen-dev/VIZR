// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The menu is drawn with Dear ImGui (MIT, Omar Cornut and contributors); see THIRD-PARTY-VIZR.md.

// VIZR MM: the in-headset mod menu.

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <string>
#include <tuple>

#include <fmt/format.h>

// Eden's Vulkan header first (its platform defines), then ImGui's backend.
#include "video_core/vulkan_common/vulkan.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "common/logging.h"
#include "common/settings.h"
#include "common/vizr.h"
#include "common/vizr_settings.h"
#include "video_core/renderer_vulkan/openxr/menu_style.h"
#include "video_core/renderer_vulkan/openxr/xr_menu.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan::OpenXr {
namespace {

constexpr const char* MenuLog = "VIZR mod menu:";

struct Vec3 {
    float x, y, z;
};

Vec3 Rotate(const XrQuaternionf& q, const Vec3& v) {
    // v' = v + 2 w (q x v) + 2 q x (q x v)
    const Vec3 u{q.x, q.y, q.z};
    const auto cross = [](const Vec3& a, const Vec3& b) {
        return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const Vec3 t = cross(u, v);
    const Vec3 t2{2.0f * t.x, 2.0f * t.y, 2.0f * t.z};
    const Vec3 c = cross(u, t2);
    return {v.x + q.w * t2.x + c.x, v.y + q.w * t2.y + c.y, v.z + q.w * t2.z + c.z};
}

XrQuaternionf Conjugate(const XrQuaternionf& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

PFN_vkVoidFunction LoadVulkan(const char* name, void* user_data) {
    const auto* menu_device = static_cast<const std::pair<const Device*, VkInstance>*>(user_data);
    const auto& dld = menu_device->first->GetDispatchLoader();
    if (dld.vkGetDeviceProcAddr != nullptr) {
        if (const PFN_vkVoidFunction fn = dld.vkGetDeviceProcAddr(*menu_device->first->GetLogical(), name)) {
            return fn;
        }
    }
    return dld.vkGetInstanceProcAddr(menu_device->second, name);
}

} // namespace

Menu::Menu(const vk::Instance& instance_, const Device& device_) : instance{instance_}, device{device_} {}

Menu::~Menu() {
    const auto& dld = device.GetDispatchLoader();
    const VkDevice dev = *device.GetLogical();
    if (fence_pending) {
        dld.vkWaitForFences(dev, 1, &fence, VK_TRUE, 1'000'000'000);
    }
    if (imgui_ready) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(imgui_context));
        ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext(static_cast<ImGuiContext*>(imgui_context));
    }
    DestroySwapchain();
    if (render_pass != VK_NULL_HANDLE) {
        dld.vkDestroyRenderPass(dev, render_pass, nullptr);
    }
    if (fence != VK_NULL_HANDLE) {
        dld.vkDestroyFence(dev, fence, nullptr);
    }
    if (command_pool != VK_NULL_HANDLE) {
        dld.vkDestroyCommandPool(dev, command_pool, nullptr);
    }
}

void Menu::Toggle(const XrPosef& head) {
    open = !open;
    if (!open) {
        return;
    }
    // Upright, yaw of the head, 1 m in front at eye height.
    const Vec3 forward = Rotate(head.orientation, {0.0f, 0.0f, -1.0f});
    const float yaw = std::atan2(-forward.x, -forward.z);
    pose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    pose.position = {head.position.x - std::sin(yaw) * DistanceM, head.position.y,
                     head.position.z - std::cos(yaw) * DistanceM};
    LOG_INFO(Render_Vulkan, "{} open at ({:.2f}, {:.2f}, {:.2f}), yaw {:.0f} deg", MenuLog, pose.position.x,
             pose.position.y, pose.position.z, yaw * 57.2958f);
}

void Menu::ShowRing(const XrPosef& head, const std::array<std::array<char, 16>, 4>& labels, int highlight) {
    if (!ring) {
        // Facing the head, 0.6 m along where it looks (pitch kept: the ring is where the eyes are).
        const Vec3 forward = Rotate(head.orientation, {0.0f, 0.0f, -0.6f});
        ring_pose.orientation = head.orientation;
        ring_pose.position = {head.position.x + forward.x, head.position.y + forward.y, head.position.z + forward.z};
    }
    ring = true;
    ring_labels = labels;
    ring_highlight = highlight;
}

void Menu::BuildRing() {
    const float size = static_cast<float>(RingPx);
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(size, size));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("ring", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 c(size * 0.5f, size * 0.5f);
    draw->AddCircle(c, size * 0.36f, ImGui::GetColorU32(MenuStyle::Col(MenuStyle::Accent, 0.8f)), 64, 6.0f);
    const std::array<ImVec2, 4> at{ImVec2(c.x, c.y - size * 0.36f), ImVec2(c.x + size * 0.36f, c.y),
                                   ImVec2(c.x, c.y + size * 0.36f), ImVec2(c.x - size * 0.36f, c.y)};
    for (std::size_t d = 0; d < at.size(); ++d) {
        const char* label = ring_labels[d].data();
        if (label[0] == '\0') {
            continue;
        }
        const bool lit = static_cast<int>(d) == ring_highlight;
        draw->AddCircleFilled(at[d], size * 0.09f,
                              ImGui::GetColorU32(lit ? MenuStyle::Col(MenuStyle::Accent, 0.95f)
                                                     : MenuStyle::Col(MenuStyle::PanelBg, 0.88f)),
                              32);
        draw->AddCircle(at[d], size * 0.09f, ImGui::GetColorU32(MenuStyle::Col(MenuStyle::Accent, 0.8f)), 32, 2.0f);
        const ImVec2 text = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(at[d].x - text.x * 0.5f, at[d].y - text.y * 0.5f),
                      ImGui::GetColorU32(lit ? MenuStyle::Col(MenuStyle::PanelBg) : MenuStyle::Col(MenuStyle::TextMain)),
                      label);
    }
    ImGui::End();
}

std::optional<std::array<float, 2>> Menu::Hit(const XrPosef& aim) const {
    // Ray in the panel's frame: the panel lies in its z = 0 plane, facing +z.
    const XrQuaternionf inv = Conjugate(pose.orientation);
    const Vec3 origin = Rotate(inv, {aim.position.x - pose.position.x, aim.position.y - pose.position.y,
                                     aim.position.z - pose.position.z});
    const Vec3 dir = Rotate(inv, Rotate(aim.orientation, {0.0f, 0.0f, -1.0f}));
    if (dir.z >= -1e-4f || origin.z <= 0.0f) {
        return std::nullopt; // pointing away, or behind the panel
    }
    const float t = -origin.z / dir.z;
    const float x = origin.x + dir.x * t;
    const float y = origin.y + dir.y * t;
    const float height_m = WidthM * static_cast<float>(Height) / static_cast<float>(Width);
    const float u = x / WidthM + 0.5f;
    const float v = 0.5f - y / height_m;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return std::nullopt;
    }
    return std::array<float, 2>{u * static_cast<float>(Width), v * static_cast<float>(Height)};
}

void Menu::BuildLayer(XrCompositionLayerQuad& quad, XrSpace space) const {
    quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = swapchain;
    if (RingShown()) {
        quad.subImage.imageRect = {{0, 0}, {static_cast<s32>(RingPx), static_cast<s32>(RingPx)}};
        quad.pose = ring_pose;
        quad.size = {RingM, RingM};
        return;
    }
    quad.subImage.imageRect = {{0, 0}, {static_cast<s32>(Width), static_cast<s32>(Height)}};
    quad.pose = pose;
    quad.size = {WidthM, WidthM * static_cast<float>(Height) / static_cast<float>(Width)};
}

void Menu::DestroySwapchain() {
    const auto& dld = device.GetDispatchLoader();
    const VkDevice dev = *device.GetLogical();
    if (fence_pending) {
        dld.vkWaitForFences(dev, 1, &fence, VK_TRUE, 1'000'000'000);
        fence_pending = false;
    }
    for (const VkFramebuffer fb : framebuffers) {
        dld.vkDestroyFramebuffer(dev, fb, nullptr);
    }
    for (const VkImageView view : views) {
        dld.vkDestroyImageView(dev, view, nullptr);
    }
    framebuffers.clear();
    views.clear();
    images.clear();
    if (swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(swapchain);
        swapchain = XR_NULL_HANDLE;
    }
    has_image = false;
}

void Menu::EnsureSwapchain(XrSession session, std::mutex& queue_mutex, VkQueue queue) {
    if (swapchain != XR_NULL_HANDLE) {
        return;
    }
    u32 count = 0;
    xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
    std::vector<s64> formats(count);
    xrEnumerateSwapchainFormats(session, count, &count, formats.data());
    VkFormat chosen = VK_FORMAT_UNDEFINED;
    for (const VkFormat wanted : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_FORMAT_B8G8R8A8_UNORM}) {
        if (std::ranges::find(formats, static_cast<s64>(wanted)) != formats.end()) {
            chosen = wanted;
            break;
        }
    }
    if (chosen == VK_FORMAT_UNDEFINED || (format != VK_FORMAT_UNDEFINED && chosen != format)) {
        LOG_ERROR(Render_Vulkan, "{} no usable swapchain format", MenuLog);
        return;
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    info.format = chosen;
    info.sampleCount = 1;
    info.width = Width;
    info.height = Height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &info, &swapchain))) {
        LOG_ERROR(Render_Vulkan, "{} xrCreateSwapchain failed", MenuLog);
        swapchain = XR_NULL_HANDLE;
        return;
    }
    u32 image_count = 0;
    xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr);
    images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(swapchain, image_count, &image_count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));

    const auto& dld = device.GetDispatchLoader();
    const VkDevice dev = *device.GetLogical();
    if (render_pass == VK_NULL_HANDLE) {
        format = chosen;
        const VkAttachmentDescription attachment{
            .flags = 0,
            .format = format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            // XR_KHR_vulkan_enable: colour swapchain images are released in COLOR_ATTACHMENT_OPTIMAL.
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkSubpassDescription subpass{
            .flags = 0,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .inputAttachmentCount = 0,
            .pInputAttachments = nullptr,
            .colorAttachmentCount = 1,
            .pColorAttachments = &reference,
            .pResolveAttachments = nullptr,
            .pDepthStencilAttachment = nullptr,
            .preserveAttachmentCount = 0,
            .pPreserveAttachments = nullptr,
        };
        const VkSubpassDependency dependency{
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dependencyFlags = 0,
        };
        const VkRenderPassCreateInfo rp_info{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .attachmentCount = 1,
            .pAttachments = &attachment,
            .subpassCount = 1,
            .pSubpasses = &subpass,
            .dependencyCount = 1,
            .pDependencies = &dependency,
        };
        dld.vkCreateRenderPass(dev, &rp_info, nullptr, &render_pass);
        const VkCommandPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = device.GetGraphicsFamily(),
        };
        dld.vkCreateCommandPool(dev, &pool_info, nullptr, &command_pool);
        const VkCommandBufferAllocateInfo alloc{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        dld.vkAllocateCommandBuffers(dev, &alloc, &command_buffer);
        const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
        dld.vkCreateFence(dev, &fence_info, nullptr, &fence);
    }
    for (const auto& image : images) {
        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = image.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .components = {},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkImageView view{};
        dld.vkCreateImageView(dev, &view_info, nullptr, &view);
        views.push_back(view);
        const VkFramebufferCreateInfo fb_info{
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .renderPass = render_pass,
            .attachmentCount = 1,
            .pAttachments = &view,
            .width = Width,
            .height = Height,
            .layers = 1,
        };
        VkFramebuffer fb{};
        dld.vkCreateFramebuffer(dev, &fb_info, nullptr, &fb);
        framebuffers.push_back(fb);
    }
    if (!imgui_ready) {
        InitImGui(format, queue_mutex, queue);
    }
    LOG_INFO(Render_Vulkan, "{} swapchain {}x{} format {} images {}", MenuLog, Width, Height,
             static_cast<int>(format), image_count);
}

void Menu::InitImGui(VkFormat, std::mutex&, VkQueue queue) {
    auto* ctx = ImGui::CreateContext();
    imgui_context = ctx;
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing on disk: the settings live in user.toml
    io.DisplaySize = ImVec2(static_cast<float>(Width), static_cast<float>(Height));
    io.MouseDrawCursor = true; // the laser's dot
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    ImGui::StyleColorsDark();
    // On an sRGB swapchain ImGui's colours (sRGB values) are encoded again and come out washed; every menu
    // colour (theme, header, ring) is handed over linear then (MenuStyle::Col).
    MenuStyle::LinearOutput() = format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB;
    MenuStyle::Apply(); // The visor theme
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(1.5f);  // legible at panel distance in the headset
    style.FontScaleMain = 2.0f; // readable at 0.8 m wide, 1 m away
    style.GrabMinSize = 28.0f;
    style.WindowPadding = ImVec2(24.0f, 18.0f);
    style.ItemSpacing = ImVec2(14.0f, 12.0f);

    static std::pair<const Device*, VkInstance> loader_data;
    loader_data = {&device, *instance};
    ImGui_ImplVulkan_LoadFunctions(device.ApiVersion(), LoadVulkan, &loader_data);
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = device.ApiVersion();
    init.Instance = *instance;
    init.PhysicalDevice = device.GetPhysical();
    init.Device = *device.GetLogical();
    init.QueueFamily = device.GetGraphicsFamily();
    init.Queue = queue;
    init.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
    init.MinImageCount = 2;
    init.ImageCount = 2;
    init.PipelineInfoMain.RenderPass = render_pass;
    init.PipelineInfoMain.Subpass = 0;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    imgui_ready = ImGui_ImplVulkan_Init(&init);
    if (!imgui_ready) {
        LOG_ERROR(Render_Vulkan, "{} ImGui Vulkan backend failed to start", MenuLog);
    }
}

void Menu::BuildUi() {
    namespace Settings = VIZR::Settings;
    // The whole frame edits a copy and stores it at the end: hold the writers' lock from the Load to the Store, so a
    // change from another thread in between is not overwritten.
    const std::scoped_lock settings_lock(VIZR::g_user_settings_write);
    VIZR::UserSettings settings = VIZR::g_user_settings.Load();
    bool changed = false;
    const auto slider = [&](const Settings::Spec& spec, std::size_t element, const std::string& id) {
        float* value = spec.field(settings, element);
        const int decimals = spec.step >= 0.1f ? 1 : 2;
        const std::string fmt_text = fmt::format("%.{}f", decimals);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
        if (ImGui::SliderFloat(fmt::format("{}##{}", VIZR::MenuLabel(spec.key, spec.label), id).c_str(), value, spec.min, spec.max,
                               fmt_text.c_str())) {
            *value = std::round(*value / spec.step) * spec.step;
            changed = true;
        }
    };
    // A choice (radio buttons on one line) for the Compatibility settings.
    const auto choice = [&](const Settings::Spec& spec, const std::string& id) {
        float* value = spec.field(settings, 0);
        ImGui::TextUnformatted(VIZR::MenuLabel(spec.key, spec.label).c_str());
        if (spec.next_launch) {
            ImGui::SameLine();
            ImGui::TextDisabled("(applies at next launch)");
        }
        for (std::size_t i = 0; i < spec.choice_count; ++i) {
            if (i != 0) {
                ImGui::SameLine();
            }
            const bool on = std::abs(*value - spec.choice_values[i]) < 1e-3f;
            if (ImGui::RadioButton(fmt::format("{}##{}.{}", spec.choice_labels[i], id, i).c_str(), on) && !on) {
                *value = spec.choice_values[i];
                changed = true;
            }
        }
    };
    // A setting with choices is a choice,
    // an on/off one (0..1 in steps of 1) a checkbox, any other number a slider.
    const auto widget = [&](const Settings::Spec& spec, std::size_t element, const std::string& id) {
        if (spec.choice_count > 0) {
            choice(spec, id);
            return;
        }
        if (spec.min == 0.0f && spec.max == 1.0f && spec.step == 1.0f) {
            float* value = spec.field(settings, element);
            bool on = *value >= 0.5f;
            if (ImGui::Checkbox(fmt::format("{}##{}", VIZR::MenuLabel(spec.key, spec.label), id).c_str(), &on)) {
                *value = on ? 1.0f : 0.0f;
                changed = true;
            }
            return;
        }
        slider(spec, element, id);
    };
    // What this launch started with, for "Restart VIZR to apply".
    if (!launch_snapshot_taken) {
        launch_snapshot = settings;
        launch_snapshot_taken = true;
    }
    bool restart_pending = false;
    for (const auto& spec : Settings::Table) {
        if (spec.next_launch &&
            std::abs(*spec.field(settings, 0) - *spec.field(launch_snapshot, 0)) > 1e-3f) {
            restart_pending = true;
        }
    }
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(Width), static_cast<float>(Height)));
    ImGui::Begin("VIZR", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);
    // Header: the title in the HUD cyan, the hint dimmed, a cyan rule under both.
    ImGui::PushStyleColor(ImGuiCol_Text, MenuStyle::Col(MenuStyle::Accent));
    ImGui::TextUnformatted("VIZR");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("  both stick clicks or B to close");
    if (const u32 from = VIZR::g_tick_capped_from_hz.load(std::memory_order_relaxed); from != 0) {
        ImGui::TextDisabled("Game frame rate held at %u Hz this session: frames did not keep up at %u Hz",
                            static_cast<unsigned>(VIZR::g_tick_session_cap_hz.load(std::memory_order_relaxed)),
                            static_cast<unsigned>(from));
    }
    if (restart_pending) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, MenuStyle::Col(MenuStyle::Accent));
        ImGui::TextUnformatted("   Restart VIZR to apply");
        ImGui::PopStyleColor();
    }
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y),
                                            ImGui::GetColorU32(MenuStyle::Col(MenuStyle::Accent, 0.7f)), 2.0f);
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("HUD")) {
            for (const auto& spec : Settings::Table) {
                if (spec.scope == Settings::Scope::Whole) {
                    widget(spec, 0, fmt::format("whole.{}", spec.key));
                }
            }
            for (std::size_t e = 0; e < settings.hud.count; ++e) {
                const char* name = settings.hud.names[e].data();
                // A plain label for the element's profile name ("visor_frame" -> "Visor frame"), or the profile's own
                // words ([menu_labels] "hud.<name>"); the ImGui id and the user.toml key stay the profile name.
                std::string plain{name};
                std::replace(plain.begin(), plain.end(), '_', ' ');
                if (!plain.empty()) {
                    plain[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(plain[0])));
                }
                const std::string label = VIZR::MenuLabel(fmt::format("hud.{}", name), plain);
                if (ImGui::CollapsingHeader(fmt::format("{}###hud.{}", label, name).c_str())) {
                    for (const auto& spec : Settings::Table) {
                        if (spec.scope == Settings::Scope::Element) {
                            widget(spec, e, fmt::format("{}.{}", name, spec.key));
                        }
                    }
                    if (ImGui::Button(fmt::format("Reset {}###reset.{}", label, name).c_str())) {
                        for (const auto& spec : Settings::Table) {
                            if (spec.scope == Settings::Scope::Element) {
                                *spec.field(settings, e) = spec.def;
                            }
                        }
                        changed = true;
                    }
                }
            }
            ImGui::EndTabItem();
        }
        // One tab per group of the settings table (Settings:MenuTab).
        static constexpr std::array<std::tuple<Settings::Tab, const char*, const char*>, 4> tabs{{
            {Settings::Tab::Comfort, "tab.comfort", "Comfort"},
            {Settings::Tab::Body, "tab.body", "Body"},
            {Settings::Tab::CannonArms, "tab.hands_arms", "Hands & arms"},
            {Settings::Tab::World, "tab.world", "World"},
        }};
        for (const auto& [tab, key, fallback] : tabs) {
            const std::string title = VIZR::MenuLabel(key, fallback) + "###" + key; // the id stays when the label changes
            if (ImGui::BeginTabItem(title.c_str())) {
                for (const auto& spec : Settings::Table) {
                    if (Settings::MenuTab(spec) == tab) {
                        widget(spec, 0, fmt::format("{}.{}", Settings::Section(spec, settings, 0), spec.key));
                    }
                }
                ImGui::EndTabItem();
            }
        }
        // Settings a player can change when a PC struggles. Automatic = today's behaviour.
        if (ImGui::BeginTabItem("Compatibility")) {
            for (const auto& spec : Settings::Table) {
                if (Settings::MenuTab(spec) != Settings::Tab::Compat) {
                    continue;
                }
                choice(spec, fmt::format("compat.{}", spec.key));
                if (std::string_view{spec.key} == "render_scale") {
                    const float up = ::Settings::values.resolution_info.up_factor;
                    ImGui::TextDisabled("now %.3gx, %ux%u", up, static_cast<unsigned>(std::lround(1920.0f * up)),
                                        static_cast<unsigned>(std::lround(1080.0f * up)));
                }
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
            }
            if (ImGui::Button("Low-end GPU preset (2x, smooth shaders, 90 Hz)")) {
                settings.compat_render_scale = 2.0f;
                settings.compat_shaders = 1.0f;
                settings.compat_refresh_hz = 90.0f;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset to automatic")) {
                for (const auto& spec : Settings::Table) {
                    if (spec.scope == Settings::Scope::Compat) {
                        *spec.field(settings, 0) = spec.def;
                    }
                }
                changed = true;
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (ImGui::Button("Close")) {
        open = false;
    }
    ImGui::End();
    if (changed) {
        VIZR::g_user_settings.Store(settings);
        VIZR::g_user_settings_changes.fetch_add(1, std::memory_order_relaxed);
    }
}

bool Menu::Render(XrSession session, const Input& input, std::mutex& queue_mutex, VkQueue queue) {
    EnsureSwapchain(session, queue_mutex, queue);
    if (swapchain == XR_NULL_HANDLE || !imgui_ready) {
        return false;
    }
    const auto& dld = device.GetDispatchLoader();
    const VkDevice dev = *device.GetLogical();
    if (fence_pending) {
        if (dld.vkWaitForFences(dev, 1, &fence, VK_TRUE, 100'000'000) != VK_SUCCESS) {
            return false; // the last frame is still drawing: keep showing it
        }
        fence_pending = false;
    }

    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(imgui_context));
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 90.0f;
    if (input.pointer) {
        io.AddMousePosEvent((*input.pointer)[0], (*input.pointer)[1]);
    } else {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    if (input.click != was_click) {
        io.AddMouseButtonEvent(0, input.click);
        was_click = input.click;
    }
    if (input.scroll != 0.0f) {
        io.AddMouseWheelEvent(0.0f, input.scroll);
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (RingShown()) {
        BuildRing();
    } else {
        BuildUi();
    }
    ImGui::Render();

    u32 index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(swapchain, &acquire, &index))) {
        return false;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(swapchain, &wait);

    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                         VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    dld.vkBeginCommandBuffer(command_buffer, &begin);
    VkClearValue clear{};
    clear.color = RingShown() ? VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}}   // The ring floats, no panel
                              : VkClearColorValue{{0.02f, 0.03f, 0.05f, 0.92f}};
    const VkRenderPassBeginInfo rp_begin{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = nullptr,
        .renderPass = render_pass,
        .framebuffer = framebuffers[index],
        .renderArea = {{0, 0}, {Width, Height}},
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    dld.vkCmdBeginRenderPass(command_buffer, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    {
        // ImGui uploads its textures (the font atlas) on the same queue, inside this call.
        std::scoped_lock lk{queue_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command_buffer);
    }
    dld.vkCmdEndRenderPass(command_buffer);
    dld.vkEndCommandBuffer(command_buffer);
    dld.vkResetFences(dev, 1, &fence);
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    VkResult submitted;
    {
        std::scoped_lock lk{queue_mutex};
        submitted = dld.vkQueueSubmit(queue, 1, &submit, fence);
    }
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain, &release);
    if (submitted != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "{} submit failed: {}", MenuLog, static_cast<int>(submitted));
        return false;
    }
    fence_pending = true;
    has_image = true;
    return true;
}

} // namespace Vulkan::OpenXr

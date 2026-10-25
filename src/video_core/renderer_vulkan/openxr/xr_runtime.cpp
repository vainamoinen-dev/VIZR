// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Uses the OpenXR SDK headers and loader (Apache-2.0, The Khronos Group Inc.); see THIRD-PARTY-VIZR.md.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <fmt/format.h>

#include "common/logging.h"
#include "common/settings.h"
#include "common/vizr.h"
#include "video_core/renderer_vulkan/openxr/xr_runtime_impl.h"

namespace Vulkan::OpenXr {
namespace {

// The renderer's Runtime, read by the instance/device extension hooks in vulkan_common.
std::mutex g_runtime_mutex;
Runtime::Impl* g_runtime = nullptr;

/// Which runtime the loader will pick, logged before xrCreateInstance so QA can prove a bot
/// run used a test runtime and not SteamVR.
struct RuntimeJson {
    std::string path; // empty = none found
    const char* source;
};

RuntimeJson ActiveRuntimeJson() {
    if (const char* json = std::getenv("XR_RUNTIME_JSON"); json != nullptr && *json != '\0') {
        return {json, "XR_RUNTIME_JSON"};
    }
#ifdef _WIN32
    wchar_t value[1024]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
                     RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS) {
        std::string narrow;
        for (const wchar_t* c = value; *c != L'\0'; ++c) {
            narrow.push_back(*c < 128 ? static_cast<char>(*c) : '?');
        }
        return {narrow, "registry ActiveRuntime"};
    }
#else
    // The loader's own search order for the active runtime (XDG config dirs, then /etc/xdg).
    std::vector<std::string> dirs;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        dirs.emplace_back(xdg);
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        dirs.emplace_back(std::string{home} + "/.config");
    }
    dirs.emplace_back("/etc/xdg");
    for (const auto& dir : dirs) {
        const std::string path = dir + "/openxr/1/active_runtime.json";
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return {path, "active_runtime.json"};
        }
    }
#endif
    return {"", "none found"};
}

bool EnvSet(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
}

/// Lower case, '/' -> '\\', for a prefix compare of Windows paths.
std::string NormalizePath(std::string path) {
    for (char& c : path) {
        c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return path;
}

/// Bot mode (VIZR_NO_VR=1 or VIZR_RIG_OUT set): only a runtime JSON under an allowed
/// folder may load, so a frontend bug can never reach Virtual Desktop or SteamVR from a bot run.
/// Allowed folders: VIZR_XR_RUNTIME_ALLOW (';'-separated; the test scripts set it). Unset = none: a test run
/// without an allow list loads no runtime at all (no path is built into the binary).
bool BotModeAllows(const std::string& json) {
    std::string allow;
    if (const char* env = std::getenv("VIZR_XR_RUNTIME_ALLOW"); env != nullptr && *env != '\0') {
        allow = env;
    }
    if (json.empty()) {
        return false;
    }
    const std::string path = NormalizePath(json);
    std::istringstream stream(allow);
    std::string folder;
    while (std::getline(stream, folder, ';')) {
        folder = NormalizePath(folder);
        while (!folder.empty() && folder.back() == '\\') {
            folder.pop_back();
        }
        if (!folder.empty() && path.size() > folder.size() && path.starts_with(folder) &&
            path[folder.size()] == '\\' && path.find("..") == std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> SplitSpaces(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    std::string item;
    while (stream >> item) {
        out.push_back(item);
    }
    return out;
}

template <typename Pfn>
Pfn GetProc(XrInstance instance, const char* name) {
    PFN_xrVoidFunction fn = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &fn))) {
        return nullptr;
    }
    return reinterpret_cast<Pfn>(fn);
}

} // namespace

std::string Runtime::Impl::ResultName(XrResult result) const {
    char name[XR_MAX_RESULT_STRING_SIZE]{};
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, result, name))) {
        return name;
    }
    return fmt::format("XrResult {}", static_cast<int>(result));
}

bool Runtime::Impl::RecheckSystem() {
    if (instance == XR_NULL_HANDLE) {
        return false;
    }
    XrSystemGetInfo get_info{XR_TYPE_SYSTEM_GET_INFO};
    get_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId id = XR_NULL_SYSTEM_ID;
    if (XR_FAILED(xrGetSystem(instance, &get_info, &id))) {
        return false;
    }
    system = id;
    return true;
}

void Runtime::Impl::DestroyInstance() {
    if (instance != XR_NULL_HANDLE) {
        xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
    }
    system = XR_NULL_SYSTEM_ID;
}

Runtime::Runtime() : impl{std::make_unique<Impl>()} {
    // --no-vr and the setting come first and make no XR call at all, so no runtime (SteamVR)
    // is started for flat-screen runs.
    if (VIZR::g_no_vr_flag) {
        Disable("--no-vr");
        return;
    }
    if (!Settings::values.vr_output.GetValue()) {
        Disable("setting");
        return;
    }
    const RuntimeJson json = ActiveRuntimeJson();
    LOG_INFO(Render_Vulkan, "{} runtime JSON: {} ({})", LogPrefix,
             json.path.empty() ? "none" : json.path, json.source);
    // No runtime installed: stop here, before the loader loads anything, so a PC without VR runs Eden as it is.
    if (json.path.empty()) {
        Disable("no OpenXR runtime installed");
        return;
    }
    // Bot-mode kill switch, independent of the frontend's flag parsing (it once ignored --no-vr).
    if ((EnvSet("VIZR_NO_VR") || EnvSet("VIZR_RIG_OUT")) && !BotModeAllows(json.path)) {
        LOG_WARNING(Render_Vulkan, "{} refused {} (bot mode)", LogPrefix,
                    json.path.empty() ? "none" : json.path);
        Disable("bot mode: runtime not allowed");
        return;
    }

    u32 ext_count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr))) {
        Disable("no runtime");
        return;
    }
    std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count,
                                                         exts.data()))) {
        Disable("no runtime");
        return;
    }
    const bool has_vulkan = std::any_of(exts.begin(), exts.end(), [](const auto& e) {
        return std::strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0;
    });
    if (!has_vulkan) {
        Disable("runtime has no XR_KHR_vulkan_enable");
        return;
    }

    // The headset refresh rate (Virtual Desktop and Quest runtimes offer it); optional.
    impl->has_refresh_rate = std::any_of(exts.begin(), exts.end(), [](const auto& e) {
        return std::strcmp(e.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0;
    });
    std::vector<const char*> enabled{XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    if (impl->has_refresh_rate) {
        enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    // Controller interaction profiles that need an extension (Pico, Vive Focus 3, Quest Touch Pro); each is
    // enabled when the runtime offers it, so the presenter can suggest bindings for it.
    for (const char* name : {"XR_BD_controller_interaction", "XR_HTC_vive_focus3_controller_interaction",
                             "XR_FB_touch_controller_pro"}) {
        if (std::any_of(exts.begin(), exts.end(), [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; })) {
            enabled.push_back(name);
        }
    }
    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(create_info.applicationInfo.applicationName, "Eden VIZR",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    create_info.applicationInfo.applicationVersion = 1;
    std::strncpy(create_info.applicationInfo.engineName, "Eden", XR_MAX_ENGINE_NAME_SIZE - 1);
    create_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    create_info.enabledExtensionCount = static_cast<u32>(enabled.size());
    create_info.enabledExtensionNames = enabled.data();
    if (const XrResult result = xrCreateInstance(&create_info, &impl->instance);
        XR_FAILED(result)) {
        impl->instance = XR_NULL_HANDLE;
        Disable(fmt::format("xrCreateInstance failed: {}", impl->ResultName(result)));
        return;
    }

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(impl->instance, &props))) {
        LOG_INFO(Render_Vulkan, "{} runtime {} {}.{}.{}", LogPrefix, props.runtimeName,
                 XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
                 XR_VERSION_PATCH(props.runtimeVersion));
    }

    if (!impl->RecheckSystem()) {
        Disable("no headset");
        return;
    }
    XrSystemProperties system_props{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(impl->instance, impl->system, &system_props))) {
        LOG_INFO(Render_Vulkan, "{} system {}", LogPrefix, system_props.systemName);
    }

    // The render scale from the runtime itself, not the launcher's guess from an old log. The launcher's interface
    // (both variables are set by tools/vizr/launch.ps1; unset = Eden's own resolution setting is kept):
    //   VIZR_RENDER_SCALE=native   pick the smallest Eden scale whose frame reaches kNativeFill of the runtime's
    //                              recommended per-eye height (game.toml [render] scale = "native");
    //   VIZR_RENDER_SCALE_MAX=<x>  never above scale x (after a session that could not hold the game's frame rate).
    // Chosen here, before the rasterizer and texture cache read Settings::values.resolution_info.
    constexpr float kGuestFrameLines = 1080.0f; // a docked Switch frame's height: Eden's 1x
    constexpr float kNativeFill = 0.95f;        // 5% under the runtime's size still counts as reaching it
    if (const char* want = std::getenv("VIZR_RENDER_SCALE"); want != nullptr && std::string_view{want} == "native") {
        u32 count = 0;
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(impl->instance, impl->system,
                                                           XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count,
                                                           nullptr)) &&
            count > 0) {
            std::vector<XrViewConfigurationView> views(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
            xrEnumerateViewConfigurationViews(impl->instance, impl->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                              count, &count, views.data());
            const u32 height = views[0].recommendedImageRectHeight;
            using Setup = Settings::ResolutionSetup;
            constexpr std::array<std::pair<float, Setup>, 4> scales{
                {{1.0f, Setup::Res1X}, {1.5f, Setup::Res3_2X}, {2.0f, Setup::Res2X}, {3.0f, Setup::Res3X}}};
            auto chosen = scales.back();
            for (const auto& entry : scales) {
                if (kGuestFrameLines * entry.first >= kNativeFill * static_cast<float>(height)) {
                    chosen = entry;
                    break;
                }
            }
            // VIZR_RENDER_SCALE_MAX: the launcher's cap from the last session's log; never below 1x.
            if (const char* max_env = std::getenv("VIZR_RENDER_SCALE_MAX"); max_env != nullptr && *max_env != '\0') {
                const float cap = std::max(1.0f, std::strtof(max_env, nullptr));
                if (chosen.first > cap) {
                    auto capped = scales.front();
                    for (const auto& entry : scales) {
                        if (entry.first <= cap) {
                            capped = entry;
                        }
                    }
                    LOG_INFO(Render_Vulkan, "{} render scale {} capped at {} by the launcher (VIZR_RENDER_SCALE_MAX={})",
                             LogPrefix, chosen.first, capped.first, max_env);
                    chosen = capped;
                }
            }
            const auto before = Settings::values.resolution_setup.GetValue();
            Settings::values.resolution_setup.SetValue(chosen.second);
            Settings::UpdateRescalingInfo();
            LOG_INFO(Render_Vulkan, "{} render scale {} from runtime per-eye {} (launcher default was setup {})",
                     LogPrefix, chosen.first, height, static_cast<int>(before));
        } else {
            LOG_WARNING(Render_Vulkan, "{} render scale: runtime gave no view size; keeping the launcher's", LogPrefix);
        }
    }

    // Required by the spec before xrCreateSession. The Meta runtime reports max 1.0.0 through
    // this (v1) extension even though it supports newer Vulkan, so the range is only logged and
    // Eden keeps its own Vulkan version.
    auto get_requirements = GetProc<PFN_xrGetVulkanGraphicsRequirementsKHR>(
        impl->instance, "xrGetVulkanGraphicsRequirementsKHR");
    auto get_instance_exts = GetProc<PFN_xrGetVulkanInstanceExtensionsKHR>(
        impl->instance, "xrGetVulkanInstanceExtensionsKHR");
    auto get_device_exts = GetProc<PFN_xrGetVulkanDeviceExtensionsKHR>(
        impl->instance, "xrGetVulkanDeviceExtensionsKHR");
    if (get_requirements == nullptr || get_instance_exts == nullptr ||
        get_device_exts == nullptr) {
        Disable("XR_KHR_vulkan_enable functions missing");
        return;
    }
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (const XrResult result = get_requirements(impl->instance, impl->system, &requirements);
        XR_FAILED(result)) {
        Disable(fmt::format("xrGetVulkanGraphicsRequirementsKHR failed: {}",
                            impl->ResultName(result)));
        return;
    }
    LOG_INFO(Render_Vulkan, "{} runtime Vulkan range {}.{}.{} - {}.{}.{}", LogPrefix,
             XR_VERSION_MAJOR(requirements.minApiVersionSupported),
             XR_VERSION_MINOR(requirements.minApiVersionSupported),
             XR_VERSION_PATCH(requirements.minApiVersionSupported),
             XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
             XR_VERSION_MINOR(requirements.maxApiVersionSupported),
             XR_VERSION_PATCH(requirements.maxApiVersionSupported));

    const auto query = [this](auto fn) -> std::optional<std::string> {
        u32 length = 0;
        if (XR_FAILED(fn(impl->instance, impl->system, 0, &length, nullptr))) {
            return std::nullopt;
        }
        std::string text(length, '\0');
        if (length != 0 &&
            XR_FAILED(fn(impl->instance, impl->system, length, &length, text.data()))) {
            return std::nullopt;
        }
        text.resize(std::strlen(text.c_str()));
        return text;
    };
    const auto instance_exts = query(get_instance_exts);
    const auto device_exts = query(get_device_exts);
    if (!instance_exts || !device_exts) {
        Disable("could not read the runtime's Vulkan extension lists");
        return;
    }
    LOG_INFO(Render_Vulkan, "{} Vulkan instance extensions: {}", LogPrefix, *instance_exts);
    LOG_INFO(Render_Vulkan, "{} Vulkan device extensions: {}", LogPrefix, *device_exts);
    impl->instance_extensions = SplitSpaces(*instance_exts);
    impl->device_extensions = SplitSpaces(*device_exts);
    for (const auto& name : impl->instance_extensions) {
        impl->instance_extension_ptrs.push_back(name.c_str());
    }
    for (const auto& name : impl->device_extensions) {
        impl->device_extension_ptrs.push_back(name.c_str());
    }

    impl->active = true;
    VIZR::g_xr_active = true;
    std::scoped_lock lk{g_runtime_mutex};
    g_runtime = impl.get();
}

Runtime::~Runtime() {
    {
        std::scoped_lock lk{g_runtime_mutex};
        if (g_runtime == impl.get()) {
            g_runtime = nullptr;
            VIZR::g_xr_active = false;
        }
    }
    impl->DestroyInstance();
}

bool Runtime::IsActive() const {
    return impl->active;
}

void Runtime::Disable(std::string_view reason) {
    impl->active = false;
    {
        std::scoped_lock lk{g_runtime_mutex};
        if (g_runtime == impl.get()) {
            g_runtime = nullptr;
            VIZR::g_xr_active = false;
        }
    }
    if (!impl->off_logged.exchange(true)) {
        LOG_INFO(Render_Vulkan, "{} off ({})", LogPrefix, reason);
    }
    // Free the runtime right away (stops holding SteamVR or Virtual Desktop for nothing).
    // Only safe before a session exists; the presenter disables through its own path first.
    impl->DestroyInstance();
}

void Runtime::CheckPhysicalDevice(VkInstance vk_instance, VkPhysicalDevice physical_device) {
    if (!IsActive()) {
        return;
    }
    auto get_device = GetProc<PFN_xrGetVulkanGraphicsDeviceKHR>(impl->instance,
                                                                 "xrGetVulkanGraphicsDeviceKHR");
    VkPhysicalDevice wanted = VK_NULL_HANDLE;
    if (get_device == nullptr ||
        XR_FAILED(get_device(impl->instance, impl->system, vk_instance, &wanted))) {
        Disable("could not ask the runtime which GPU it uses");
        return;
    }
    if (wanted != physical_device) {
        // Eden's GPU comes from the vulkan_device setting; we never override it.
        Disable("runtime wants a different GPU than the Device setting picks");
    }
}

std::vector<const char*> RequiredInstanceExtensions() {
    std::scoped_lock lk{g_runtime_mutex};
    return g_runtime != nullptr && g_runtime->active ? g_runtime->instance_extension_ptrs
                                                     : std::vector<const char*>{};
}

std::vector<const char*> RequiredDeviceExtensions() {
    std::scoped_lock lk{g_runtime_mutex};
    return g_runtime != nullptr && g_runtime->active ? g_runtime->device_extension_ptrs
                                                     : std::vector<const char*>{};
}

bool WantsOwnQueue() {
    std::scoped_lock lk{g_runtime_mutex};
    return g_runtime != nullptr && g_runtime->active;
}

void AppendUnique(std::vector<const char*>& list, const std::vector<const char*>& extra) {
    for (const char* name : extra) {
        const bool present = std::any_of(list.begin(), list.end(), [name](const char* other) {
            return std::strcmp(name, other) == 0;
        });
        if (!present) {
            list.push_back(name);
        }
    }
}

} // namespace Vulkan::OpenXr

// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// OpenXR presenter: the XR session on its own thread, the head pose for the camera hook, and the frames to the headset.

#include <cstdlib>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/settings.h"
#include "common/vizr.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/framebuffer_config.h"
#include "video_core/present.h"
#include "video_core/renderer_vulkan/openxr/xr_menu.h"
#include "video_core/renderer_vulkan/openxr/xr_presenter.h"
#include "video_core/renderer_vulkan/openxr/xr_runtime_impl.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::OpenXr {
namespace {

// The newest slot per eye (left, right, mono) stays readable while the XR thread holds one
// and Composite draws another.
constexpr int RingSize = 8; // the newest slot per eye, the last two completed eye pairs, the copies in flight
constexpr auto LossRetryInterval = std::chrono::seconds{5};
constexpr auto JoinTimeout = std::chrono::seconds{2};
constexpr u64 SwapchainWaitTimeoutNs = 100'000'000; // 100 ms
constexpr u64 PairCopyWaitNs = 20'000'000;          // Mode C, the right eye's copy waits for the left one
// The comfort vignette (a tunnel): a black texture with a clear centre on a head-locked quad 1 m ahead.
constexpr u32 VignetteSize = 256;      // texels per side
constexpr int VignetteLevels = 10;     // strength steps (the texture is re-uploaded only when the step changes)
constexpr float VignetteQuadM = 3.6f;  // the quad's side at 1 m: +/-61 deg, past every headset's FOV edge

// Where the flat screen sits: LOCAL space, centred on the head pose at session start.
constexpr float QuadDistance = 1.8f; // metres in front
constexpr float QuadWidth = 2.4f;    // metres wide; height follows the game's aspect

/// Calls whose queue-lock hold time is logged (per call, not per span).
enum LockedCall { CallBegin, CallAcquire, CallRelease, CallEnd, CallSubmit, CallCount };
constexpr std::array<const char*, CallCount> LockedCallNames{"begin", "acquire", "release", "end",
                                                             "submit"};

/// The XR swapchain format that keeps Eden's pixel values exactly: a raw copy (no conversion)
/// into an sRGB image of the same channel order gives the runtime the same encoded values the
/// desktop shows; a UNORM image would be treated as linear by the runtime (too bright).
std::array<VkFormat, 2> PreferredXrFormats(VkFormat ring_format) {
    switch (ring_format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM};
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
    default:
        return {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    }
}

/// The game's output resolution (same rule as Eden's screenshots), independent of the window.
Layout::FramebufferLayout GameLayout() {
    const u32 base_height =
        Settings::IsDockedMode() ? Layout::ScreenDocked::Height : Layout::ScreenUndocked::Height;
    const u32 height = static_cast<u32>(
        static_cast<float>(base_height) * Settings::values.resolution_info.up_factor);
    const u32 width = height * 16 / 9;
    return Layout::DefaultFrameLayout(width, height);
}

struct Slot {
    Frame frame{};
    u64 tick{};
    u64 seq{};
    u64 tag{};
    u32 width{};
    u32 height{};
    VkFormat image_format{VK_FORMAT_UNDEFINED};
    // M5 AFR: the eye this image was rendered for (pose ledger of tag - pose_lag_frames), and the
    // tangents of the horizontal extent it was cropped to (left < 0 < right). Both = mono.
    VIZR::Eye eye{VIZR::Eye::Both};
    float tan_left{};
    float tan_right{};
};

/// What the XR thread knows about the eyes, for Composite's per-eye crop.
struct EyeViews {
    std::array<std::array<float, 4>, 2> fov{}; // per eye: angleLeft, angleRight, angleUp, angleDown (rad)
    u32 recommended_width{};
    u32 recommended_height{};
    bool valid{};
};

/// An XR swapchain and the image it last released. M5: one per VIZR:Eye: left and right
/// (AFR eye frames, each at the recommended per-eye size) and mono (the whole game frame).
struct Chain {
    XrSwapchain handle{XR_NULL_HANDLE};
    std::vector<XrSwapchainImageVulkanKHR> images;
    u32 width = 0;
    u32 height = 0;
    VkFormat ring_format = VK_FORMAT_UNDEFINED;
    bool has_image = false;
    u64 tag = 0; // pose id the released image was rendered with
    u64 seq = 0; // ring seq of the released image
    float tan_left = 0.0f, tan_right = 0.0f;
};

/// Thrown inside the XR thread to leave the frame loop; never escapes the thread.
struct XrFatal : std::exception {
    std::string reason;
    explicit XrFatal(std::string reason_) : reason{std::move(reason_)} {}
    const char* what() const noexcept override {
        return reason.c_str();
    }
};

} // namespace

struct Presenter::Impl {
    Impl(Runtime& runtime_, const vk::Instance& instance_, const Device& device_,
         Scheduler& scheduler_, MemoryAllocator& memory_allocator,
         PresentManager& present_manager, Swapchain& swapchain_,
         Tegra::MaxwellDeviceMemoryManager& device_memory, RasterizerVulkan& rasterizer_)
        : runtime{runtime_}, rt{*runtime_.GetImpl()}, instance{instance_}, device{device_},
          scheduler{scheduler_}, allocator{memory_allocator}, swapchain{swapchain_}, rasterizer{rasterizer_},
          blit{device_memory, device_, memory_allocator, present_manager, scheduler_,
               PresentFiltersForDisplay} {}

    // ---- Composite thread ------------------------------------------------------------------

    void Publish(std::span<const Tegra::FramebufferConfig> framebuffers, u64 tag) {
        if (!enabled) {
            return; // XR off: stock Eden, nothing extra.
        }
        try {
            const auto mode = VIZR::g_stereo_mode.load(std::memory_order_relaxed);
            if (!framebuffers.empty()) {
                VIZR::g_present_guest_width = framebuffers[0].width;
                VIZR::g_present_guest_height = framebuffers[0].height;
            }
            // The camera write this frame was rendered with (ledger of tag - pose_lag_frames).
            std::optional<VIZR::PoseLedger::Rendered> rendered;
            if (tag != 0) {
                const u64 lag = VIZR::g_hook_state.pose_lag_frames.load(std::memory_order_relaxed);
                rendered = VIZR::g_pose_ledger.Find(tag > lag ? tag - lag : 0);
            }
            const EyeViews views = eye_views.Load();
            const bool can_crop = rendered && rendered->render_vfov > 0.0f && views.valid &&
                                  views.recommended_width > 0 && views.recommended_height > 0;
            VIZR::Eye drawn = VIZR::Eye::Both;
            if (mode == VIZR::StereoMode::Afr && can_crop && rendered->eye != VIZR::Eye::Both) {
                // M5 AFR: this frame is one eye's.
                drawn = rendered->eye;
                PublishSlot(framebuffers, tag, drawn, &views, rendered->render_vfov);
            } else if (mode == VIZR::StereoMode::ModeC && can_crop && !framebuffers.empty() &&
                       rasterizer.ModeCHeldCurrent(framebuffers[0])) {
                // Mode C: both eyes of this game frame. The presented image holds eye R; the left eye
                // is the held copy of that same image from this frame's marker-R clear
                // (g_modec_present_held makes the display lookup return it). Never a left image of
                // another frame: otherwise mono.
                VIZR::g_modec_present_held = true;
                PublishSlot(framebuffers, tag, VIZR::Eye::Left, &views, rendered->render_vfov);
                VIZR::g_modec_present_held = false;
                PublishSlot(framebuffers, tag, VIZR::Eye::Right, &views, rendered->render_vfov);
                VIZR::g_modec_stats.paired.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (mode == VIZR::StereoMode::ModeC) {
                    VIZR::g_modec_stats.stale.fetch_add(1, std::memory_order_relaxed);
                }
                PublishSlot(framebuffers, tag, VIZR::Eye::Both, nullptr, 0.0f);
            }
            // Flat layers (HUD) of the frame drawn next: AFR alternates, so it is the other eye. A
            // repeated or dropped camera write puts one frame's HUD at the wrong disparity, never
            // the world in the wrong eye (that follows the ledger). Mode C: the marker clears set it
            // per eye on the GPU thread.
            if (mode != VIZR::StereoMode::ModeC) {
                VIZR::g_draw_eye = drawn == VIZR::Eye::Left    ? VIZR::Eye::Right
                                       : drawn == VIZR::Eye::Right ? VIZR::Eye::Left
                                                                       : VIZR::Eye::Both;
            }
            ++stat_published; // game frames (mode C draws two slots per frame)
            {
                // The game's frame time (publish to publish), for the once-a-second distribution.
                const s64 now_ns = VIZR::SteadyNowNs();
                std::scoped_lock lock{frame_ms_mutex};
                if (frame_last_ns != 0 && frame_ms.size() < 1024) {
                    frame_ms.push_back(static_cast<float>(now_ns - frame_last_ns) * 1e-6f);
                }
                frame_last_ns = now_ns;
            }
        } catch (const std::exception& e) {
            RequestOff(fmt::format("publish failed: {}", e.what()));
        }
    }

    /// Draws one ring slot: the whole game frame (eye Both), or one eye at the runtime's recommended
    /// per-eye size, cropped horizontally to that eye's FOV (the game frame is wider than an eye sees).
    void PublishSlot(std::span<const Tegra::FramebufferConfig> framebuffers, u64 tag, VIZR::Eye eye,
                     const EyeViews* views, float render_vfov) {
        auto layout = GameLayout();
        float tan_left = 0.0f, tan_right = 0.0f;
        if (eye != VIZR::Eye::Both) {
            const auto& fov = views->fov[static_cast<std::size_t>(eye)];
            const float half_w = std::tan(render_vfov * 0.5f) * static_cast<float>(layout.width) /
                                 static_cast<float>(std::max<u32>(layout.height, 1));
            tan_left = std::clamp(std::tan(fov[0]), -half_w, half_w);
            tan_right = std::clamp(std::tan(fov[1]), -half_w, half_w);
            const float u0 = 0.5f + tan_left / (2.0f * half_w);
            const float u1 = 0.5f + tan_right / (2.0f * half_w);
            eye_framebuffers.assign(framebuffers.begin(), framebuffers.end());
            for (auto& fb : eye_framebuffers) {
                auto rect = fb.crop_rect.IsEmpty()
                                ? Common::Rectangle<int>{0, 0, static_cast<int>(fb.width), static_cast<int>(fb.height)}
                                : fb.crop_rect;
                const float w = static_cast<float>(rect.GetWidth());
                const int left = rect.left;
                rect.left = left + static_cast<int>(std::lround(w * u0));
                rect.right = left + static_cast<int>(std::lround(w * u1));
                fb.crop_rect = rect;
            }
            framebuffers = eye_framebuffers;
            layout = Layout::DefaultFrameLayout(views->recommended_width, views->recommended_height);
            layout.screen = {0, 0, views->recommended_width, views->recommended_height};
        }
        int slot = -1;
        {
            std::scoped_lock lk{slot_mutex};
            for (int i = 0; i < RingSize; ++i) {
                if (i != latest_by_eye[0] && i != latest_by_eye[1] && i != latest_by_eye[2] && i != held &&
                    i != held_pair && i != pairs[0].l && i != pairs[0].r && i != pairs[1].l && i != pairs[1].r) {
                    slot = i;
                    break;
                }
            }
        }
        if (slot < 0) {
            ++stat_no_slot; // impossible while the XR thread holds at most one slot
            return;
        }
        Slot& s = slots[slot];
        scheduler.RequestOutsideRenderPassOperationContext();
        // Creates the slot's image on first use (or after a size/format change) through
        // PresentManager::RecreateFrame, like Eden's own frames. Its render pass goes
        // UNDEFINED -> GENERAL, so the slot rests in GENERAL for the XR copy.
        blit.DrawToFrame(device, rasterizer, &s.frame, framebuffers, layout, RingSize,
                         swapchain.GetImageViewFormat());
        // Submit now, even if the window is hidden and Composite returns early after us.
        const u64 tick = scheduler.Flush();
        {
            std::scoped_lock lk{slot_mutex};
            s.tick = tick;
            s.seq = ++publish_seq;
            s.tag = tag;
            s.width = layout.width;
            s.height = layout.height;
            s.image_format = swapchain.GetImageFormat();
            s.eye = eye;
            s.tan_left = tan_left;
            s.tan_right = tan_right;
            latest = slot;
            latest_by_eye[static_cast<std::size_t>(eye)] = slot;
            // Both eyes of one frame published: a completed pair (the newest two are kept for the copy).
            const int pl = latest_by_eye[static_cast<std::size_t>(VIZR::Eye::Left)];
            const int pr = latest_by_eye[static_cast<std::size_t>(VIZR::Eye::Right)];
            if ((eye == VIZR::Eye::Left || eye == VIZR::Eye::Right) && pl >= 0 && pr >= 0 &&
                slots[pl].tag == slots[pr].tag && (pl != pairs[0].l || pr != pairs[0].r ||
                                                   slots[pl].seq != pairs[0].seq_l || slots[pr].seq != pairs[0].seq_r)) {
                pairs[1] = pairs[0];
                pairs_published.fetch_add(1, std::memory_order_relaxed);
                pairs[0] = PairRec{pl, pr, std::max(slots[pl].tick, slots[pr].tick), slots[pl].seq, slots[pr].seq,
                                   VIZR::SteadyNowNs()};
            }
        }
    }

    // ---- lifetime ----------------------------------------------------------------------------

    void Start() {
        if (!runtime.IsActive()) {
            return;
        }
        enabled = true;
        exited = false;
        thread = std::thread([this] { Run(); });
    }

    void Stop() {
        if (!thread.joinable()) {
            return;
        }
        enabled = false;
        {
            std::scoped_lock lk{wake_mutex};
            stopping = true;
        }
        wake_cv.notify_all();
        // A running session returns from xrWaitFrame within one headset frame; the XR thread
        // then ends the session itself (EndSessionForShutdown).
        std::unique_lock lk{wake_mutex};
        if (exit_cv.wait_for(lk, JoinTimeout, [this] { return exited; })) {
            lk.unlock();
            thread.join();
        } else {
            // A crashed runtime never returned. Leaking beats hanging Eden on Stop; the thread
            // touches no Vulkan object after it wakes, because `leaked` makes it skip teardown.
            LOG_CRITICAL(Render_Vulkan,
                         "{} XR thread did not stop within 2 s; leaking XR objects", LogPrefix);
            leaked = true;
            lk.unlock();
            thread.detach();
        }
    }

    // ---- XR thread ---------------------------------------------------------------------------

    void Run() {
        std::string off_reason;
        try {
            CreateCommandObjects();
            CreateSession();
            while (!stopping) {
                ThrowIfOffRequested();
                PollEvents();
                if (session_lost) {
                    RecoverFromLoss();
                    continue;
                }
                if (!session_running) {
                    WaitFor(std::chrono::milliseconds{50});
                    continue;
                }
                RunFrame();
                LogStatsEverySecond();
            }
            EndSessionForShutdown();
        } catch (const XrFatal& e) {
            off_reason = e.reason;
        } catch (const std::exception& e) {
            off_reason = fmt::format("exception on XR thread: {}", e.what());
        } catch (...) {
            off_reason = "unknown exception on XR thread";
        }
        enabled = false;
        if (!leaked) {
            Teardown();
            if (!off_reason.empty()) {
                runtime.Disable(off_reason); // after the session is gone (destroys the instance)
            }
        }
        {
            std::scoped_lock lk{wake_mutex};
            exited = true;
        }
        exit_cv.notify_all();
    }

    void RequestOff(std::string reason) {
        if (enabled.exchange(false)) {
            {
                std::scoped_lock lk{wake_mutex};
                pending_off = std::move(reason);
            }
            wake_cv.notify_all();
        }
    }

    void ThrowIfOffRequested() {
        std::scoped_lock lk{wake_mutex};
        if (!pending_off.empty()) {
            throw XrFatal(pending_off);
        }
    }

    /// Normal stop: ask the runtime to end the session and follow it through STOPPING, so the
    /// session is ended cleanly before it is destroyed. Bounded; Teardown destroys it anyway.
    void EndSessionForShutdown() {
        if (session == XR_NULL_HANDLE || !session_running) {
            return;
        }
        xrRequestExitSession(session);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
        while (session_running && std::chrono::steady_clock::now() < deadline) {
            PollEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }

    void Check(XrResult result, const char* what) {
        if (XR_SUCCEEDED(result)) {
            return;
        }
        if (result == XR_ERROR_SESSION_LOST) {
            session_lost = true;
            return;
        }
        throw XrFatal(fmt::format("{} failed: {}", what, rt.ResultName(result)));
    }

    /// The mutex that host-syncs the queue the runtime and our copy use: the XR queue's own
    /// Or Eden's submit_mutex when sharing the graphics queue.
    std::mutex& QueueMutex() {
        return own_queue ? device.XrQueueMutex() : scheduler.submit_mutex;
    }

    template <typename F>
    XrResult Locked(LockedCall call, F&& fn) {
        std::scoped_lock lk{QueueMutex()};
        const auto start = std::chrono::steady_clock::now();
        const XrResult result = fn();
        const auto held_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        max_hold_us[call] = std::max<s64>(max_hold_us[call], held_us);
        return result;
    }

    void WaitFor(std::chrono::milliseconds duration) {
        std::unique_lock lk{wake_mutex};
        wake_cv.wait_for(lk, duration, [this] { return stopping || !pending_off.empty(); });
        if (!pending_off.empty()) {
            throw XrFatal(pending_off);
        }
    }

    void CreateCommandObjects() {
        menu = std::make_unique<Menu>(instance, device); // The mod menu (no Vulkan objects until opened)
        const auto& dld = device.GetLogical();
        command_pool = dld.CreateCommandPool({
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = device.GetGraphicsFamily(),
        });
        command_buffers = command_pool.Allocate(1);
        command_buffer = vk::CommandBuffer{command_buffers[0], device.GetDispatchLoader()};
        copy_fence = dld.CreateFence({
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        });
    }

    void CreateSession() {
        XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
        binding.instance = *instance;
        binding.physicalDevice = device.GetPhysical();
        binding.device = *device.GetLogical();
        binding.queueFamilyIndex = device.GetGraphicsFamily();
        // The XR thread's own queue (index 1) when the device has one; else Eden's queue 0.
        own_queue = device.HasOwnXrQueue();
        binding.queueIndex = device.GetXrQueueIndex();
        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
        info.next = &binding;
        info.systemId = rt.system;
        XrResult created = xrCreateSession(rt.instance, &info, &session);
        if (XR_FAILED(created) && own_queue) {
            LOG_WARNING(Render_Vulkan, "{} runtime rejected queue index 1 ({}); sharing Eden's queue",
                        LogPrefix, rt.ResultName(created));
            own_queue = false;
            binding.queueIndex = 0;
            created = xrCreateSession(rt.instance, &info, &session);
        }
        Check(created, "xrCreateSession");
        LogViewConfiguration();
        RequestRefreshRate();
        CreateInput();

        XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        space_info.poseInReferenceSpace.orientation.w = 1.0f;
        Check(xrCreateReferenceSpace(session, &space_info, &space), "xrCreateReferenceSpace");
        // The head (VIEW) is located in LOCAL each frame and published for the hook.
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        Check(xrCreateReferenceSpace(session, &space_info, &view_space), "xrCreateReferenceSpace");
        LOG_INFO(Render_Vulkan, "{} session created", LogPrefix);
    }

    void PollEvents() {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(rt.instance, &event) == XR_SUCCESS) {
            switch (event.type) {
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                throw XrFatal("runtime instance loss pending");
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto& changed =
                    *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                OnStateChanged(changed.state);
                break;
            }
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                // The runtime recentered LOCAL; take a new recenter yaw for poses from changeTime.
                const auto& changed =
                    *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
                if (changed.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                    pending_space_change = changed.changeTime != 0 ? changed.changeTime : 1;
                }
                break;
            }
            default:
                break;
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    void OnStateChanged(XrSessionState state) {
        session_state = state;
        switch (state) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            Check(xrBeginSession(session, &begin), "xrBeginSession");
            session_running = true;
            LOG_INFO(Render_Vulkan, "{} session running", LogPrefix);
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            // Headset off the head, a dashboard, or another XR app in front: not an error.
            Check(xrEndSession(session), "xrEndSession");
            session_running = false;
            LOG_INFO(Render_Vulkan, "{} session stopped by the runtime (waiting for READY)",
                     LogPrefix);
            break;
        case XR_SESSION_STATE_LOSS_PENDING:
            session_lost = true;
            break;
        case XR_SESSION_STATE_EXITING:
            if (stopping) {
                break; // our own EndSessionForShutdown
            }
            // The runtime or the user closed us (e.g. quit from the VR dashboard).
            throw XrFatal("session exited by the runtime");
        default:
            break; // IDLE, SYNCHRONIZED, VISIBLE, FOCUSED: tracked in session_state
        }
    }

    void RecoverFromLoss() {
        LOG_WARNING(Render_Vulkan, "{} session lost; retrying every 5 s", LogPrefix);
        DestroySession();
        session_lost = false;
        while (!stopping) {
            WaitFor(std::chrono::duration_cast<std::chrono::milliseconds>(LossRetryInterval));
            if (stopping) {
                return;
            }
            if (rt.RecheckSystem()) {
                CreateSession();
                return;
            }
        }
    }

    void RunFrame() {
        XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        // Never under the queue lock: this is the call that blocks on the headset.
        Check(xrWaitFrame(session, &wait_info, &frame_state), "xrWaitFrame");
        if (session_lost || stopping) {
            return;
        }
        CountMissedRefreshes(frame_state);
        if (const u64 changes = VIZR::g_user_settings_changes.load(std::memory_order_relaxed); changes != settings_seen) {
            settings_seen = changes; // The Compatibility tab's live settings
            const auto s = VIZR::g_user_settings.Load();
            frame_hold = s.compat_frame_hold >= 0.5f;
            if (s.compat_refresh_hz != refresh_setting) {
                RequestRefreshRate();
            }
        }
        LogEyeFov(frame_state.predictedDisplayTime);
        menu_display_time = frame_state.predictedDisplayTime;
        SyncInput();
        PublishHead(frame_state.predictedDisplayTime);
        XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        Check(Locked(CallBegin, [&] { return xrBeginFrame(session, &begin_info); }),
              "xrBeginFrame");
        if (session_lost) {
            return;
        }

        if (frame_state.shouldRender) {
            CopyLatest();
        }

        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::array<XrCompositionLayerProjectionView, 2> projection_views{};
        std::array<const XrCompositionLayerBaseHeader*, 3> layers{};
        XrCompositionLayerQuad menu_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad vignette_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        u32 layer_count = 0;
        const bool visible = session_state == XR_SESSION_STATE_VISIBLE ||
                             session_state == XR_SESSION_STATE_FOCUSED;
        const Chain& left = chains[static_cast<std::size_t>(VIZR::Eye::Left)];
        const Chain& right = chains[static_cast<std::size_t>(VIZR::Eye::Right)];
        const Chain& mono = chains[static_cast<std::size_t>(VIZR::Eye::Both)];
        // Black while a [fade] state holds (the morph transition's camera flight), but never (MORPH-BALL.md)
        // while the game is in a menu (the camera hook is silent there and the state would be stale).
        const bool faded = (VIZR::g_fade_active.load(std::memory_order_relaxed) && !VIZR::HookSilent() &&
                            VIZR::g_user_settings.Load().comfort_fade >= 0.5f) || // menu "Morph fade"
                           VIZR::SteadyNowNs() < VIZR::g_fade_until_ns.load(std::memory_order_relaxed); // Cut fades
        if (faded != fade_logged) {
            fade_logged = faded;
            const s64 now_ns = VIZR::SteadyNowNs();
            if (faded) {
                fade_start_ns = now_ns;
                LOG_INFO(Render_Vulkan, "{} comfort fade on", LogPrefix);
            } else {
                LOG_INFO(Render_Vulkan, "{} comfort fade off after {} ms", LogPrefix, (now_ns - fade_start_ns) / 1'000'000);
            }
        }
        if (faded) {
            // no game layer this frame: the runtime shows black (the menu and ring still draw)
        } else if (frame_state.shouldRender && visible && show_stereo && left.has_image && right.has_image) {
            // M5 AFR: each eye's newest image with the pose and FOV its camera write used; the eye
            // was decided by that write's ledger entry.
            const auto rendered_left = RenderedFor(left);
            const auto rendered_right = RenderedFor(right);
            // Labels alone could pair a pre-gap image with a new one; the two eye frames
            // must also be neighbours (pose ids at most 2 apart).
            const u64 apart = left.tag > right.tag ? left.tag - right.tag : right.tag - left.tag;
            // Mode C: both images are one frame (same pose id, ledger eye Both).
            const bool same_frame_pair = left.tag == right.tag && rendered_left &&
                                         rendered_left->eye == VIZR::Eye::Both;
            // In mode C gameplay a frame whose pair is incomplete (a copy still busy, a ledger entry aged
            // out) keeps 3D: both eyes' own poses when both are known (at most 2 frames apart), else the last good
            // projection again (the runtime reprojects it). The flat quad stays for passthrough by state only.
            const bool modec_gameplay = frame_hold && // The Compatibility tab's "Hold last 3D frame"
                                        VIZR::g_stereo_mode.load(std::memory_order_relaxed) == VIZR::StereoMode::ModeC &&
                                        VIZR::g_hook_state.camera_active.load(std::memory_order_relaxed);
            const bool modec_near_pair = modec_gameplay && rendered_left && rendered_right &&
                                         rendered_left->eye == VIZR::Eye::Both &&
                                         rendered_right->eye == VIZR::Eye::Both && apart <= 2;
            // Test runs only (VIZR_RIG_DROP_PAIR_EVERY=N): every Nth mode C gameplay frame acts as if its pair were
            // incomplete (as a late copy under a loaded GPU), so the hold below must cover it. The test PC's GPU cannot
            // be loaded enough for real late pairs (Eden's per-eye scale stops at 3x: 45% GPU).
            static const u64 rig_drop_every = [] {
                // Test runs only: honoured only with VIZR_RIG_OUT (the launcher clears it for players).
                const char* rig = std::getenv("VIZR_RIG_OUT");
                const char* v = std::getenv("VIZR_RIG_DROP_PAIR_EVERY");
                return rig != nullptr && *rig != '\0' && v != nullptr ? std::strtoull(v, nullptr, 10) : 0ULL;
            }();
            static u64 rig_drop_count = 0;
            static u64 rig_drop_seen = 0;
            if (rig_drop_every != 0 && ++rig_drop_seen % 600 == 1) {
                LOG_INFO(Render_Vulkan, "{} rig drop-pair: every {}, frame_hold {}, mode C {}, camera_active {}, last good {}",
                         LogPrefix, rig_drop_every, frame_hold,
                         VIZR::g_stereo_mode.load(std::memory_order_relaxed) == VIZR::StereoMode::ModeC,
                         VIZR::g_hook_state.camera_active.load(std::memory_order_relaxed), last_good_valid);
            }
            const bool rig_drop = rig_drop_every != 0 && modec_gameplay && ++rig_drop_count % rig_drop_every == 0;
            if (!rig_drop && rendered_left && rendered_right &&
                ((rendered_left->eye == VIZR::Eye::Left && rendered_right->eye == VIZR::Eye::Right &&
                  apart <= 2) ||
                 same_frame_pair || modec_near_pair)) {
                BuildEyeView(projection_views[0], left, *rendered_left, -0.5f);
                BuildEyeView(projection_views[1], right, *rendered_right, 0.5f);
                FinishProjection(projection, projection_views);
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                ++stat_projection_frames;
                ++stat_stereo_frames;
                if (!same_frame_pair && modec_near_pair) {
                    ++stat_pair_held;
                }
                last_good_views = projection_views;
                last_good_valid = true;
                if (!settings_snapshot_logged) {
                    // What the presenter applies at the first stereo frame; the test runs checks it against
                    // the player's user.toml (a settings store that is read before it is filled fails loudly).
                    settings_snapshot_logged = true;
                    const auto s = VIZR::g_user_settings.Load();
                    LOG_INFO(Render_Vulkan,
                             "{} settings at the first stereo frame: frame_hold {}, refresh_hz {}, render_scale {}, shaders {}, "
                             "gpu_accuracy {}, ball_view {}, ball_vignette {:.2f}, cutscene_view {}, cutscene_vignette {:.2f}, "
                             "true_helmet {}, world_scale {:.2f}",
                             LogPrefix, frame_hold, s.compat_refresh_hz, s.compat_render_scale, s.compat_shaders,
                             s.compat_gpu_accuracy, s.ball_view, s.ball_vignette, s.cutscene_view, s.cutscene_vignette,
                             s.visor_true_helmet, s.world_scale);
                }
            } else if (modec_gameplay && last_good_valid) {
                projection_views = last_good_views;
                FinishProjection(projection, projection_views);
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                ++stat_projection_frames;
                ++stat_stereo_frames;
                ++stat_pair_held;
            } else {
                BuildLayer(quad, left);
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                ++stat_quad_frames;
            }
            layer_count = 1;
        } else if (frame_state.shouldRender && visible && mono.has_image) {
            last_good_valid = false; // passthrough or mono: a later stereo frame starts a fresh pair
            // The image's camera came from hook (tag - pose_lag_frames). Submit it with
            // the pose it was rendered with; no pose (passthrough, no head, aged out) = the quad.
            const auto rendered = RenderedFor(mono);
            if (rendered && rendered->pose.valid && rendered->render_vfov > 0.0f) {
                BuildProjection(projection, projection_views, mono, *rendered);
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                ++stat_projection_frames;
            } else {
                BuildLayer(quad, mono);
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                ++stat_quad_frames;
            }
            layer_count = 1;
        }

        // The comfort vignette over the 3D game (the camera hook sets its strength from the game camera's
        // own motion), under the menu.
        if (layer_count == 1 && layers[0] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)) {
            const float strength = VIZR::g_vignette.load(std::memory_order_relaxed);
            const int level = strength < 0.05f ? 0
                                               : std::clamp(static_cast<int>(strength * VignetteLevels + 0.5f), 1,
                                                            VignetteLevels);
            if (level > 0 && PrepareVignette(level)) {
                BuildVignette(vignette_quad);
                layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&vignette_quad);
                ++stat_vignette_frames;
            }
        }

        // The mod menu, on top of the game.
        menu_layer_last_frame = false;
        if ((menu->IsOpen() || menu->RingShown()) && frame_state.shouldRender && visible) {
            const vk::Queue queue = own_queue ? device.GetXrQueue() : device.GetGraphicsQueue();
            menu->Render(session, menu_input, QueueMutex(), *queue);
            if (menu->HasImage()) {
                menu->BuildLayer(menu_quad, space);
                layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menu_quad);
                menu_layer_last_frame = true;
                menu_frames_unseen = 0;
                ++stat_menu_layers;
            }
        }

        XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
        end_info.displayTime = frame_state.predictedDisplayTime;
        end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end_info.layerCount = layer_count;
        end_info.layers = layers.data();
        Check(Locked(CallEnd, [&] { return xrEndFrame(session, &end_info); }), "xrEndFrame");
        ++stat_xr_frames;
    }

    /// The head pose at this frame's predicted display time, for the hook.
    /// A failed or untracked locate publishes nothing; the hook keeps the previous pose.
    void PublishHead(XrTime display_time) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (xrLocateSpace(view_space, space, display_time, &location) != XR_SUCCESS) {
            ++stat_head_failed;
            return;
        }
        constexpr XrSpaceLocationFlags needed =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((location.locationFlags & needed) != needed) {
            ++stat_head_failed;
            return;
        }
        const auto& p = location.pose;
        last_head_pose = p; // The mod menu opens in front of it
        head_height = p.position.y; // Crouch
        PublishHands(display_time);
        const float yaw = YawOf(p.orientation);
        // Recenter on the first valid pose, on the recenter button, and for poses
        // from the runtime's reference-space change time on (not on receipt).
        if (!have_recenter || recenter_requested ||
            (pending_space_change != 0 && display_time >= pending_space_change)) {
            recenter_yaw = yaw;
            have_recenter = true;
            stand_height = p.position.y; // Standing height = the head at recenter
            recenter_position = {p.position.x, p.position.y, p.position.z}; // The 6DoF origin
            stand_height_valid = true;
            recenter_requested = false;
            pending_space_change = 0;
            LOG_INFO(Render_Vulkan, "{} recentered (yaw {:.1f} deg)", LogPrefix, yaw * 57.29578f);
            ++recenter_count;
            torso_rebase = true;
        }
        UpdateTorso(p, yaw);
        // The eyes this frame (IPD for the hook's eye offset, FOVs for Composite's eye crop).
        // A failed locate keeps the previous values.
        XrViewLocateInfo view_info{XR_TYPE_VIEW_LOCATE_INFO};
        view_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        view_info.displayTime = display_time;
        view_info.space = space;
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        std::array<XrView, 2> eyes{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
        u32 eye_count = 0;
        if (XR_SUCCEEDED(xrLocateViews(session, &view_info, &view_state, 2, &eye_count, eyes.data())) &&
            eye_count == 2 && (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
            const float dx = eyes[1].pose.position.x - eyes[0].pose.position.x;
            const float dy = eyes[1].pose.position.y - eyes[0].pose.position.y;
            const float dz = eyes[1].pose.position.z - eyes[0].pose.position.z;
            ipd_m = std::sqrt(dx * dx + dy * dy + dz * dz);
            EyeViews views;
            for (std::size_t e = 0; e < 2; ++e) {
                const auto& f = eyes[e].fov;
                views.fov[e] = {f.angleLeft, f.angleRight, f.angleUp, f.angleDown};
            }
            views.recommended_width = recommended_width;
            views.recommended_height = recommended_height;
            views.valid = true;
            eye_views.Store(views);
            VIZR::NoteFlatBasis(flat_basis, ipd_m, headset_vfov);
        }
        VIZR::g_latest_head.Store(VIZR::HeadPose{
            .orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
            .position = {p.position.x, p.position.y, p.position.z},
            .display_time = display_time,
            .sample_ns = VIZR::SteadyNowNs(),
            .recenter_position = recenter_position,
            .recenter_yaw = recenter_yaw,
            .headset_vfov = headset_vfov,
            .ipd_m = ipd_m,
            .torso_offset = torso_valid ? WrapPi(torso_held - torso_ref) : 0.0f,
            .torso_valid = torso_valid,
            .recenter_count = recenter_count,
            .valid = true,
        });
    }

    static float WrapPi(float a) {
        constexpr float Pi = 3.14159265f;
        while (a > Pi) {
            a -= 2.0f * Pi;
        }
        while (a < -Pi) {
            a += 2.0f * Pi;
        }
        return a;
    }

    /// The torso's yaw in LOCAL space, continuously: the head's yaw turned toward the
    /// hands by the Comfort "shoulder weight" (default 0.7) of the angle between the head's facing and the sum of the
    /// horizontal unit directions from the head to each controller, clamped to +/-50 deg, low-passed over the Comfort
    /// "smoothing" time (default 0.25 s). A whole-body turn turns the head and the hands together, so the body follows
    /// it; a head turn alone moves it by (1 - weight) of the turn. The hands count only while both RESTING (below the
    /// shoulders and near the body) for RestHoldNs: a raised or pointing hand (aiming, grappling, scanning) says where
    /// the arm points, not where the torso faces, so the estimate holds then, never more than HoldLimitDeg from the
    /// head's yaw, and drifts toward the head after a sustained head turn (the drift constants). It is published every
    /// frame and the hook turns the body by the small difference each frame. Without both hands it is the head's yaw.
    void UpdateTorso(const XrPosef& p, float head_yaw) {
        const s64 now = VIZR::SteadyNowNs();
        const float dt = torso_ns != 0 ? std::clamp(static_cast<float>(now - torso_ns) * 1e-9f, 0.0f, 0.5f) : 0.0f;
        torso_ns = now;
        const auto& q = p.orientation;
        const auto rotate = [&](const std::array<float, 3>& v) { // v by the head's orientation (LOCAL)
            const std::array<float, 3> u{q.x, q.y, q.z};
            const std::array<float, 3> t{2.0f * (u[1] * v[2] - u[2] * v[1]), 2.0f * (u[2] * v[0] - u[0] * v[2]),
                                         2.0f * (u[0] * v[1] - u[1] * v[0])};
            return std::array<float, 3>{v[0] + q.w * t[0] + (u[1] * t[2] - u[2] * t[1]),
                                        v[1] + q.w * t[1] + (u[2] * t[0] - u[0] * t[2]),
                                        v[2] + q.w * t[2] + (u[0] * t[1] - u[1] * t[0])};
        };
        constexpr float Pi = 3.14159265f;
        const VIZR::UserSettings user = VIZR::g_user_settings.Load();
        if (user.move_forward > 3.5f) {
            // "Body follows the head": the body is the head's yaw every frame (no estimate, hold or drift).
            if (drifting) {
                StopDrift(head_yaw, std::abs(WrapPi(head_yaw - torso_yaw)) * 180.0f / Pi, now, "the body follows the head");
            }
            rest_since_ns = 0;
            hands_feed = false;
            drift_over_ns = 0;
            hold_yaw = head_yaw;
            torso_yaw = head_yaw;
            if (!torso_valid) {
                torso_ref = head_yaw;
                torso_valid = true;
            }
            torso_held = torso_yaw;
            if (torso_rebase) {
                torso_ref = torso_held;
                torso_rebase = false;
            }
            LogBodyRules(p, head_yaw, dt);
            return;
        }
        float target = head_yaw;
        // Resting: well below the eyes (shoulder height is about 0.25 m below them) and within reach of the body.
        const auto resting = [&](const VIZR::HandPose& h) {
            const auto r = rotate(h.position);
            return r[1] < RestBelowHeadM && std::sqrt(r[0] * r[0] + r[2] * r[2]) < RestReachM;
        };
        const bool both_resting = left_hand_view.valid && right_hand_view.valid && resting(left_hand_view) &&
                                  resting(right_hand_view);
        rest_since_ns = both_resting ? (rest_since_ns != 0 ? rest_since_ns : now) : 0;
        const bool was_feeding = hands_feed;
        hands_feed = both_resting && now - rest_since_ns >= RestHoldNs;
        const bool holding = left_hand_view.valid && right_hand_view.valid && !hands_feed && torso_valid;
        if (holding && was_feeding) {
            // The hold keeps the estimate of RestHoldNs ago: a hand on its way up still counted as resting for a moment
            // and turned the estimate (a 0.3 s raise: 5.5 deg); the estimate eases back over the smoothing time.
            s64 best_ns = 0;
            for (const auto& [at_ns, yaw] : torso_history) {
                if (at_ns != 0 && now - at_ns >= RestHoldNs && at_ns > best_ns) {
                    best_ns = at_ns;
                    hold_yaw = yaw;
                }
            }
        } else if (!holding) {
            hold_yaw = torso_yaw; // no hold: a later one starts from here unless it finds an older sample
        }
        torso_history[torso_history_next] = {now, torso_yaw};
        torso_history_next = (torso_history_next + 1) % torso_history.size();
        if (holding) {
            // Aiming or reaching: hold, within HoldLimitDeg of the head (a whole-body turn while aiming still turns the
            // body). The hold is not sticky: a real body follows a sustained head turn even while the arms aim, so
            // once the head has stayed more than DriftStartDeg from the estimate for DriftArmNs, the held yaw eases
            // toward the head (time constant DriftTauS) until they are within DriftStopDeg.
            const float limit = HoldLimitDeg * Pi / 180.0f;
            hold_yaw = WrapPi(head_yaw + std::clamp(WrapPi(hold_yaw - head_yaw), -limit, limit));
            const float gap_deg = std::abs(WrapPi(head_yaw - torso_yaw)) * 180.0f / Pi;
            if (!drifting) {
                drift_over_ns = gap_deg > DriftStartDeg ? (drift_over_ns != 0 ? drift_over_ns : now) : 0;
                if (drift_over_ns != 0 && now - drift_over_ns >= DriftArmNs) {
                    drifting = true;
                    drift_since_ns = now;
                    LOG_INFO(Render_Vulkan, "{} body drift: start (head {:.1f} ours {:.1f} deg, {:.1f} apart for {:.1f} s)",
                             LogPrefix, head_yaw * 180.0f / Pi, torso_yaw * 180.0f / Pi, gap_deg,
                             static_cast<float>(now - drift_over_ns) * 1e-9f);
                }
            } else if (gap_deg < DriftStopDeg) {
                StopDrift(head_yaw, gap_deg, now, "caught up");
            }
            if (drifting) {
                hold_yaw = WrapPi(hold_yaw + (1.0f - std::exp(-dt / DriftTauS)) * WrapPi(head_yaw - hold_yaw));
            }
            target = hold_yaw;
        } else if (left_hand_view.valid && right_hand_view.valid) {
            if (drifting) {
                StopDrift(head_yaw, std::abs(WrapPi(head_yaw - torso_yaw)) * 180.0f / Pi, now, "hands resting");
            }
            float sx = 0.0f, sz = 0.0f;
            for (const VIZR::HandPose* h : {&left_hand_view, &right_hand_view}) {
                const auto r = rotate(h->position); // view space -> LOCAL, relative to the head
                const float n = std::sqrt(r[0] * r[0] + r[2] * r[2]);
                if (n > 1e-3f) {
                    sx += r[0] / n;
                    sz += r[2] / n;
                }
            }
            if (std::sqrt(sx * sx + sz * sz) > 0.05f) {
                const float rel = WrapPi(std::atan2(-sx, -sz) - head_yaw);
                const float w = std::clamp(user.body_shoulder_weight, 0.0f, 1.0f);
                target = WrapPi(head_yaw + std::clamp(w * rel, -50.0f * Pi / 180.0f, 50.0f * Pi / 180.0f));
            }
        }
        if (!torso_valid) {
            torso_yaw = target;
            torso_ref = target;
            torso_valid = true;
        } else {
            const float tau = std::max(user.body_smoothing_s, 0.01f);
            torso_yaw = WrapPi(torso_yaw + (1.0f - std::exp(-dt / tau)) * WrapPi(target - torso_yaw));
        }
        torso_held = torso_yaw;
        if (torso_rebase) {
            torso_ref = torso_held;
            torso_rebase = false;
        }
        if (!holding) {
            drift_over_ns = 0;
            if (drifting) {
                StopDrift(head_yaw, std::abs(WrapPi(head_yaw - torso_yaw)) * 180.0f / Pi, now, "no hold");
            }
        }
        LogBodyRules(p, head_yaw, dt);
    }

    /// Ends a hold's drift toward the head (UpdateTorso), with its log line.
    void StopDrift(float head_yaw, float gap_deg, s64 now, const char* why) {
        drifting = false;
        drift_over_ns = 0;
        LOG_INFO(Render_Vulkan, "{} body drift: stop, {} (head {:.1f} ours {:.1f} deg, {:.1f} apart, after {:.1f} s)",
                 LogPrefix, why, head_yaw * 57.29578f, torso_yaw * 57.29578f, gap_deg,
                 static_cast<float>(now - drift_since_ns) * 1e-9f);
    }

    /// Every session, once a second (log only, no behaviour): the head yaw, ours (torso_held, the continuous estimate) and
    /// what two other body rules would give for the same poses. Anchor: the head yaw turned by 0.7 x the hands'
    /// direction from a 1.5 s chest anchor (relative to the head), clamped to +/-50 deg, continuous. Follow:
    /// the root keeps within 25 deg of the head (beyond it, it follows), low-passed over 0.3 s, no hold. Degrees, LOCAL.
    void LogBodyRules(const XrPosef& p, float head_yaw, float dt) {
        constexpr float Pi = 3.14159265f;
        if (!shadow_valid) {
            shadow_anchor = {p.position.x, p.position.z};
            anchor_yaw = head_yaw;
            follow_yaw = head_yaw;
            shadow_valid = true;
        }
        const float ka = 1.0f - std::exp(-dt / 1.5f);
        shadow_anchor[0] += ka * (p.position.x - shadow_anchor[0]);
        shadow_anchor[1] += ka * (p.position.z - shadow_anchor[1]);
        const auto& q = p.orientation;
        const auto rotate = [&](const std::array<float, 3>& v) {
            const std::array<float, 3> u{q.x, q.y, q.z};
            const std::array<float, 3> t{2.0f * (u[1] * v[2] - u[2] * v[1]), 2.0f * (u[2] * v[0] - u[0] * v[2]),
                                         2.0f * (u[0] * v[1] - u[1] * v[0])};
            return std::array<float, 3>{v[0] + q.w * t[0] + (u[1] * t[2] - u[2] * t[1]),
                                        v[1] + q.w * t[1] + (u[2] * t[0] - u[0] * t[2]),
                                        v[2] + q.w * t[2] + (u[0] * t[1] - u[1] * t[0])};
        };
        float sx = 0.0f, sz = 0.0f;
        for (const VIZR::HandPose* h : {&left_hand_view, &right_hand_view}) {
            if (h->valid) {
                const auto r = rotate(h->position);
                sx += p.position.x + r[0] - shadow_anchor[0];
                sz += p.position.z + r[2] - shadow_anchor[1];
            }
        }
        if (std::sqrt(sx * sx + sz * sz) > 0.05f) {
            const float rel = WrapPi(std::atan2(-sx, -sz) - head_yaw);
            anchor_yaw = WrapPi(head_yaw + std::clamp(0.7f * rel, -50.0f * Pi / 180.0f, 50.0f * Pi / 180.0f));
        } else {
            anchor_yaw = head_yaw;
        }
        const float gap = WrapPi(head_yaw - follow_yaw);
        const float max_gap = 25.0f * Pi / 180.0f;
        if (std::abs(gap) > max_gap) {
            const float target = WrapPi(head_yaw - (gap > 0.0f ? max_gap : -max_gap));
            follow_yaw = WrapPi(follow_yaw + (1.0f - std::exp(-dt / 0.3f)) * WrapPi(target - follow_yaw));
        }
        const s64 now = VIZR::SteadyNowNs();
        if (now - shadow_logged_ns < 1'000'000'000) {
            return;
        }
        shadow_logged_ns = now;
        // The hands' pointing yaw (each controller's forward, LOCAL) and the raw left stick, for "where is forward".
        const auto aim_yaw = [&](const VIZR::HandPose& h) {
            if (!h.valid) {
                return 0.0f;
            }
            const auto& o = h.orientation; // the controller in view space: its forward (0,0,-1), then the head's rotation
            const std::array<float, 3> f{-2.0f * (o[0] * o[2] + o[3] * o[1]), -2.0f * (o[1] * o[2] - o[3] * o[0]),
                                         -(1.0f - 2.0f * (o[0] * o[0] + o[1] * o[1]))};
            const auto w = rotate(f);
            return std::atan2(-w[0], -w[2]) * 57.29578f;
        };
        LOG_INFO(Render_Vulkan,
                 "{} body rules 1s: head {:.1f} ours {:.1f} anchor {:.1f} follow {:.1f} deg; hands aim L {:.1f} R {:.1f} "
                 "deg, resting {} ({}); stick ({:.2f}, {:.2f})",
                 LogPrefix, head_yaw * 57.29578f, torso_held * 57.29578f, anchor_yaw * 57.29578f, follow_yaw * 57.29578f,
                 aim_yaw(left_hand_view), aim_yaw(right_hand_view), rest_since_ns != 0 ? "yes" : "no",
                 hands_feed ? "feeding the body" : "body held", log_stick[0], log_stick[1]);
    }

    /// Head yaw (rad) about LOCAL +Y: forward (0,0,-1) rotated by yaw = (-sin, 0, -cos).
    static float YawOf(const XrQuaternionf& q) {
        const float fx = -2.0f * (q.x * q.z + q.y * q.w);
        const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        return std::atan2(-fx, -fz);
    }

    /// Take the highest refresh rate the runtime offers up (VD at 120 Hz for mode A)
    /// to 120 Hz, and log the choice (boot line "headset refresh"). Without the extension the
    /// runtime's own rate stays and is logged as such.
    void RequestRefreshRate() {
        if (!rt.has_refresh_rate) {
            LOG_INFO(Render_Vulkan, "{} headset refresh: runtime default (no XR_FB_display_refresh_rate)", LogPrefix);
            return;
        }
        const auto enumerate = GetXrProc<PFN_xrEnumerateDisplayRefreshRatesFB>("xrEnumerateDisplayRefreshRatesFB");
        const auto request = GetXrProc<PFN_xrRequestDisplayRefreshRateFB>("xrRequestDisplayRefreshRateFB");
        const auto current = GetXrProc<PFN_xrGetDisplayRefreshRateFB>("xrGetDisplayRefreshRateFB");
        u32 count = 0;
        if (!enumerate || !request || XR_FAILED(enumerate(session, 0, &count, nullptr)) || count == 0) {
            LOG_WARNING(Render_Vulkan, "{} headset refresh: rates not available", LogPrefix);
            return;
        }
        std::vector<float> rates(count);
        enumerate(session, count, &count, rates.data());
        float best = 0.0f;
        for (const float r : rates) {
            if (r <= 120.5f && r > best) {
                best = r;
            }
        }
        // A fixed game frame rate (Compatibility: Game frame rate 90 / 120) wants that refresh: one game frame per
        // headset frame. "Match headset" keeps the automatic choice and the game follows it.
        if (const float fps = VIZR::g_user_settings.Load().compat_game_fps; fps > 89.0f) {
            for (const float r : rates) {
                if (std::abs(r - fps) < 0.6f) {
                    best = r;
                }
            }
        }
        // The Compatibility tab's choice, where the runtime offers it (else the automatic choice).
        refresh_setting = VIZR::g_user_settings.Load().compat_refresh_hz;
        if (refresh_setting > 0.0f) {
            for (const float r : rates) {
                if (std::abs(r - refresh_setting) < 0.6f) {
                    best = r;
                }
            }
        }
        std::string offered;
        for (const float r : rates) {
            offered += fmt::format("{}{:.0f}", offered.empty() ? "" : "/", r);
        }
        const XrResult result = best > 0.0f ? request(session, best) : XR_ERROR_VALIDATION_FAILURE;
        float now = 0.0f;
        if (current) {
            current(session, &now);
        }
        VIZR::g_display_hz.store(static_cast<u32>(std::lround(now > 0.0f ? now : best)), std::memory_order_relaxed);
        offered_rates = rates;
        LOG_INFO(Render_Vulkan, "{} headset refresh: offered {} Hz, requested {:.0f} ({}), now {:.0f} Hz{}", LogPrefix,
                 offered, best, XR_SUCCEEDED(result) ? std::string{"ok"} : rt.ResultName(result), now,
                 refresh_setting > 0.0f ? fmt::format(" (compat: {:.0f} Hz)", refresh_setting) : std::string{});
    }

    template <typename Fn>
    Fn GetXrProc(const char* name) const {
        PFN_xrVoidFunction fn = nullptr;
        if (XR_FAILED(xrGetInstanceProcAddr(rt.instance, name, &fn))) {
            return nullptr;
        }
        return reinterpret_cast<Fn>(fn);
    }

    /// The runtime's recommended per-eye size, logged once per session.
    void LogViewConfiguration() {
        u32 count = 0;
        if (XR_FAILED(xrEnumerateViewConfigurationViews(rt.instance, rt.system,
                                                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                                        &count, nullptr)) ||
            count == 0) {
            LOG_WARNING(Render_Vulkan, "{} no view configuration views", LogPrefix);
            return;
        }
        std::vector<XrViewConfigurationView> views(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        xrEnumerateViewConfigurationViews(rt.instance, rt.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                          count, &count, views.data());
        recommended_width = views[0].recommendedImageRectWidth;
        recommended_height = views[0].recommendedImageRectHeight;
        max_width = views[0].maxImageRectWidth;
        max_height = views[0].maxImageRectHeight;
        fov_logged = false; // the FOV half of the line follows at the first xrLocateViews
    }

    /// First frame of a session: eye FOVs from xrLocateViews, completing the R6 line.
    void LogEyeFov(XrTime display_time) {
        if (fov_logged) {
            return;
        }
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime = display_time;
        info.space = space;
        XrViewState state{XR_TYPE_VIEW_STATE};
        std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
        u32 count = 0;
        if (XR_FAILED(xrLocateViews(session, &info, &state, 2, &count, views.data())) || count < 2) {
            return; // try again next frame
        }
        fov_logged = true;
        const auto& l = views[0].fov;
        const auto& r = views[1].fov;
        headset_vfov = std::max(l.angleUp - l.angleDown, r.angleUp - r.angleDown);
        constexpr float deg = 57.29578f;
        LOG_INFO(Render_Vulkan,
                 "{} recommended per-eye {}x{} (max {}x{}), eye FOV L/R/U/D deg left {:.1f}/{:.1f}/{:.1f}/{:.1f} "
                 "right {:.1f}/{:.1f}/{:.1f}/{:.1f}, vertical {:.1f}",
                 LogPrefix, recommended_width, recommended_height, max_width, max_height,
                 l.angleLeft * deg, l.angleRight * deg, l.angleUp * deg, l.angleDown * deg,
                 r.angleLeft * deg, r.angleRight * deg, r.angleUp * deg, r.angleDown * deg,
                 headset_vfov * deg);
    }

    /// The game image in both eyes, with the pose and FOV it was rendered with.
    /// The runtime reprojects it to the real head pose at every refresh.
    void BuildProjection(XrCompositionLayerProjection& layer,
                         std::array<XrCompositionLayerProjectionView, 2>& views, const Chain& chain,
                         const VIZR::PoseLedger::Rendered& rendered) const {
        const float aspect = static_cast<float>(chain.width) / static_cast<float>(std::max<u32>(chain.height, 1));
        const float half_v = rendered.render_vfov * 0.5f;
        const float half_h = std::atan(std::tan(half_v) * aspect);
        const auto& p = rendered.pose;
        for (auto& view : views) {
            view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            view.pose.orientation = {p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]};
            view.pose.position = {p.position[0], p.position[1], p.position[2]};
            view.fov = {-half_h, half_h, half_v, -half_v};
            SetSubImage(view.subImage, chain);
        }
        FinishProjection(layer, views);
    }

    /// M5 AFR: one eye's view. The camera write moved the game camera by `side` x IPD along the
    /// head's right axis, so the view pose is the head pose moved the same way; the FOV is the
    /// eye crop's horizontal extent and the game's vertical FOV.
    void BuildEyeView(XrCompositionLayerProjectionView& view, const Chain& chain,
                      const VIZR::PoseLedger::Rendered& rendered, float side) const {
        const auto& p = rendered.pose;
        const float x = p.orientation[0], y = p.orientation[1], z = p.orientation[2], w = p.orientation[3];
        // Head right axis = the rotation's first column.
        const std::array<float, 3> head_right{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)};
        const float offset = side * p.ipd_m;
        view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        view.pose.orientation = {x, y, z, w};
        view.pose.position = {p.position[0] + head_right[0] * offset, p.position[1] + head_right[1] * offset,
                              p.position[2] + head_right[2] * offset};
        const float half_v = rendered.render_vfov * 0.5f;
        view.fov = {std::atan(chain.tan_left), std::atan(chain.tan_right), half_v, -half_v};
        SetSubImage(view.subImage, chain);
    }

    static void SetSubImage(XrSwapchainSubImage& sub, const Chain& chain) {
        sub.swapchain = chain.handle;
        sub.imageRect.offset = {0, 0};
        sub.imageRect.extent = {static_cast<s32>(chain.width), static_cast<s32>(chain.height)};
        sub.imageArrayIndex = 0;
    }

    void FinishProjection(XrCompositionLayerProjection& layer,
                          std::array<XrCompositionLayerProjectionView, 2>& views) const {
        layer.layerFlags = 0;
        layer.space = space;
        layer.viewCount = static_cast<u32>(views.size());
        layer.views = views.data();
    }

    /// The ledger entry of the camera write a chain's image was rendered with (tag - pose_lag).
    std::optional<VIZR::PoseLedger::Rendered> RenderedFor(const Chain& chain) {
        if (chain.tag == 0) {
            return std::nullopt;
        }
        const u64 lag = VIZR::g_hook_state.pose_lag_frames.load(std::memory_order_relaxed);
        auto rendered = VIZR::g_pose_ledger.Find(chain.tag > lag ? chain.tag - lag : 0);
        // The image still on screen keeps the pose it was rendered with, even after a long game stall
        // pushed that entry out of the ledger.
        auto& cache = rendered_cache[static_cast<std::size_t>(&chain - chains.data())];
        if (rendered) {
            cache = {chain.tag, *rendered};
        } else if (cache.first == chain.tag) {
            ++stat_pose_cached;
            return cache.second;
        } else {
            ++stat_missing_pose;
        }
        return rendered;
    }

    /// Headset refreshes we missed, and how many fell inside a game hitch.
    void CountMissedRefreshes(const XrFrameState& state) {
        const XrTime previous = prev_display_time;
        prev_display_time = state.predictedDisplayTime;
        if (previous == 0 || state.predictedDisplayPeriod <= 0) {
            return;
        }
        const XrTime gap = state.predictedDisplayTime - previous;
        if (gap * 2 <= state.predictedDisplayPeriod * 3) {
            return; // within 1.5 periods
        }
        const u64 missed = static_cast<u64>((gap + state.predictedDisplayPeriod / 2) / state.predictedDisplayPeriod) - 1;
        if (missed == 0) {
            return;
        }
        auto& drops = VIZR::g_xr_drops;
        drops.xr_missed.fetch_add(missed, std::memory_order_relaxed);
        stat_xr_missed += missed;
        if (VIZR::GameHitchActive(VIZR::SteadyNowNs())) {
            drops.xr_missed_in_hitch.fetch_add(missed, std::memory_order_relaxed);
            stat_xr_missed_in_hitch += missed;
        }
    }

    // --- M4 Touch input ---------------------------------------------------

    static constexpr std::array<const char*, 11> TouchButtonPaths{
        "/user/hand/left/input/x/click",           "/user/hand/left/input/y/click",
        "/user/hand/left/input/menu/click",        "/user/hand/left/input/thumbstick/click",
        "/user/hand/left/input/trigger/value",     "/user/hand/left/input/squeeze/value",
        "/user/hand/right/input/a/click",          "/user/hand/right/input/b/click",
        "/user/hand/right/input/thumbstick/click", "/user/hand/right/input/trigger/value",
        "/user/hand/right/input/squeeze/value",
    }; // same order as VIZR::TouchInput up to RightGrip

    void CreateInput() {
        input_ok = false;
        try {
            XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
            std::strcpy(set_info.actionSetName, "vizr");
            std::strcpy(set_info.localizedActionSetName, "VIZR");
            Check(xrCreateActionSet(rt.instance, &set_info, &action_set), "xrCreateActionSet");
            std::vector<XrActionSuggestedBinding> bindings;
            const auto make = [&](XrActionType type, const char* name, const char* path, XrAction& out) {
                XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
                info.actionType = type;
                std::strcpy(info.actionName, name);
                std::strcpy(info.localizedActionName, name);
                Check(xrCreateAction(action_set, &info, &out), "xrCreateAction");
                XrPath binding = XR_NULL_PATH;
                Check(xrStringToPath(rt.instance, path, &binding), "xrStringToPath");
                bindings.push_back({out, binding});
            };
            for (std::size_t i = 0; i < TouchButtonPaths.size(); ++i) {
                const bool analog = i == 4 || i == 5 || i == 9 || i == 10; // triggers and grips
                make(analog ? XR_ACTION_TYPE_FLOAT_INPUT : XR_ACTION_TYPE_BOOLEAN_INPUT,
                     fmt::format("touch_{}", i).c_str(), TouchButtonPaths[i], button_actions[i]);
            }
            make(XR_ACTION_TYPE_VECTOR2F_INPUT, "left_stick", "/user/hand/left/input/thumbstick", stick_actions[0]);
            make(XR_ACTION_TYPE_VECTOR2F_INPUT, "right_stick", "/user/hand/right/input/thumbstick", stick_actions[1]);
            // The mod menu's laser (right-hand aim pose) and its open/close pulse.
            make(XR_ACTION_TYPE_POSE_INPUT, "right_aim", "/user/hand/right/input/aim/pose", aim_action);
            make(XR_ACTION_TYPE_VIBRATION_OUTPUT, "right_haptic", "/user/hand/right/output/haptic", haptic_action);
            // The left hand's pose (gestures, arm presence) and its pulse.
            make(XR_ACTION_TYPE_POSE_INPUT, "left_aim", "/user/hand/left/input/aim/pose", left_aim_action);
            make(XR_ACTION_TYPE_VIBRATION_OUTPUT, "left_haptic", "/user/hand/left/output/haptic", left_haptic_action);
            XrPath profile = XR_NULL_PATH;
            Check(xrStringToPath(rt.instance, "/interaction_profiles/oculus/touch_controller", &profile),
                  "xrStringToPath");
            XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggested.interactionProfile = profile;
            suggested.countSuggestedBindings = static_cast<u32>(bindings.size());
            suggested.suggestedBindings = bindings.data();
            Check(xrSuggestInteractionProfileBindings(rt.instance, &suggested),
                  "xrSuggestInteractionProfileBindings");
            LOG_INFO(Render_Vulkan, "{} input: suggested oculus/touch_controller", LogPrefix);
            SuggestOtherControllers();
            XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
            attach.countActionSets = 1;
            attach.actionSets = &action_set;
            Check(xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets");
            XrActionSpaceCreateInfo aim_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            aim_info.action = aim_action;
            aim_info.poseInActionSpace.orientation.w = 1.0f;
            Check(xrCreateActionSpace(session, &aim_info, &aim_space), "xrCreateActionSpace");
            aim_info.action = left_aim_action;
            Check(xrCreateActionSpace(session, &aim_info, &left_aim_space), "xrCreateActionSpace");
            input_ok = true;
        } catch (const XrFatal& e) {
            // No Touch input is not a reason to turn XR off.
            LOG_WARNING(Render_Vulkan, "{} Touch input unavailable: {}", LogPrefix, e.reason);
            DestroyInput();
        }
    }

    /// Every other controller family, bound onto the same actions (the Touch slots: X, Y, menu, stick click,
    /// trigger, grip per hand, A, B; the sticks; aim poses; haptics), so one layout serves every headset. The runtime
    /// picks the profile of the controllers in use; a profile it does not know (or whose extension it lacks) is logged
    /// and skipped. Mapped from the OpenXR spec's input paths; only Touch is tested on a device. The VIZR readme has
    /// the table.
    void SuggestOtherControllers() {
        enum Slot : int { LX, LY, LMenu, LStickClick, LTrigger, LGrip, RA, RB, RStickClick, RTrigger, RGrip, LStick, RStick };
        using Map = std::vector<std::pair<int, const char*>>;
        // Profiles laid out like Touch (face buttons X/Y + A/B, sticks, analog trigger and grip).
        const Map touch_like{
            {LX, "/user/hand/left/input/x/click"},          {LY, "/user/hand/left/input/y/click"},
            {LMenu, "/user/hand/left/input/menu/click"},     {LStickClick, "/user/hand/left/input/thumbstick/click"},
            {LTrigger, "/user/hand/left/input/trigger/value"}, {LGrip, "/user/hand/left/input/squeeze/value"},
            {RA, "/user/hand/right/input/a/click"},         {RB, "/user/hand/right/input/b/click"},
            {RStickClick, "/user/hand/right/input/thumbstick/click"}, {RTrigger, "/user/hand/right/input/trigger/value"},
            {RGrip, "/user/hand/right/input/squeeze/value"}, {LStick, "/user/hand/left/input/thumbstick"},
            {RStick, "/user/hand/right/input/thumbstick"}};
        const std::vector<std::pair<const char*, Map>> profiles{
            {"/interaction_profiles/facebook/touch_controller_pro", touch_like},
            {"/interaction_profiles/htc/vive_focus3_controller", touch_like},
            {"/interaction_profiles/bytedance/pico4_controller", touch_like},
            {"/interaction_profiles/bytedance/pico_neo3_controller", touch_like},
            // Index: A/B on both hands (left A/B = X/Y); the left trackpad pressed hard = menu (the system button is the
            // runtime's).
            {"/interaction_profiles/valve/index_controller",
             {{LX, "/user/hand/left/input/a/click"}, {LY, "/user/hand/left/input/b/click"},
              {LMenu, "/user/hand/left/input/trackpad/force"}, {LStickClick, "/user/hand/left/input/thumbstick/click"},
              {LTrigger, "/user/hand/left/input/trigger/value"}, {LGrip, "/user/hand/left/input/squeeze/value"},
              {RA, "/user/hand/right/input/a/click"}, {RB, "/user/hand/right/input/b/click"},
              {RStickClick, "/user/hand/right/input/thumbstick/click"}, {RTrigger, "/user/hand/right/input/trigger/value"},
              {RGrip, "/user/hand/right/input/squeeze/value"}, {LStick, "/user/hand/left/input/thumbstick"},
              {RStick, "/user/hand/right/input/thumbstick"}}},
            // Windows Mixed Reality: stick + trackpad; trackpad clicks = X (left) / A (right), right menu = B; no Y.
            {"/interaction_profiles/microsoft/motion_controller",
             {{LX, "/user/hand/left/input/trackpad/click"}, {LMenu, "/user/hand/left/input/menu/click"},
              {LStickClick, "/user/hand/left/input/thumbstick/click"}, {LTrigger, "/user/hand/left/input/trigger/value"},
              {LGrip, "/user/hand/left/input/squeeze/click"}, {RA, "/user/hand/right/input/trackpad/click"},
              {RB, "/user/hand/right/input/menu/click"}, {RStickClick, "/user/hand/right/input/thumbstick/click"},
              {RTrigger, "/user/hand/right/input/trigger/value"}, {RGrip, "/user/hand/right/input/squeeze/click"},
              {LStick, "/user/hand/left/input/thumbstick"}, {RStick, "/user/hand/right/input/thumbstick"}}},
            // Vive wands: the trackpads are the sticks (touch = move / turn, click = the stick click), right menu = A;
            // no B, X, Y.
            {"/interaction_profiles/htc/vive_controller",
             {{LMenu, "/user/hand/left/input/menu/click"}, {LStickClick, "/user/hand/left/input/trackpad/click"},
              {LTrigger, "/user/hand/left/input/trigger/value"}, {LGrip, "/user/hand/left/input/squeeze/click"},
              {RA, "/user/hand/right/input/menu/click"}, {RStickClick, "/user/hand/right/input/trackpad/click"},
              {RTrigger, "/user/hand/right/input/trigger/value"}, {RGrip, "/user/hand/right/input/squeeze/click"},
              {LStick, "/user/hand/left/input/trackpad"}, {RStick, "/user/hand/right/input/trackpad"}}},
            // The fallback every runtime knows: select = trigger, left menu = menu, right menu = A; no sticks.
            {"/interaction_profiles/khr/simple_controller",
             {{LTrigger, "/user/hand/left/input/select/click"}, {RTrigger, "/user/hand/right/input/select/click"},
              {LMenu, "/user/hand/left/input/menu/click"}, {RA, "/user/hand/right/input/menu/click"}}},
        };
        const auto action_of = [&](int slot) -> XrAction {
            return slot == LStick ? stick_actions[0] : slot == RStick ? stick_actions[1] : button_actions[slot];
        };
        for (const auto& [name, map] : profiles) {
            std::vector<XrActionSuggestedBinding> list;
            const auto add = [&](XrAction action, const char* path) {
                XrPath binding = XR_NULL_PATH;
                if (XR_SUCCEEDED(xrStringToPath(rt.instance, path, &binding))) {
                    list.push_back({action, binding});
                }
            };
            for (const auto& [slot, path] : map) {
                add(action_of(slot), path);
            }
            add(aim_action, "/user/hand/right/input/aim/pose");
            add(left_aim_action, "/user/hand/left/input/aim/pose");
            add(haptic_action, "/user/hand/right/output/haptic");
            add(left_haptic_action, "/user/hand/left/output/haptic");
            XrPath profile = XR_NULL_PATH;
            XrResult result = xrStringToPath(rt.instance, name, &profile);
            if (XR_SUCCEEDED(result)) {
                XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
                suggested.interactionProfile = profile;
                suggested.countSuggestedBindings = static_cast<u32>(list.size());
                suggested.suggestedBindings = list.data();
                result = xrSuggestInteractionProfileBindings(rt.instance, &suggested);
            }
            if (XR_SUCCEEDED(result)) {
                LOG_INFO(Render_Vulkan, "{} input: suggested {} ({} bindings; active only if those controllers are in use)",
                         LogPrefix, name + 22, list.size());
            } else {
                LOG_INFO(Render_Vulkan, "{} input: {} not supported by this runtime ({})", LogPrefix, name + 22,
                         static_cast<int>(result));
            }
        }
    }

    void DestroyInput() {
        SendPad(VIZR::ProPad{}); // release everything
        if (aim_space != XR_NULL_HANDLE) {
            xrDestroySpace(aim_space);
            aim_space = XR_NULL_HANDLE;
        }
        if (left_aim_space != XR_NULL_HANDLE) {
            xrDestroySpace(left_aim_space);
            left_aim_space = XR_NULL_HANDLE;
        }
        if (action_set != XR_NULL_HANDLE) {
            xrDestroyActionSet(action_set); // destroys its actions
            action_set = XR_NULL_HANDLE;
        }
        // Its actions are gone with it: no later Pulse() or sync may use a dead handle.
        button_actions.fill(XR_NULL_HANDLE);
        stick_actions.fill(XR_NULL_HANDLE);
        aim_action = left_aim_action = XR_NULL_HANDLE;
        haptic_action = left_haptic_action = XR_NULL_HANDLE;
        input_ok = false;
    }

    void SendPad(const VIZR::ProPad& pad) {
        if (pad == last_pad) {
            return;
        }
        // Input debug line: button changes and sticks leaving / returning to centre.
        const auto moving = [](const VIZR::ProPad& p) {
            return std::array<bool, 2>{p.lx != 0.0f || p.ly != 0.0f, p.rx != 0.0f || p.ry != 0.0f};
        };
        if (pad.buttons != last_pad.buttons || moving(pad) != moving(last_pad)) {
            LOG_INFO(Render_Vulkan, "{} pad buttons={:#06x} left=({:.2f},{:.2f}) right=({:.2f},{:.2f})",
                     LogPrefix, pad.buttons, pad.lx, pad.ly, pad.rx, pad.ry);
        }
        last_pad = pad;
        if (auto* sink = VIZR::g_gamepad_sink.load(std::memory_order_acquire)) {
            sink(pad);
        }
    }

    static std::pair<float, float> Deadzone(float x, float y, float deadzone) {
        const float length = std::sqrt(x * x + y * y);
        if (length <= deadzone) {
            return {0.0f, 0.0f};
        }
        const float scale = std::min(1.0f, (length - deadzone) / (1.0f - deadzone)) / length;
        return {x * scale, y * scale};
    }

    /// Reads Touch, applies the profile's layout (map, menu chord, recenter) and sends the Pro
    /// Controller state to Eden's virtual gamepad.
    void SyncInput() {
        using VIZR::ProTarget;
        using VIZR::TouchInput;
        const VIZR::InputLayout layout = VIZR::g_input_layout.Load();
        if (!input_ok || !layout.valid || session_state != XR_SESSION_STATE_FOCUSED) {
            SendPad(VIZR::ProPad{});
            modifier_down = false;
            chord_prev.fill(false);
            latched.fill(ProTarget::None);
            flick_target.fill(ProTarget::None);
            at_head = at_cannon = false; // M7
            menu->HideRing();
            VIZR::g_visor_preview.store(-1, std::memory_order_relaxed);
            return;
        }
        XrActiveActionSet active{action_set, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        if (XR_FAILED(xrSyncActions(session, &sync))) {
            return;
        }
        constexpr std::size_t count = static_cast<std::size_t>(TouchInput::Count);
        std::array<bool, count> pressed{};
        for (std::size_t i = 0; i < TouchButtonPaths.size(); ++i) {
            XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
            get.action = button_actions[i];
            const bool analog = i == 4 || i == 5 || i == 9 || i == 10;
            if (analog) {
                XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
                xrGetActionStateFloat(session, &get, &state);
                pressed[i] = state.isActive && state.currentState > 0.5f;
            } else {
                XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
                xrGetActionStateBoolean(session, &get, &state);
                pressed[i] = state.isActive && state.currentState == XR_TRUE;
            }
        }
        std::array<std::pair<float, float>, 2> sticks{};
        for (std::size_t s = 0; s < 2; ++s) {
            XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
            get.action = stick_actions[s];
            XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
            xrGetActionStateVector2f(session, &get, &state);
            sticks[s] = state.isActive ? Deadzone(state.currentState.x, state.currentState.y, layout.deadzone)
                                       : std::pair<float, float>{0.0f, 0.0f};
        }
        if (SyncMenu(pressed, sticks)) {
            return; // The mod menu has the controllers; the game gets a neutral pad
        }
        // Stick flicks as chord sources.
        constexpr float Flick = 0.7f;
        pressed[static_cast<std::size_t>(TouchInput::RightStickUp)] = sticks[1].second > Flick;
        pressed[static_cast<std::size_t>(TouchInput::RightStickDown)] = sticks[1].second < -Flick;
        pressed[static_cast<std::size_t>(TouchInput::RightStickLeft)] = sticks[1].first < -Flick;
        pressed[static_cast<std::size_t>(TouchInput::RightStickRight)] = sticks[1].first > Flick;
        pressed[static_cast<std::size_t>(TouchInput::LeftStickUp)] = sticks[0].second > Flick;
        pressed[static_cast<std::size_t>(TouchInput::LeftStickDown)] = sticks[0].second < -Flick;
        pressed[static_cast<std::size_t>(TouchInput::LeftStickLeft)] = sticks[0].first < -Flick;
        pressed[static_cast<std::size_t>(TouchInput::LeftStickRight)] = sticks[0].first > Flick;
        // "hold" mode latch: a flick chord stays pressed while its stick stays past Release; a
        // button chord while the button stays down.
        constexpr float Release = 0.5f;
        std::array<bool, count> still = pressed;
        still[static_cast<std::size_t>(TouchInput::RightStickUp)] = sticks[1].second > Release;
        still[static_cast<std::size_t>(TouchInput::RightStickDown)] = sticks[1].second < -Release;
        still[static_cast<std::size_t>(TouchInput::RightStickLeft)] = sticks[1].first < -Release;
        still[static_cast<std::size_t>(TouchInput::RightStickRight)] = sticks[1].first > Release;
        still[static_cast<std::size_t>(TouchInput::LeftStickUp)] = sticks[0].second > Release;
        still[static_cast<std::size_t>(TouchInput::LeftStickDown)] = sticks[0].second < -Release;
        still[static_cast<std::size_t>(TouchInput::LeftStickLeft)] = sticks[0].first < -Release;
        still[static_cast<std::size_t>(TouchInput::LeftStickRight)] = sticks[0].first > Release;

        // Hotfix: in a menu (title, pause, map: the camera hook is silent) [map.menu] wins.
        const bool menu_mode = VIZR::HookSilent();
        SyncGestures(pressed, layout, sticks[0], menu_mode); // Left hand at the head / on the cannon
        const auto target_of = [&](std::size_t i) {
            return menu_mode && layout.menu_override[i] ? layout.map_menu[i] : layout.map[i];
        };
        VIZR::ProPad pad;
        const auto press = [&pad](ProTarget target) {
            if (target != ProTarget::None && target != ProTarget::Recenter) {
                pad.buttons |= 1u << static_cast<u32>(target);
            }
        };
        const bool has_modifier = layout.modifier != TouchInput::Count;
        const bool modifier_held = has_modifier && pressed[static_cast<std::size_t>(layout.modifier)];
        if (modifier_held && !modifier_down) {
            chord_used = false;
        }
        const auto start_tap = [this](ProTarget target) {
            if (target >= ProTarget::A && target <= ProTarget::DDown) {
                tap_frames_left[static_cast<std::size_t>(target)] = TapFrames;
            }
        };
        if (!modifier_held && modifier_down && !chord_used && layout.modifier_tap != ProTarget::None) {
            start_tap(layout.modifier_tap);
        }
        modifier_down = modifier_held;
        // Second chord set (e.g. the beam select on the left grip): hold mode only; while both
        // modifiers are held the first set wins. Its hold_also target stays pressed while held.
        const bool modifier2_held = layout.modifier2 != TouchInput::Count &&
                                    pressed[static_cast<std::size_t>(layout.modifier2)] && !modifier_held;
        if (modifier2_held) {
            press(layout.hold_also2);
        }

        for (std::size_t i = 0; i < count; ++i) {
            const bool edge = pressed[i] && !chord_prev[i];
            chord_prev[i] = pressed[i];
            // A source keeps the target it got on its press edge until it is released, so (hotfix)
            // a button held across a menu <-> gameplay switch never turns into a fresh press of another target.
            if (!pressed[i]) {
                held_valid[i] = false;
            } else if (edge || !held_valid[i]) {
                held_target[i] = target_of(i);
                held_valid[i] = true;
            }
            const bool hold_chord1 = layout.modifier_hold && layout.chord[i] != ProTarget::None;
            const bool hold_chord2 = layout.chord2[i] != ProTarget::None;
            const bool stick_source = i >= static_cast<std::size_t>(TouchInput::RightStickUp) &&
                                      i <= static_cast<std::size_t>(TouchInput::LeftStickRight);
            if ((hold_chord1 || hold_chord2) && stick_source) {
                // A true flick: the chord fires when the stick RETURNS, and
                // only if it was past the flick threshold for at most flick_max_ms. A longer push is
                // a strafe or turn and sends no chord (strafing while locked on no longer pops the
                // visor). Cost: the visor changes on the return, up to flick_max_ms later.
                const s64 now = VIZR::SteadyNowNs();
                if (edge) {
                    flick_target[i] = modifier_held && hold_chord1    ? layout.chord[i]
                                      : modifier2_held && hold_chord2 ? layout.chord2[i]
                                                                      : ProTarget::None;
                    flick_start_ns[i] = now;
                }
                const bool too_long = now - flick_start_ns[i] > s64{layout.flick_max_ms} * 1'000'000;
                if (flick_target[i] != ProTarget::None && (too_long || !pressed[i])) {
                    if (!too_long) {
                        start_tap(flick_target[i]);
                    }
                    flick_target[i] = ProTarget::None;
                }
                continue; // a stick direction has no own button target
            }
            if (hold_chord1 || hold_chord2) {
                // One press per button press made while a modifier is held, kept down (latched)
                // until the button is released. The source's own target is not sent while latched.
                if (latched[i] != ProTarget::None && !still[i]) {
                    latched[i] = ProTarget::None;
                }
                if (latched[i] == ProTarget::None && edge) {
                    if (modifier_held && hold_chord1) {
                        latched[i] = layout.chord[i];
                    } else if (modifier2_held && hold_chord2) {
                        latched[i] = layout.chord2[i];
                    }
                }
                if (latched[i] != ProTarget::None) {
                    press(latched[i]);
                    continue;
                }
            }
            if (!pressed[i]) {
                continue;
            }
            if (modifier_held && layout.chord[i] != ProTarget::None) {
                if (layout.modifier_hold) {
                    // Held past the threshold since before the modifier: no chord, no own target.
                } else {
                    press(layout.chord[i]);
                    chord_used = true;
                }
                continue;
            }
            if (modifier2_held && hold_chord2) {
                continue; // held since before the second modifier: no chord, no own target
            }
            if (held_target[i] == ProTarget::Recenter) {
                if (!recenter_down[i]) {
                    recenter_requested = true; // PublishHead applies it this frame
                }
                continue;
            }
            // [when]: in gameplay the source's own target only while one of its game states holds
            // (Prime: the left trigger sends ZL only in the Scan Visor or at a grapple point: no lock-on).
            if (!menu_mode && layout.when[i] != 0 &&
                (layout.when[i] & VIZR::g_input_states.load(std::memory_order_relaxed)) == 0) {
                continue;
            }
            press(held_target[i]);
        }
        for (std::size_t i = 0; i < count; ++i) {
            recenter_down[i] = pressed[i] && held_valid[i] && held_target[i] == ProTarget::Recenter;
        }
        SyncCrouch(layout, VIZR::g_user_settings.Load(), menu_mode); // Crouch = morph ball
        for (std::size_t t = 0; t < tap_frames_left.size(); ++t) {
            if (tap_frames_left[t] > 0) {
                press(static_cast<ProTarget>(t));
                --tap_frames_left[t];
            }
        }
        // While a hand gesture is held the left stick picks a visor / beam and does not walk.
        const bool gesture_held = at_head || at_cannon;
        // The left stick walks where the head (or the left controller) faces, not the game body's forward
        // The hook runtime's matrix is identity when the body yaw is unknown or stale.
        float lx = sticks[0].first, ly = sticks[0].second;
        log_stick = {lx, ly};
        // In the ball the game turns the stick by its own ball camera's control direction: it goes through unrotated.
        const bool in_ball = layout.crouch_state != 0 &&
                             (VIZR::g_input_states.load(std::memory_order_relaxed) & layout.crouch_state) != 0;
        if (const VIZR::StickXf mx = VIZR::g_move_stick.Load(); mx.sample_ns != 0 && !menu_mode && !in_ball &&
                                                                 VIZR::SteadyNowNs() - mx.sample_ns < 200'000'000) {
            const float x = mx.m[0] * lx + mx.m[1] * ly, y = mx.m[2] * lx + mx.m[3] * ly;
            lx = std::clamp(x, -1.0f, 1.0f);
            ly = std::clamp(y, -1.0f, 1.0f);
        }
        pad.lx = gesture_held ? 0.0f : lx;
        VIZR::g_move_stick_held.store(!gesture_held && !menu_mode && std::max(std::abs(lx), std::abs(ly)) > 0.1f,
                                      std::memory_order_relaxed);
        pad.ly = gesture_held ? 0.0f : ly;
        // "tap" mode: while the chord modifier is held the right stick is the D-pad and does not
        // turn. "hold" mode zeroes no stick.
        const bool zero_right = modifier_held && !layout.modifier_hold;
        const VIZR::UserSettings user = VIZR::g_user_settings.Load();
        if (menu_mode) {
            // No snap turn in a menu: the stick moves the cursor, nothing is queued for gameplay,
            // and the stick must come back to centre after the menu before the next snap.
            VIZR::g_snap_request_deg.store(0.0f, std::memory_order_relaxed);
            snap_active = false;
            snap_armed = false;
        }
        pad.rx = zero_right ? 0.0f
                 : user.snap_turn >= 0.5f && !menu_mode ? SnapTurnRx(sticks[1].first, sticks[1].second, user) // Snap
                                                        : sticks[1].first;
        pad.ry = zero_right ? 0.0f : sticks[1].second;
        SendPad(pad);
        VIZR::g_sent_pad.Store(VIZR::SentPad{{pad.lx, pad.ly, pad.rx, pad.ry}}); // The scan-release log
    }

    /// Hotfix: a short pulse on the right controller when the menu opens or closes. M7: either hand.
    void Pulse() {
        Pulse(haptic_action, 0.6f);
    }
    void Pulse(XrAction action, float amplitude) {
        if (action == XR_NULL_HANDLE || session == XR_NULL_HANDLE || !input_ok) {
            return;
        }
        XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
        vibration.duration = 60'000'000; // 60 ms
        vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
        vibration.amplitude = amplitude;
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = action;
        xrApplyHapticFeedback(session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
    }

    /// The snap's stick edge: +1 / -1 when a push past snap_fire turns, else 0. A push turns only when the stick was
    /// released (below snap_rearm) for snap_rearm_ms first, |x| outweighs |y| (no diagonal), and it is not the
    /// opposite of a snap less than snap_reverse_ms ago (a thumb springing back past centre). One log line per push,
    /// at its release: the peak x, how long it stayed out, the time released before it, and fired or why not.
    float SnapEdge(float x, float y, const VIZR::UserSettings& user, bool can_turn) {
        const s64 now = VIZR::SteadyNowNs();
        const float ax = std::abs(x);
        if (ax < user.snap_rearm) {
            if (snap_below_ns == 0) {
                snap_below_ns = now;
                if (snap_push) {
                    snap_push = false;
                    LOG_INFO(Render_Vulkan,
                             "{} snap stick: {} {}: peak x {:+.2f} (y {:+.2f} at the push), released below {:.2f} after "
                             "{} ms; {} ms released before it, {} ms after the last snap",
                             LogPrefix, snap_push_result, snap_push_dir > 0.0f ? "right" : "left", snap_push_peak,
                             snap_push_y, user.snap_rearm, (now - snap_push_ns) / 1'000'000, snap_push_below_ms,
                             snap_push_since_ms);
                }
            }
            if (!snap_armed && now - snap_below_ns >= static_cast<s64>(user.snap_rearm_ms * 1'000'000.0f)) {
                snap_armed = true;
            }
            return 0.0f;
        }
        if (snap_below_ns != 0) {
            snap_released_ms = (now - snap_below_ns) / 1'000'000;
            snap_below_ns = 0;
        }
        if (snap_push) { // the same push: no second turn until a release
            if (ax > std::abs(snap_push_peak)) {
                snap_push_peak = x;
            }
            return 0.0f;
        }
        if (ax <= user.snap_fire) {
            return 0.0f;
        }
        const float dir = x > 0.0f ? 1.0f : -1.0f;
        snap_push = true;
        snap_push_peak = x;
        snap_push_y = y;
        snap_push_ns = now;
        snap_push_dir = dir;
        snap_push_below_ms = snap_released_ms;
        snap_push_since_ms = snap_last_ns != 0 ? (now - snap_last_ns) / 1'000'000 : -1;
        float fired = 0.0f;
        if (!snap_armed) {
            snap_push_result = "ignored (released too briefly)";
        } else if (std::abs(y) >= ax) {
            snap_push_result = "ignored (diagonal: |y| >= |x|)";
        } else if (snap_last_ns != 0 && dir != snap_last_dir &&
                   now - snap_last_ns < static_cast<s64>(user.snap_reverse_ms * 1'000'000.0f)) {
            snap_push_result = "ignored (opposite of the last snap, too soon)";
        } else if (!can_turn) {
            snap_push_result = "dropped (the game's yaw is stale or a turn is in progress)";
        } else {
            snap_push_result = "fired";
            snap_last_ns = now;
            snap_last_dir = dir;
            fired = dir;
        }
        snap_armed = false;
        return fired;
    }

    /// Snap turn by a closed loop on the game's yaw: a push (SnapEdge) holds the stick fully
    /// over until the game has turned by the angle (at most 1 s, or the yaw stops updating), then
    /// releases.
    float SnapTurnRx(float x, float y, const VIZR::UserSettings& user) {
        const float degrees = user.snap_deg;
        if (VIZR::g_snap_instant.load(std::memory_order_relaxed)) {
            // [snap]: the camera hook turns the body at once; the stick never reaches the game.
            if (const float edge = SnapEdge(x, y, user, true); edge != 0.0f) {
                VIZR::g_snap_request_deg.store(edge * degrees, std::memory_order_relaxed);
            }
            return 0.0f;
        }
        const s64 now = VIZR::SteadyNowNs();
        const float yaw = VIZR::g_game_yaw.load(std::memory_order_relaxed);
        const bool yaw_fresh = now - VIZR::g_game_yaw_ns.load(std::memory_order_relaxed) < 200'000'000;
        const float edge = SnapEdge(x, y, user, yaw_fresh && !snap_active);
        if (snap_active) {
            float moved = yaw - snap_start_yaw;
            while (moved > std::numbers::pi_v<float>) {
                moved -= 2.0f * std::numbers::pi_v<float>;
            }
            while (moved < -std::numbers::pi_v<float>) {
                moved += 2.0f * std::numbers::pi_v<float>;
            }
            const float target = degrees * std::numbers::pi_v<float> / 180.0f;
            if (std::abs(moved) >= target - 0.035f || now - snap_start_ns > 1'000'000'000 || !yaw_fresh) {
                LOG_INFO(Render_Vulkan, "{} snap turn {:+.1f} deg in {} ms", LogPrefix,
                         moved * 57.29578f, (now - snap_start_ns) / 1'000'000);
                snap_active = false;
                return 0.0f;
            }
            return snap_dir;
        }
        if (edge != 0.0f) {
            snap_active = true;
            snap_dir = edge;
            snap_start_yaw = yaw;
            snap_start_ns = now;
            return snap_dir;
        }
        return 0.0f;
    }

    /// The right hand's aim pose relative to the head, for the cannon hook. M7: the left hand too.
    VIZR::HandPose LocateHand(XrSpace hand_space, XrTime display_time) const {
        VIZR::HandPose pose;
        if (hand_space == XR_NULL_HANDLE) {
            return pose;
        }
        XrSpaceLocation hand{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags valid =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (xrLocateSpace(hand_space, view_space, display_time, &hand) == XR_SUCCESS &&
            (hand.locationFlags & valid) == valid) {
            pose.position = {hand.pose.position.x, hand.pose.position.y, hand.pose.position.z};
            pose.orientation = {hand.pose.orientation.x, hand.pose.orientation.y, hand.pose.orientation.z,
                                hand.pose.orientation.w};
            pose.sample_ns = VIZR::SteadyNowNs();
            pose.valid = true;
        }
        return pose;
    }

    void PublishHands(XrTime display_time) {
        right_hand_view = LocateHand(aim_space, display_time);
        left_hand_view = LocateHand(left_aim_space, display_time);
        VIZR::g_right_hand.Store(right_hand_view);
        VIZR::g_left_hand.Store(left_hand_view);
    }

    /// The gesture sources. The left hand at the head (within 0.24 m of the eyes, out past 0.30 m)
    /// and on the right controller (within 0.15 m, out past 0.20 m). The cannon one is a Touch input (the beam chord
    /// modifier). The head one is the visor gesture (headset correction, 2026-10-16):
    /// - hold the hand there: the ring shows (after 0.25 s); a left-stick push past 0.5 lights a choice, nothing
    ///   switches; when the hand LEAVES, the lit choice is pressed (ring_press) and becomes the selected visor
    ///   (user.toml [state], default right = Scan);
    /// - a tap (in and out under 0.4 s, no choice): the next OWNED visor after the one on, in ring order (up, right,
    ///   down, left: Combat, Scan, Thermal, X-Ray), pressed through the game's own visor input (ring_press); without
    ///   ring_state (the layout's visor states) the old toggle: ring_home on -> the selected one, else ring_home. No
    ///   ring. A pulse on entering and on every switch.
    /// No gesture while crouched (the head more than 0.30 m below standing) or in the ball (crouch_state): the head
    /// drops to the hands there and would hold the cannon gesture (swap select, the left stick zeroed). Every pass of
    /// the left hand within 0.35 m of the head logs its closest distance, so a near miss of the 0.20 m zone shows.
    void SyncGestures(std::array<bool, static_cast<std::size_t>(VIZR::TouchInput::Count)>& pressed,
                      const VIZR::InputLayout& layout, const std::pair<float, float>& left_stick, bool menu_mode) {
        const s64 now = VIZR::SteadyNowNs();
        const auto& l = left_hand_view;
        const auto& r = right_hand_view;
        const auto length = [](float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); };
        const u32 states_now = VIZR::g_input_states.load(std::memory_order_relaxed);
        const bool crouch_block = (stand_height_valid && stand_height - head_height > 0.30f) ||
                                  (layout.crouch_state != 0 && (states_now & layout.crouch_state) != 0);
        if (crouch_block && (at_head || at_cannon)) {
            // The posture changed under a held gesture: drop it without its leave action (no tap, no switch).
            at_head = false;
            at_cannon = false;
            head_choice = -1;
            VIZR::g_visor_preview.store(-1, std::memory_order_relaxed);
            menu->HideRing();
            LOG_INFO(Render_Vulkan, "{} gesture: dropped (crouched or in the ball)", LogPrefix);
        }
        const bool head_ok = l.valid && !menu_mode && !crouch_block;
        const float head_d = l.valid ? length(l.position[0], l.position[1], l.position[2]) : 9.0f;
        constexpr float HeadZone = 0.24f; // entry (taps at 0.20-0.22 m missed an older 0.20)
        const bool now_at_head = head_ok && head_d < (at_head ? HeadZone + 0.06f : HeadZone);
        if (head_ok && head_d < 0.35f) {
            if (!near_head) {
                near_head = true;
                near_min_m = head_d;
                near_entered = false;
                near_enter_ns = now;
            }
            near_min_m = std::min(near_min_m, head_d);
            near_entered = near_entered || now_at_head;
        } else if (near_head) {
            near_head = false;
            LOG_INFO(Render_Vulkan, "{} gesture: head pass: closest {:.2f} m in {} ms ({:.2f} m: {})", LogPrefix, near_min_m,
                     (now - near_enter_ns) / 1'000'000, HeadZone, near_entered ? "entered the zone" : "missed the zone");
        }
        const float cannon_d = l.valid && r.valid ? length(l.position[0] - r.position[0], l.position[1] - r.position[1],
                                                           l.position[2] - r.position[2])
                                                  : 9.0f;
        const bool now_at_cannon = !menu_mode && !crouch_block && !now_at_head && cannon_d < (at_cannon ? 0.20f : 0.15f);
        const auto label = [&](int d) { return d >= 0 && d < 4 ? layout.ring_labels[d].data() : "?"; };
        const auto owned = [&](int d) { // Only the visors the player owns
            return d < 0 || d > 3 || layout.ring_owned[d] == 0 || (states_now & layout.ring_owned[d]) != 0;
        };
        const auto press_dir = [&](int d) {
            const auto t = d >= 0 && d < 4 ? static_cast<std::size_t>(layout.ring_press[d]) : tap_frames_left.size();
            if (t < tap_frames_left.size()) {
                tap_frames_left[t] = TapFrames;
            }
            Pulse(left_haptic_action, 0.5f);
        };
        if (now_at_head && !at_head) {
            head_enter_ns = now;
            head_choice = -1;
            head_refused = -1;
            Pulse(left_haptic_action, 0.4f);
            LOG_INFO(Render_Vulkan, "{} gesture: left hand at the head ({:.2f} m)", LogPrefix, head_d);
        } else if (now_at_head) {
            const auto [x, y] = left_stick;
            if (std::max(std::abs(x), std::abs(y)) > 0.5f) {
                const int d = std::abs(y) >= std::abs(x) ? (y > 0.0f ? 0 : 2) : (x > 0.0f ? 1 : 3);
                if (d != head_choice && !owned(d)) {
                    if (d != head_refused) {
                        head_refused = d;
                        LOG_INFO(Render_Vulkan, "{} gesture: {} not owned: no choice", LogPrefix, label(d));
                    }
                } else if (d != head_choice) {
                    head_choice = d;
                    LOG_INFO(Render_Vulkan, "{} gesture: visor choice {} (switches when the hand leaves)", LogPrefix,
                             label(d));
                }
            }
        } else if (at_head) {
            const s64 held_ns = now - head_enter_ns;
            if (head_choice >= 0) {
                press_dir(head_choice);
                const std::scoped_lock lock(VIZR::g_user_settings_write);
                VIZR::UserSettings user = VIZR::g_user_settings.Load();
                if (static_cast<int>(user.selected_visor) != head_choice) {
                    user.selected_visor = static_cast<float>(head_choice);
                    VIZR::g_user_settings.Store(user);
                    VIZR::g_user_settings_changes.fetch_add(1, std::memory_order_relaxed);
                }
                LOG_INFO(Render_Vulkan, "{} gesture: hand left the head: switch to {} (selected visor now)", LogPrefix,
                         label(head_choice));
            } else if (held_ns < 400'000'000) {
                int on = -1; // the visor on now, by the layout's ring_state
                for (int d = 0; d < 4; ++d) {
                    if (layout.ring_state[d] != 0 && (states_now & layout.ring_state[d]) != 0) {
                        on = d;
                        break;
                    }
                }
                const bool has_states = layout.ring_state[0] != 0 || layout.ring_state[1] != 0 ||
                                        layout.ring_state[2] != 0 || layout.ring_state[3] != 0;
                if (has_states) {
                    int to = layout.ring_home;
                    if (on >= 0) {
                        for (int k = 1; k <= 4; ++k) { // the next owned one after the visor on (wraps to itself)
                            const int d = (on + k) % 4;
                            if (layout.ring_press[d] != VIZR::ProTarget::None && owned(d)) {
                                to = d;
                                break;
                            }
                        }
                    }
                    if (to != on) {
                        press_dir(to);
                    }
                    LOG_INFO(Render_Vulkan, "{} gesture: head tap ({} ms): {} on -> {}", LogPrefix, held_ns / 1'000'000,
                             on >= 0 ? label(on) : "unknown", label(to));
                } else {
                    const int selected = std::clamp(static_cast<int>(VIZR::g_user_settings.Load().selected_visor), 0, 3);
                    const bool home_on = layout.home_state != 0 && (states_now & layout.home_state) != 0;
                    const int to = home_on ? selected : layout.ring_home;
                    press_dir(to);
                    LOG_INFO(Render_Vulkan, "{} gesture: head tap ({} ms): {} on -> {}", LogPrefix, held_ns / 1'000'000,
                             home_on ? label(layout.ring_home) : "not home", label(to));
                }
            } else {
                LOG_INFO(Render_Vulkan, "{} gesture: hand left the head, no choice: no change", LogPrefix);
            }
            head_choice = -1;
        }
        if (now_at_cannon && !at_cannon) {
            Pulse(left_haptic_action, 0.4f);
            Pulse(haptic_action, 0.4f);
            LOG_INFO(Render_Vulkan, "{} gesture: left hand on the cannon ({:.2f} m)", LogPrefix, cannon_d);
        }
        at_head = now_at_head;
        at_cannon = now_at_cannon;
        pressed[static_cast<std::size_t>(VIZR::TouchInput::LeftHandHead)] = at_head;
        pressed[static_cast<std::size_t>(VIZR::TouchInput::LeftHandCannon)] = at_cannon;
        // The ring: while the hand has been at the head 0.25 s (a tap shows none), if the layout names labels.
        const bool has_ring = layout.ring_labels[0][0] != '\0' || layout.ring_labels[1][0] != '\0' ||
                              layout.ring_labels[2][0] != '\0' || layout.ring_labels[3][0] != '\0';
        const bool show = !menu_mode && at_head && (now - head_enter_ns >= 250'000'000 || head_choice >= 0);
        // With ring_slots the game's own HUD dial previews the choice (the game's ring shows only what the player
        // owns); the custom ring is only for layouts without them.
        const bool game_dial = layout.ring_slots[0] >= 0 || layout.ring_slots[1] >= 0 || layout.ring_slots[2] >= 0 ||
                               layout.ring_slots[3] >= 0;
        if (game_dial) {
            const int slot = show && head_choice >= 0 ? layout.ring_slots[head_choice] : -1;
            VIZR::g_visor_preview.store(slot, std::memory_order_relaxed);
            menu->HideRing();
        } else if (has_ring && show) {
            auto labels = layout.ring_labels;
            for (int d = 0; d < 4; ++d) {
                if (!owned(d)) {
                    labels[d][0] = '\0'; // not drawn
                }
            }
            menu->ShowRing(last_head_pose, labels, head_choice);
        } else {
            menu->HideRing();
        }
    }

    /// Crouch = morph ball, on posture changes only. Crouching (the head below the standing height by the menu's depth)
    /// taps the layout's crouch_press once, unless the game is already in the ball (crouch_state) or mid-transition
    /// (crouch_block: the request waits up to 1.5 s). Standing back up (the head within 0.15 m of the standing height
    /// for 0.3 s) starts the way out: while the game still reports the ball, a fresh press every 0.3 s for up to 3 s (the
    /// game drops a press while it cannot leave the ball: a projectile, no headroom), never mid-transition.
    /// Every decision is logged with the game's state bits. Standing still never taps, so the button keeps working.
    void SyncCrouch(const VIZR::InputLayout& layout, const VIZR::UserSettings& user, bool menu_mode) {
        if (layout.crouch_press == VIZR::ProTarget::None || user.crouch_morph < 0.5f || !stand_height_valid ||
            menu_mode) {
            crouch_request = 0;
            unmorph_until_ns = 0;
            return;
        }
        const s64 now = VIZR::SteadyNowNs();
        const float drop = stand_height - head_height;
        const u32 states = VIZR::g_input_states.load(std::memory_order_relaxed);
        const bool in_ball = layout.crouch_state != 0 && (states & layout.crouch_state) != 0;
        const bool moving = (states & layout.crouch_block) != 0;
        const auto tap = [&] {
            const auto t = static_cast<std::size_t>(layout.crouch_press);
            if (t < tap_frames_left.size()) {
                tap_frames_left[t] = TapFrames;
            }
        };
        if (!crouched && drop > user.crouch_drop_m) {
            crouched = true;
            stand_since_ns = 0;
            unmorph_until_ns = 0;
            crouch_request = 1;
            crouch_request_ns = now;
        } else if (crouched) {
            if (drop < 0.15f) {
                if (stand_since_ns == 0) {
                    stand_since_ns = now;
                } else if (now - stand_since_ns >= 300'000'000) {
                    crouched = false;
                    crouch_request = 0;
                    unmorph_until_ns = now + 3'000'000'000;
                    unmorph_next_ns = now;
                    unmorph_taps = 0;
                    LOG_INFO(Render_Vulkan, "{} crouch: standing (head {:.2f} m below standing; in ball {}, moving {})",
                             LogPrefix, drop, in_ball, moving);
                }
            } else {
                stand_since_ns = 0;
            }
        }
        if (unmorph_until_ns != 0) {
            if (!in_ball && !moving) {
                LOG_INFO(Render_Vulkan, "{} crouch: out of the ball ({} press(es))", LogPrefix, unmorph_taps);
                unmorph_until_ns = 0;
            } else if (now > unmorph_until_ns) {
                LOG_INFO(Render_Vulkan, "{} crouch: still in the ball after 3 s ({} press(es); in ball {}, moving {})",
                         LogPrefix, unmorph_taps, in_ball, moving);
                unmorph_until_ns = 0;
            } else if (in_ball && !moving && now >= unmorph_next_ns) {
                tap();
                ++unmorph_taps;
                unmorph_next_ns = now + 300'000'000;
                LOG_INFO(Render_Vulkan, "{} crouch: up press {} (in ball)", LogPrefix, unmorph_taps);
            }
            return;
        }
        if (crouch_request <= 0) {
            return;
        }
        if (now - crouch_request_ns > 1'500'000'000) {
            LOG_INFO(Render_Vulkan, "{} crouch: down request dropped (blocked 1.5 s)", LogPrefix);
            crouch_request = 0;
            return;
        }
        if (moving) {
            return; // mid-transition (a press then is lost or reverses it)
        }
        crouch_request = 0;
        if (in_ball) {
            LOG_INFO(Render_Vulkan, "{} crouch: down (already in the ball, no press)", LogPrefix);
            return;
        }
        tap();
        LOG_INFO(Render_Vulkan, "{} crouch: head {:.2f} m below standing -> down (press)", LogPrefix, drop);
    }

    /// Both stick clicks within 150 ms (or F1) open / close the mod menu; B closes it.
    /// While it is open the right-hand laser points, the right trigger clicks, the right stick scrolls
    /// one row per push, and the game gets a neutral pad. Returns true while the menu has the input.
    bool SyncMenu(const std::array<bool, static_cast<std::size_t>(VIZR::TouchInput::Count)>& pressed,
                  const std::array<std::pair<float, float>, 2>& sticks) {
        constexpr std::size_t LeftClick = 3, RightB = 7, RightClick = 8, RightTrigger = 9;
        const s64 now = VIZR::SteadyNowNs();
        // Hotfix: both clicks
        // need their own press edges within 150 ms of each other, then both held together for 150 ms more.
        // A single click, or one stick held while the other clicks, never toggles.
        constexpr s64 Pair = 150'000'000;
        constexpr s64 Hold = 150'000'000;
        bool toggle = false;
        std::string reason;
        if (VIZR::g_menu_toggle_request.exchange(false)) {
            toggle = true;
            reason = "F1";
        }
        const bool left_edge = pressed[LeftClick] && !menu_prev_left;
        const bool right_edge = pressed[RightClick] && !menu_prev_right;
        menu_prev_left = pressed[LeftClick];
        menu_prev_right = pressed[RightClick];
        if (left_edge) {
            left_click_ns = now;
        }
        if (right_edge) {
            right_click_ns = now;
        }
        if ((left_edge || right_edge) && pressed[LeftClick] && pressed[RightClick] &&
            std::abs(left_click_ns - right_click_ns) <= Pair) {
            pair_since_ns = now; // both edges close together: start the hold
        }
        if (!(pressed[LeftClick] && pressed[RightClick])) {
            pair_since_ns = 0;
        } else if (pair_since_ns != 0 && now - pair_since_ns >= Hold) {
            toggle = true;
            reason = fmt::format("both stick clicks (edges {} ms apart, held {} ms)",
                                 std::abs(left_click_ns - right_click_ns) / 1'000'000, (now - pair_since_ns) / 1'000'000);
            pair_since_ns = 0;
        }
        const bool b_edge = pressed[RightB] && !menu_prev_b;
        menu_prev_b = pressed[RightB];
        const bool was_open = menu->IsOpen();
        if (toggle) {
            menu->Toggle(last_head_pose);
            menu_frames_unseen = 0;
            Pulse();
            LOG_INFO(Render_Vulkan, "{} mod menu {} by {}", LogPrefix, menu->IsOpen() ? "opened" : "closed", reason);
        } else if (was_open && b_edge) {
            menu->Close();
            Pulse();
            LOG_INFO(Render_Vulkan, "{} mod menu closed by B", LogPrefix);
        }
        // Hotfix: the menu has the controllers only while its panel was really on screen last frame. Half
        // a second without a submitted panel closes it (it can never be open and invisible).
        if (menu->IsOpen() && !menu_layer_last_frame && ++menu_frames_unseen > 45) {
            menu->Close();
            LOG_WARNING(Render_Vulkan, "{} mod menu closed: its panel was not shown for 0.5 s", LogPrefix);
        }
        VIZR::g_menu_open = menu->IsOpen();
        if (was_open && !menu->IsOpen()) {
            menu_release_wait = true; // the closing clicks / B never reach the game
        }
        if (menu->IsOpen() && !menu_layer_last_frame) {
            return false; // not on screen (yet): the game keeps the controllers
        }
        if (!menu->IsOpen()) {
            if (menu_release_wait) {
                if (pressed[LeftClick] || pressed[RightClick] || pressed[RightB]) {
                    SendPad(VIZR::ProPad{});
                    return true;
                }
                menu_release_wait = false;
            }
            return false;
        }
        menu_input = {};
        if (aim_space != XR_NULL_HANDLE && menu_display_time != 0) {
            XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
            constexpr XrSpaceLocationFlags valid =
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if (xrLocateSpace(aim_space, space, menu_display_time, &aim) == XR_SUCCESS &&
                (aim.locationFlags & valid) == valid) {
                menu_input.pointer = menu->Hit(aim.pose);
            }
        }
        menu_input.click = pressed[RightTrigger];
        // One row per push: the stick must come back to centre before the next row.
        const float y = sticks[1].second;
        if (std::abs(y) > 0.7f && !menu_scroll_held) {
            menu_input.scroll = y > 0.0f ? 1.0f : -1.0f;
            menu_scroll_held = true;
        } else if (std::abs(y) < 0.3f) {
            menu_scroll_held = false;
        }
        SendPad(VIZR::ProPad{});
        return true;
    }

    /// A quad. From M4 the switch here builds a projection layer (same image in both eyes,
    /// pose from the ledger for the slot's tag, FOV from the profile); a quad cannot carry the
    /// rendered pose, so it would double-rotate once the head drives the camera.
    void BuildLayer(XrCompositionLayerQuad& quad, const Chain& chain) const {
        quad.layerFlags = 0;
        quad.space = space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        SetSubImage(quad.subImage, chain);
        quad.pose.orientation.w = 1.0f;
        quad.pose.position = {0.0f, 0.0f, -QuadDistance};
        quad.size = {QuadWidth, QuadWidth * static_cast<float>(chain.height) /
                                    static_cast<float>(std::max<u32>(chain.width, 1))};
    }

    /// The newest game frame decides mono or stereo; stereo refreshes each eye's chain from
    /// that eye's newest frame (an eye comes from the frame's pose ledger entry, never parity).
    void CopyLatest() {
        {
            std::scoped_lock lk{slot_mutex};
            show_stereo = latest >= 0 && slots[latest].eye != VIZR::Eye::Both;
        }
        if (show_stereo && VIZR::g_stereo_mode.load(std::memory_order_relaxed) == VIZR::StereoMode::ModeC) {
            CopyPair();
        } else if (show_stereo) {
            CopyInto(static_cast<std::size_t>(VIZR::Eye::Left));
            CopyInto(static_cast<std::size_t>(VIZR::Eye::Right));
        } else {
            CopyInto(static_cast<std::size_t>(VIZR::Eye::Both));
        }
    }

    /// Mode C copies the two eyes of ONE game
    /// frame or neither. Copied one by one, the left eye could land while the right eye's GPU work was still
    /// running (not_ready), so the two swapchains held different frames and the frame fell back to the flat quad.
    /// Copying nothing keeps the last released pair on screen (the runtime reprojects it).
    void CopyPair() {
        if (copy_pending) {
            if (!CopyFenceDone(SwapchainWaitTimeoutNs)) {
                ++stat_copy_busy;
                return;
            }
            copy_pending = false;
        }
        const std::size_t cl = static_cast<std::size_t>(VIZR::Eye::Left);
        const std::size_t cr = static_cast<std::size_t>(VIZR::Eye::Right);
        int l = -1, r = -1;
        {
            // The newest completed pair whose GPU work is done; else the one before it (a frame older: added latency,
            // never a freeze); else nothing new this headset frame. The newest alone froze the headset when the game's
            // frames took longer than a headset frame to finish (seen at 90 Hz: 6-34 fresh copies a second).
            auto& master = scheduler.GetMasterSemaphore();
            master.Refresh();
            std::scoped_lock lk{slot_mutex};
            held = held_pair = -1;
            const auto newer = [&](const PairRec& p) {
                return p.l >= 0 && p.r >= 0 && (p.seq_l > chains[cl].seq || p.seq_r > chains[cr].seq) &&
                       slots[p.l].seq == p.seq_l && slots[p.r].seq == p.seq_r;
            };
            const PairRec* pick = nullptr;
            if (newer(pairs[0]) && master.IsFree(pairs[0].tick)) {
                pick = &pairs[0];
            } else if (newer(pairs[1]) && master.IsFree(pairs[1].tick)) {
                pick = &pairs[1];
                ++stat_fallback;
            } else if (newer(pairs[0])) {
                ++stat_not_ready;
                return; // neither is finished: try again next headset frame
            } else {
                ++stat_no_new_frame;
                return;
            }
            l = pick->l;
            r = pick->r;
            held_pair = l;
            held = r;
            if (copy_latency_ms.size() < 256) {
                copy_latency_ms.push_back(static_cast<float>(VIZR::SteadyNowNs() - pick->published_ns) * 1e-6f);
            }
        }
        CopyInto(cl, l);
        CopyInto(cr, r, PairCopyWaitNs); // waits for the left copy (one command buffer); a short blit
        // Both slots stay held until the next CopyPair has waited for the copy in flight (the right one, or the
        // left one if the right copy found it still busy).
    }

    /// The vignette texture for strength `level` (1..VignetteLevels) in its own XR swapchain: alpha 0 inside a
    /// radius that shrinks as the strength rises, a soft edge, then black to the corners. Uploaded through the copy
    /// command buffer only when the level changes. False = no vignette (no usable format or memory: logged once).
    bool PrepareVignette(int level) {
        if (vignette_failed) {
            return false;
        }
        if (vignette.handle == XR_NULL_HANDLE && !CreateVignette()) {
            vignette_failed = true;
            return false;
        }
        if (level == vignette_level) {
            return true;
        }
        if (copy_pending) {
            if (!CopyFenceDone(SwapchainWaitTimeoutNs)) {
                return vignette_level > 0; // the old level stays on screen this frame
            }
            copy_pending = false;
        }
        const float strength = static_cast<float>(level) / VignetteLevels;
        const float inner = 0.50f - 0.32f * strength; // clear radius (fraction of the half side)
        const float edge = 0.16f;
        auto texels = vignette_staging.Mapped();
        for (u32 y = 0; y < VignetteSize; ++y) {
            for (u32 x = 0; x < VignetteSize; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) / VignetteSize * 2.0f - 1.0f;
                const float dy = (static_cast<float>(y) + 0.5f) / VignetteSize * 2.0f - 1.0f;
                const float r = std::sqrt(dx * dx + dy * dy);
                const float t = std::clamp((r - inner) / edge, 0.0f, 1.0f);
                const float a = t * t * (3.0f - 2.0f * t); // smoothstep
                u8* px = texels.data() + (static_cast<std::size_t>(y) * VignetteSize + x) * 4;
                px[0] = px[1] = px[2] = 0;
                px[3] = static_cast<u8>(std::lround(a * 255.0f));
            }
        }
        u32 image_index = 0;
        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        Check(Locked(CallAcquire, [&] { return xrAcquireSwapchainImage(vignette.handle, &acquire_info, &image_index); }),
              "xrAcquireSwapchainImage");
        if (session_lost) {
            return false; // Check() records a lost session and returns: there is no image to fill
        }
        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = XR_INFINITE_DURATION;
        Check(xrWaitSwapchainImage(vignette.handle, &wait_info), "xrWaitSwapchainImage");
        const VkImage image = vignette.images[image_index].image;
        constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        command_buffer.Begin({
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        });
        command_buffer.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                       VkImageMemoryBarrier{
                                           .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                           .pNext = nullptr,
                                           .srcAccessMask = 0,
                                           .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                           .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                           .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .image = image,
                                           .subresourceRange = range,
                                       });
        const VkBufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {VignetteSize, VignetteSize, 1},
        };
        command_buffer.CopyBufferToImage(*vignette_staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
        // XR_KHR_vulkan_enable: colour swapchain images are released in COLOR_ATTACHMENT_OPTIMAL.
        command_buffer.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                       VkImageMemoryBarrier{
                                           .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                           .pNext = nullptr,
                                           .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                           .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                                           .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .image = image,
                                           .subresourceRange = range,
                                       });
        command_buffer.End();
        copy_fence.Reset();
        const VkCommandBuffer cmdbuf = *command_buffer;
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmdbuf,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr,
        };
        const VkResult submitted = [&] {
            std::scoped_lock lk{QueueMutex()};
            return (own_queue ? device.GetXrQueue() : device.GetGraphicsQueue()).Submit(submit, *copy_fence);
        }();
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        Check(Locked(CallRelease, [&] { return xrReleaseSwapchainImage(vignette.handle, &release_info); }),
              "xrReleaseSwapchainImage");
        if (submitted != VK_SUCCESS) {
            throw XrFatal(fmt::format("vignette submit failed: {}", static_cast<int>(submitted)));
        }
        copy_pending = true;
        vignette_level = level;
        return true;
    }

    bool CreateVignette() {
        u32 count = 0;
        xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
        std::vector<s64> formats(count);
        xrEnumerateSwapchainFormats(session, count, &count, formats.data());
        VkFormat chosen = VK_FORMAT_UNDEFINED;
        for (const VkFormat wanted : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
                                      VK_FORMAT_B8G8R8A8_SRGB}) {
            if (std::find(formats.begin(), formats.end(), static_cast<s64>(wanted)) != formats.end()) {
                chosen = wanted; // black texels: the channel order does not matter, the alpha is linear either way
                break;
            }
        }
        if (chosen == VK_FORMAT_UNDEFINED) {
            LOG_WARNING(Render_Vulkan, "{} vignette: no RGBA8 swapchain format; comfort vignette off", LogPrefix);
            return false;
        }
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = chosen;
        info.sampleCount = 1;
        info.width = VignetteSize;
        info.height = VignetteSize;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(session, &info, &vignette.handle))) {
            vignette.handle = XR_NULL_HANDLE;
            LOG_WARNING(Render_Vulkan, "{} vignette: xrCreateSwapchain failed; comfort vignette off", LogPrefix);
            return false;
        }
        u32 image_count = 0;
        xrEnumerateSwapchainImages(vignette.handle, 0, &image_count, nullptr);
        vignette.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(vignette.handle, image_count, &image_count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(vignette.images.data()));
        vignette.width = vignette.height = VignetteSize;
        const VkBufferCreateInfo buffer_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = static_cast<VkDeviceSize>(VignetteSize) * VignetteSize * 4,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        vignette_staging = allocator.CreateBuffer(buffer_info, MemoryUsage::Upload);
        if (!vignette_staging.IsHostVisible() || !vignette_staging.IsHostCoherent()) {
            LOG_WARNING(Render_Vulkan, "{} vignette: no host-coherent upload memory; comfort vignette off", LogPrefix);
            return false;
        }
        LOG_INFO(Render_Vulkan, "{} vignette: swapchain {}x{} format {}", LogPrefix, VignetteSize, VignetteSize,
                 static_cast<int>(chosen));
        return true;
    }

    /// The vignette quad: head-locked (VIEW space), 1 m ahead, alpha-blended over the game.
    void BuildVignette(XrCompositionLayerQuad& quad) const {
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                          XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        quad.space = view_space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        SetSubImage(quad.subImage, vignette);
        quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        quad.pose.position = {0.0f, 0.0f, -1.0f};
        quad.size = {VignetteQuadM, VignetteQuadM};
    }

    /// `forced` (CopyPair): copy that slot; the caller chose it, holds it and checked its GPU work is done.
    void CopyInto(std::size_t c, int forced = -1, u64 fence_wait_ns = SwapchainWaitTimeoutNs) {
        Chain& chain = chains[c];
        // At most one copy is in flight: the previous copy must finish before its slot is released.
        if (copy_pending) {
            if (!CopyFenceDone(fence_wait_ns)) {
                ++stat_copy_busy;
                return; // keep holding it; the layer keeps showing the last released image
            }
            copy_pending = false;
        }
        int take = -1;
        Slot slot_copy;
        {
            std::scoped_lock lk{slot_mutex};
            if (forced < 0) {
                held = held_pair = -1; // the copy in flight (if any) has finished: release both
            }
            const int newest = forced >= 0 ? forced : latest_by_eye[c];
            if (newest >= 0 && slots[newest].seq != chain.seq) {
                take = newest;
                if (forced < 0) {
                    held = take;
                }
                slot_copy.tick = slots[take].tick;
                slot_copy.seq = slots[take].seq;
                slot_copy.tag = slots[take].tag;
                slot_copy.width = slots[take].width;
                slot_copy.height = slots[take].height;
                slot_copy.image_format = slots[take].image_format;
                slot_copy.tan_left = slots[take].tan_left;
                slot_copy.tan_right = slots[take].tan_right;
            }
        }
        if (take < 0) {
            ++stat_no_new_frame;
            return;
        }
        // Wait on the CPU, not on the GPU: Flush() only handed the blit to Eden's worker
        // thread, and a GPU wait queued before Eden's own submit on this queue could deadlock.
        auto& master = scheduler.GetMasterSemaphore();
        master.Refresh();
        if (forced < 0 && !master.IsFree(slot_copy.tick)) {
            std::scoped_lock lk{slot_mutex};
            held = -1;
            ++stat_not_ready;
            return; // try again next headset frame
        }

        EnsureXrSwapchain(c, slot_copy.width, slot_copy.height, slot_copy.image_format);

        u32 image_index = 0;
        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        Check(Locked(CallAcquire,
                     [&] { return xrAcquireSwapchainImage(chain.handle, &acquire_info, &image_index); }),
              "xrAcquireSwapchainImage");
        if (stopping || leaked || session_lost) {
            return; // Stop() may have detached us, or the session is lost (no image acquired): touch no Vulkan object
        }
        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        // XR_TIMEOUT_EXPIRED counts as success but leaves the image unusable, so wait without
        // a timeout; a hung runtime is covered by Stop()'s bounded join.
        wait_info.timeout = XR_INFINITE_DURATION;
        Check(xrWaitSwapchainImage(chain.handle, &wait_info), "xrWaitSwapchainImage");
        if (stopping || leaked || session_lost) {
            return; // Stop() may have detached us, or the session is lost; touch no Vulkan object
        }

        RecordCopy(slots[take].frame, chain.images[image_index].image, slot_copy.width,
                   slot_copy.height);
        copy_fence.Reset();
        const VkCommandBuffer cmdbuf = *command_buffer;
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmdbuf,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr,
        };
        const VkResult submitted = [&] {
            std::scoped_lock lk{QueueMutex()};
            const auto start = std::chrono::steady_clock::now();
            const VkResult result =
                (own_queue ? device.GetXrQueue() : device.GetGraphicsQueue()).Submit(submit, *copy_fence);
            max_hold_us[CallSubmit] = std::max<s64>(
                max_hold_us[CallSubmit], std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count());
            return result;
        }();
        // Release even if the submit failed, so the swapchain is not left acquired.
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        Check(Locked(CallRelease,
                     [&] { return xrReleaseSwapchainImage(chain.handle, &release_info); }),
              "xrReleaseSwapchainImage");
        if (submitted != VK_SUCCESS) {
            throw XrFatal(fmt::format("copy submit failed: {}", static_cast<int>(submitted)));
        }
        copy_pending = true;
        chain.seq = slot_copy.seq;
        chain.tag = slot_copy.tag; // The pose id the image in the XR swapchain was rendered with
        chain.tan_left = slot_copy.tan_left;
        chain.tan_right = slot_copy.tan_right;
        chain.has_image = true;
        ++stat_copies;
    }

    void RecordCopy(const Frame& ring, VkImage xr_image, u32 width, u32 height) {
        command_buffer.Begin({
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        });
        constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const std::array<VkImageMemoryBarrier, 2> before{{
            {
                // Ring slot: written by Eden's blit (render pass leaves it GENERAL). Its tick is
                // already complete on the GPU; this makes the writes visible to our copy.
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = *ring.image,
                .subresourceRange = range,
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = xr_image,
                .subresourceRange = range,
            },
        }};
        command_buffer.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);
        const VkImageCopy region{
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffset = {0, 0, 0},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffset = {0, 0, 0},
            .extent = {width, height, 1},
        };
        // Raw bits: no colour conversion between the UNORM ring and an sRGB XR image.
        command_buffer.CopyImage(*ring.image, VK_IMAGE_LAYOUT_GENERAL, xr_image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
        // XR_KHR_vulkan_enable: colour swapchain images are released in COLOR_ATTACHMENT_OPTIMAL.
        command_buffer.PipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = xr_image,
                .subresourceRange = range,
            });
        command_buffer.End();
    }

    void EnsureXrSwapchain(std::size_t c, u32 width, u32 height, VkFormat ring_format) {
        Chain& chain = chains[c];
        if (chain.handle != XR_NULL_HANDLE && width == chain.width && height == chain.height &&
            ring_format == chain.ring_format) {
            return;
        }
        DestroyChain(chain);

        u32 count = 0;
        Check(xrEnumerateSwapchainFormats(session, 0, &count, nullptr),
              "xrEnumerateSwapchainFormats");
        std::vector<s64> formats(count);
        Check(xrEnumerateSwapchainFormats(session, count, &count, formats.data()),
              "xrEnumerateSwapchainFormats");
        VkFormat chosen = VK_FORMAT_UNDEFINED;
        for (const VkFormat wanted : PreferredXrFormats(ring_format)) {
            if (wanted != VK_FORMAT_UNDEFINED &&
                std::find(formats.begin(), formats.end(), static_cast<s64>(wanted)) !=
                    formats.end()) {
                chosen = wanted;
                break;
            }
        }
        if (chosen == VK_FORMAT_UNDEFINED) {
            throw XrFatal(fmt::format("no XR swapchain format matches Eden's ring format {}",
                                      static_cast<int>(ring_format)));
        }

        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = chosen;
        info.sampleCount = 1;
        info.width = width;
        info.height = height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        Check(xrCreateSwapchain(session, &info, &chain.handle), "xrCreateSwapchain");
        u32 image_count = 0;
        Check(xrEnumerateSwapchainImages(chain.handle, 0, &image_count, nullptr),
              "xrEnumerateSwapchainImages");
        chain.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        Check(xrEnumerateSwapchainImages(
                  chain.handle, image_count, &image_count,
                  reinterpret_cast<XrSwapchainImageBaseHeader*>(chain.images.data())),
              "xrEnumerateSwapchainImages");
        chain.width = width;
        chain.height = height;
        chain.ring_format = ring_format;
        chain.has_image = false;
        chain.seq = 0;
        chain.tag = 0;
        // M5 G-FPS: the eye chains' size is the runtime's recommended per-eye size.
        static constexpr std::array<const char*, 3> ChainNames{"left eye", "right eye", "mono"};
        LOG_INFO(Render_Vulkan,
                 "{} swapchain {} {}x{} (recommended per-eye {}x{}, resolution scale {:.2f}) images={} "
                 "formats: desktop view {}, ring {}, XR {}",
                 LogPrefix, ChainNames[c], width, height, recommended_width, recommended_height,
                 Settings::values.resolution_info.up_factor, image_count,
                 static_cast<int>(swapchain.GetImageViewFormat()), static_cast<int>(ring_format),
                 static_cast<int>(chosen));
    }

    /// "Match headset" with the profile's cap (g_tick_match_max_hz): once the cap is known, a refresh above it is lowered
    /// to the cap (an offered rate), so the game's tick and the headset's refresh stay equal. Checked once a second.
    void ApplyRefreshCap() {
        const u32 cap = VIZR::g_tick_match_max_hz.load(std::memory_order_relaxed);
        const u32 hz = VIZR::g_display_hz.load(std::memory_order_relaxed);
        if (cap == 0 || hz <= cap || cap_requested || VIZR::g_user_settings.Load().compat_game_fps > 0.5f ||
            !rt.has_refresh_rate) {
            return;
        }
        cap_requested = true;
        float pick = 0.0f;
        for (const float r : offered_rates) {
            if (r <= static_cast<float>(cap) + 0.5f && r > pick) {
                pick = r;
            }
        }
        const auto request = GetXrProc<PFN_xrRequestDisplayRefreshRateFB>("xrRequestDisplayRefreshRateFB");
        if (pick <= 0.0f || !request) {
            return;
        }
        const XrResult result = request(session, pick);
        if (XR_SUCCEEDED(result)) {
            VIZR::g_display_hz.store(static_cast<u32>(std::lround(pick)), std::memory_order_relaxed);
        }
        LOG_INFO(Render_Vulkan, "{} headset refresh: requested {:.0f} Hz ({}): Match headset is capped at {} Hz", LogPrefix,
                 pick, XR_SUCCEEDED(result) ? std::string{"ok"} : rt.ResultName(result), cap);
    }

    void LogStatsEverySecond() {
        ApplyRefreshCap();
        const auto now = std::chrono::steady_clock::now();
        if (now - last_stats < std::chrono::seconds{1}) {
            return;
        }
        last_stats = now;
        {
            // The hands relative to the head (gesture distances), once a second.
            const auto& l = left_hand_view;
            const auto& r = right_hand_view;
            LOG_INFO(Render_Vulkan,
                     "{} 1s hands: left {} ({:.2f},{:.2f},{:.2f}) right {} ({:.2f},{:.2f},{:.2f}) at_head={} at_cannon={} "
                     "head_drop={:.2f}",
                     LogPrefix, l.valid ? "ok" : "lost", l.position[0], l.position[1], l.position[2],
                     r.valid ? "ok" : "lost", r.position[0], r.position[1], r.position[2], at_head, at_cannon,
                     stand_height - head_height);
        }
        {
            // Every session, once a second: the game's frame times (publish to publish): median, 99th percentile,
            // longest, and the frames longer than twice the median (hitches).
            std::vector<float> ms;
            {
                std::scoped_lock lock{frame_ms_mutex};
                ms.swap(frame_ms);
            }
            if (!ms.empty()) {
                std::sort(ms.begin(), ms.end());
                const float p50 = ms[ms.size() / 2];
                const float p99 = ms[std::min(ms.size() - 1, ms.size() * 99 / 100)];
                const auto long_frames = std::count_if(ms.begin(), ms.end(), [&](float v) { return v > 2.0f * p50; });
                LOG_INFO(Render_Vulkan, "{} 1s game frames: {} at p50 {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms; over 2x p50: {}",
                         LogPrefix, ms.size(), p50, p99, ms.back(), long_frames);
            }
        }
        {
            // Every session, once a second: how fresh the headset's frames are. The time from a pair's publish to its
            // copy (median, longest), and the headset frames that found the newest pair unfinished (an older one shown,
            // or none). Watchdog: above a 60 game rate, a second falls behind when the game publishes eye pairs (at least
            // half its rate: not a loading screen) and it shows a freeze signature: none ready in over 30% of the headset
            // frames (the headset repeats a frame; seen at 90 Hz: 50-87 a second), copies below half the lower of
            // the game rate and the headset refresh (a collapse; seen at 90 Hz: 6-34 copies a second), or the median publish
            // to copy time above two headset frames (a growing backlog). Left alone: a steady one-frame fallback at full
            // rate (the rig's slower 2x run: 89 pairs, 12 ms) and a pacing beat (the rig's 2x: 70 of 91 pairs copied
            // for ~20 s, none ready under 9, 10.4 ms: frames unshown when two land in one headset frame). Armed
            // 10 s after fresh frames began, counted lagging or not (a load hitch as gameplay starts is not a verdict:
            // the rig's 2x strafe at 90 fell behind for 2 s there, then ran at 90; a session that lags from the start
            // still arms); after 5 such seconds in a row the game rate is held at 60 for the session. Meanwhile the
            // older pairs keep the view moving.
            std::vector<float> lat;
            lat.swap(copy_latency_ms);
            std::sort(lat.begin(), lat.end());
            const u32 hz = VIZR::g_present_hz.load(std::memory_order_relaxed);
            const u32 pairs_out = pairs_published.exchange(0, std::memory_order_relaxed);
            const u32 display = VIZR::g_display_hz.load(std::memory_order_relaxed);
            const float two_frames_ms = 2000.0f / static_cast<float>(display != 0 ? display : 90);
            const float p50 = lat.empty() ? 0.0f : lat[lat.size() / 2];
            const bool stereo = pairs_out * 2 >= hz;
            // Copies cannot exceed the headset's refresh (a 120 game rate on a 90 Hz runtime copies at most 90 a second).
            const u32 copy_rate = display != 0 ? std::min(hz, display) : hz;
            const bool repeat = stat_not_ready * 10 > stat_xr_frames * 3;
            const bool collapse = lat.size() * 2 < static_cast<std::size_t>(copy_rate);
            const bool backlog = p50 > two_frames_ms;
            const bool lagging = hz > 60 && stereo && stat_xr_frames > 0 && (repeat || collapse || backlog);
            fresh_seconds = !lat.empty() ? fresh_seconds + 1 : fresh_seconds;
            watchdog_armed = watchdog_armed || fresh_seconds > 10;
            lag_seconds = lagging && watchdog_armed ? lag_seconds + 1 : 0;
            LOG_INFO(Render_Vulkan, "{} 1s fresh frames: copied {} pairs of {} published, publish to copy p50 {:.1f} ms "
                     "max {:.1f} ms; older pair shown {}, none ready {} of {} headset frames{}",
                     LogPrefix, lat.size(), pairs_out, p50, lat.empty() ? 0.0f : lat.back(), stat_fallback, stat_not_ready,
                     stat_xr_frames, !lagging ? "" : repeat ? " (falling behind: frames repeated)"
                                             : collapse ? " (falling behind: copies collapsed)" : " (falling behind: backlog)");
            if (lag_seconds >= 5 && VIZR::g_tick_session_cap_hz.load(std::memory_order_relaxed) == 0) {
                VIZR::g_tick_capped_from_hz.store(hz, std::memory_order_relaxed);
                VIZR::g_tick_session_cap_hz.store(60, std::memory_order_relaxed);
                LOG_WARNING(Render_Vulkan, "{} game frames are not keeping up at {} Hz ({} pairs copied of {} published, "
                            "publish to copy p50 {:.1f} ms, for {} s): back to 60 for this session", LogPrefix, hz,
                            lat.size(), pairs_out, p50, lag_seconds);
            }
            stat_fallback = 0;
        }
        LOG_INFO(Render_Vulkan,
                 "{} 1s: published={} xr_frames={} copies={} no_new={} not_ready={} "
                 "copy_busy={} no_slot={} head_failed={} projection={} stereo={} quad={} missing_pose={} "
                 "xr_missed={} xr_missed_in_hitch={} game_hitches_total={} queue={} max_hold_us {}={} {}={} {}={} {}={} {}={} "
                 "modec markers_l={} markers_r={} held={} no_hold={} held_miss={} paired={} stale={} eye_sets={} centre_kept={} "
                 "pair_wait={} pair_held={} pose_cached={} vignette={} vignette_level={}",
                 LogPrefix, stat_published.exchange(0), stat_xr_frames, stat_copies,
                 stat_no_new_frame, stat_not_ready, stat_copy_busy, stat_no_slot.exchange(0),
                 stat_head_failed, stat_projection_frames, stat_stereo_frames, stat_quad_frames, stat_missing_pose,
                 stat_xr_missed, stat_xr_missed_in_hitch,
                 VIZR::g_xr_drops.game_hitches.load(std::memory_order_relaxed),
                 own_queue ? "own" : "shared",
                 LockedCallNames[0], max_hold_us[0], LockedCallNames[1], max_hold_us[1],
                 LockedCallNames[2], max_hold_us[2], LockedCallNames[3], max_hold_us[3],
                 LockedCallNames[4], max_hold_us[4],
                 VIZR::g_modec_stats.markers_l.exchange(0), VIZR::g_modec_stats.markers_r.exchange(0),
                 VIZR::g_modec_stats.held_copies.exchange(0), VIZR::g_modec_stats.no_hold.exchange(0),
                 VIZR::g_modec_stats.held_miss.exchange(0), VIZR::g_modec_stats.paired.exchange(0),
                 VIZR::g_modec_stats.stale.exchange(0), VIZR::g_modec_stats.eye_sets.exchange(0),
                 VIZR::g_modec_stats.centre_kept.exchange(0), stat_pair_wait, stat_pair_held, stat_pose_cached,
                 stat_vignette_frames, vignette_level);
        if (menu && (menu->IsOpen() || stat_menu_layers != 0)) {
            LOG_INFO(Render_Vulkan, "{} 1s: mod menu {}, layer submitted in {} frames", LogPrefix,
                     menu->IsOpen() ? "open" : "closed", stat_menu_layers);
        }
        stat_menu_layers = 0;
        stat_xr_frames = stat_copies = stat_no_new_frame = stat_not_ready = stat_copy_busy =
            stat_head_failed = stat_projection_frames = stat_stereo_frames = stat_quad_frames = stat_missing_pose =
                stat_xr_missed = stat_xr_missed_in_hitch = stat_pair_wait = stat_pair_held = stat_pose_cached =
                    stat_vignette_frames = 0;
        max_hold_us.fill(0);
    }

    /// The previous copy's fence: true when done, false while busy (a timeout); a lost device ends the session instead of
    /// counting as busy forever.
    bool CopyFenceDone(u64 timeout_ns) {
        const VkResult r = copy_fence.Wait(timeout_ns);
        if (r == VK_SUCCESS) {
            return true;
        }
        if (r == VK_TIMEOUT) {
            return false;
        }
        throw XrFatal(fmt::format("copy fence wait failed: {} (device lost?)", static_cast<int>(r)));
    }

    void DestroyChain(Chain& chain) {
        if (copy_pending) {
            // Bounded: a lost device never signals it. After 2 s the swapchain goes anyway (the device is gone).
            if (const VkResult r = copy_fence.Wait(2'000'000'000); r != VK_SUCCESS) {
                LOG_WARNING(Render_Vulkan, "{} the last copy did not finish ({}); tearing down anyway", LogPrefix,
                            static_cast<int>(r));
            }
            copy_pending = false;
        }
        if (chain.handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(chain.handle);
        }
        chain = Chain{};
    }

    void DestroyXrSwapchain() {
        for (auto& chain : chains) {
            DestroyChain(chain);
        }
        show_stereo = false;
    }

    void DestroySession() {
        DestroyInput();
        DestroyXrSwapchain();
        if (menu) {
            menu->DestroySwapchain();
        }
        have_recenter = false;
        prev_display_time = 0;
        if (view_space != XR_NULL_HANDLE) {
            xrDestroySpace(view_space);
            view_space = XR_NULL_HANDLE;
        }
        if (space != XR_NULL_HANDLE) {
            xrDestroySpace(space);
            space = XR_NULL_HANDLE;
        }
        // Children before their session: the vignette swapchain belongs to it.
        if (vignette.handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(vignette.handle);
            vignette = {};
        }
        vignette_level = 0;
        vignette_failed = false; // a new session may create it again
        if (session != XR_NULL_HANDLE) {
            if (session_running) {
                xrEndSession(session);
            }
            xrDestroySession(session);
            session = XR_NULL_HANDLE;
        }
        session_running = false;
        session_state = XR_SESSION_STATE_UNKNOWN;
        last_good_valid = false;
        std::scoped_lock lk{slot_mutex};
        held = held_pair = -1;
    }

    void Teardown() {
        try {
            DestroySession();
        } catch (...) {
            // Teardown must never throw into Stop().
        }
        // The command pool, buffer and fence go with Impl, which ~Presenter destroys before
        // the device (Presenter is RendererVulkan's last member).
    }

    Runtime& runtime;
    Runtime::Impl& rt;
    const vk::Instance& instance;
    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& allocator; // The vignette's staging buffer
    Swapchain& swapchain;
    RasterizerVulkan& rasterizer;
    BlitScreen blit;

    // Ring, shared between the Composite thread and the XR thread under slot_mutex. The XR
    // thread takes "latest" and marks it held in one step; Composite picks a slot that is
    // neither, in one step, so a slot is never overwritten mid-copy.
    std::array<Slot, RingSize> slots{};
    std::mutex slot_mutex;
    int latest = -1;
    std::array<int, 3> latest_by_eye{-1, -1, -1}; // Newest slot per VIZR:Eye (left, right, mono)
    int held = -1;
    int held_pair = -1; // Mode C, the left eye's slot while its pair is copied (the right eye is `held`)
    struct PairRec {    // a completed eye pair (both eyes of one frame published), newest first
        int l = -1, r = -1;
        u64 tick = 0;   // the later of the two eyes' GPU ticks
        u64 seq_l = 0, seq_r = 0;
        s64 published_ns = 0;
    };
    std::array<PairRec, 2> pairs{};
    u64 stat_fallback = 0;               // copies of the older pair (the newer one not finished yet)
    std::vector<float> copy_latency_ms;  // publish -> copy this second (XR thread)
    u32 lag_seconds = 0;                 // consecutive seconds the fresh frames fell behind (the watchdog)
    u32 fresh_seconds = 0;               // seconds with fresh frames copied (gameplay, not loading)
    std::atomic<u32> pairs_published{0}; // completed eye pairs published this second (the game thread)
    bool watchdog_armed = false;         // after 10 of them
    u64 publish_seq = 0;
    std::vector<Tegra::FramebufferConfig> eye_framebuffers; // Composite thread: the cropped copy
    VIZR::SeqLocked<EyeViews> eye_views;                 // written by the XR thread

    std::atomic<bool> enabled{false};
    std::thread thread;
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    std::condition_variable exit_cv;
    std::atomic<bool> stopping{false};
    bool exited = false;
    std::string pending_off;
    std::atomic<bool> leaked{false};

    // XR thread only.
    XrSession session{XR_NULL_HANDLE};
    XrSpace space{XR_NULL_HANDLE};
    XrSpace view_space{XR_NULL_HANDLE};
    std::array<Chain, 3> chains{};
    bool show_stereo = false; // the newest game frame is an eye frame: submit the two eye chains
    std::atomic<bool> session_running{false};
    bool session_lost = false;
    XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
    vk::CommandPool command_pool;
    vk::CommandBuffers command_buffers;
    vk::CommandBuffer command_buffer;
    vk::Fence copy_fence;
    bool copy_pending = false;
    // M4 (XR thread only)
    bool own_queue = false;          // the session uses the device's own XR queue (index 1)
    bool have_recenter = false;
    bool recenter_requested = false;
    XrTime pending_space_change = 0; // runtime recenter: apply to poses from this time
    float recenter_yaw = 0.0f;
    u32 recenter_count = 0;                 // The torso offset restarts at every recenter
    bool torso_rebase = false;
    bool torso_valid = false;
    bool shadow_valid = false;                     // the body-rules log (LogBodyRules)
    std::array<float, 2> shadow_anchor{};
    float anchor_yaw = 0.0f, follow_yaw = 0.0f;
    // The hands feed the torso estimate only after both rest this long (UpdateTorso).
    static constexpr float RestBelowHeadM = -0.30f; // hand height relative to the eyes
    static constexpr float RestReachM = 0.45f;      // horizontal distance from the head
    static constexpr s64 RestHoldNs = 500'000'000;
    s64 rest_since_ns = 0;
    bool hands_feed = false;
    float hold_yaw = 0.0f; // the estimate a hold keeps
    // A hold follows a sustained head turn (UpdateTorso): the gate is a 60 deg head turn held with the hands raised,
    // the body within 10 deg of the head by 6 s.
    static constexpr float HoldLimitDeg = 45.0f;  // a hold is never further than this from the head
    static constexpr float DriftStartDeg = 20.0f; // the head this far from the estimate ...
    static constexpr s64 DriftArmNs = 1'000'000'000; // ... for this long starts the drift
    static constexpr float DriftTauS = 2.5f;      // the drift's time constant
    static constexpr float DriftStopDeg = 5.0f;   // the drift ends this close to the head
    bool drifting = false;
    s64 drift_over_ns = 0;  // since when the head has been more than DriftStartDeg from the estimate (0: not)
    s64 drift_since_ns = 0; // the drift's start
    std::array<std::pair<s64, float>, 128> torso_history{}; // (time, estimate) ring: more than RestHoldNs at 120 Hz
    std::size_t torso_history_next = 0;
    std::array<float, 2> log_stick{}; // the left stick as read (body rules log)
    s64 shadow_logged_ns = 0;
    float torso_yaw = 0.0f, torso_held = 0.0f, torso_ref = 0.0f;
    s64 torso_ns = 0;
    float headset_vfov = 0.0f;
    float ipd_m = 0.0f; // Distance between the eyes from xrLocateViews
    VIZR::FlatBasis flat_basis; // IPD/FOV the flat-layer pipelines were last built for
    u32 recommended_width = 0, recommended_height = 0, max_width = 0, max_height = 0;
    bool fov_logged = false;
    XrTime prev_display_time = 0;
    XrActionSet action_set{XR_NULL_HANDLE};
    std::array<XrAction, 11> button_actions{};
    std::array<XrAction, 2> stick_actions{};
    // The mod menu and its input. (XR thread only)
    std::unique_ptr<Menu> menu;
    XrAction aim_action{XR_NULL_HANDLE};
    XrAction left_aim_action{XR_NULL_HANDLE};
    XrAction left_haptic_action{XR_NULL_HANDLE};
    XrSpace left_aim_space{XR_NULL_HANDLE};
    // M7 gestures: XR thread only.
    VIZR::HandPose left_hand_view{}, right_hand_view{}; // this frame's hands relative to the head
    float head_height{};                // the head's height in the tracking space this frame
    float stand_height{};               // the head's height at the last recenter (standing)
    std::array<float, 3> recenter_position{}; // The head's position at the last recenter
    bool stand_height_valid = false;
    bool at_head = false, at_cannon = false;
    s64 head_enter_ns{};
    int head_choice = -1;               // the lit ring direction while the hand is at the head
    int head_refused = -1;              // the last unowned direction pushed (logged once)
    bool fade_logged = false;
    s64 fade_start_ns{};
    bool crouched = false;              // the player's posture (head below the crouch line)
    s64 stand_since_ns = 0;             // the head back within 0.15 m of standing since (0 = not)
    s64 unmorph_until_ns = 0, unmorph_next_ns = 0; // the way out of the ball after standing up (0 = not leaving)
    u32 unmorph_taps = 0;
    bool near_head = false, near_entered = false; // a left-hand pass within 0.35 m of the head (the tap log)
    float near_min_m = 9.0f;
    s64 near_enter_ns = 0;
    int crouch_request = 0;             // +1 down / -1 up, waiting while the game is mid-transition
    s64 crouch_request_ns{};
    XrSpace aim_space{XR_NULL_HANDLE};
    XrPosef last_head_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    XrTime menu_display_time = 0;
    Menu::Input menu_input{};
    bool menu_prev_left = false, menu_prev_right = false, menu_prev_b = false, menu_scroll_held = false;
    bool menu_release_wait = false;
    // Hotfix
    XrAction haptic_action{XR_NULL_HANDLE};
    s64 pair_since_ns = 0;
    std::array<VIZR::ProTarget, static_cast<std::size_t>(VIZR::TouchInput::Count)> held_target{};
    std::array<bool, static_cast<std::size_t>(VIZR::TouchInput::Count)> held_valid{};
    bool menu_layer_last_frame = false;
    int menu_frames_unseen = 0;
    u64 stat_menu_layers = 0;
    // M6 snap turn (XR thread only)
    bool snap_active = false, snap_armed = true;
    float snap_dir = 0.0f, snap_start_yaw = 0.0f;
    s64 snap_start_ns = 0;
    // The snap's stick edge (SnapEdge): the release, the push in progress, the last snap
    s64 snap_below_ns = 0, snap_released_ms = -1, snap_last_ns = 0;
    float snap_last_dir = 0.0f;
    bool snap_push = false;
    float snap_push_peak = 0.0f, snap_push_y = 0.0f, snap_push_dir = 0.0f;
    s64 snap_push_ns = 0, snap_push_below_ms = -1, snap_push_since_ms = -1;
    const char* snap_push_result = "";
    s64 left_click_ns = 0, right_click_ns = 0;
    bool input_ok = false;
    VIZR::ProPad last_pad{};
    bool modifier_down = false;
    bool chord_used = false;
    static constexpr int TapFrames = 5; // ~55 ms at 90 Hz: at least two game frames
    std::array<int, 16> tap_frames_left{}; // per ProTarget 0..15
    std::array<bool, static_cast<std::size_t>(VIZR::TouchInput::Count)> recenter_down{};
    std::array<bool, static_cast<std::size_t>(VIZR::TouchInput::Count)> chord_prev{};
    /// Stick chord sources: the chord a flick will send on its return, and when it started.
    std::array<VIZR::ProTarget, static_cast<std::size_t>(VIZR::TouchInput::Count)> flick_target = [] {
        std::array<VIZR::ProTarget, static_cast<std::size_t>(VIZR::TouchInput::Count)> a;
        a.fill(VIZR::ProTarget::None);
        return a;
    }();
    std::array<s64, static_cast<std::size_t>(VIZR::TouchInput::Count)> flick_start_ns{};
    /// The target each chord source has latched (None = not latched).
    std::array<VIZR::ProTarget, static_cast<std::size_t>(VIZR::TouchInput::Count)> latched = [] {
        std::array<VIZR::ProTarget, static_cast<std::size_t>(VIZR::TouchInput::Count)> a;
        a.fill(VIZR::ProTarget::None);
        return a;
    }();

    // Stats for the per-second QA line.
    std::atomic<u64> stat_published{0};
    std::mutex frame_ms_mutex;  // the game's frame times since the last stats line (publish thread -> stats)
    std::vector<float> frame_ms;
    s64 frame_last_ns = 0;      // the last publish (publish thread only)
    std::atomic<u64> stat_no_slot{0};
    u64 stat_xr_frames = 0;
    u64 stat_copies = 0;
    u64 stat_no_new_frame = 0;
    u64 stat_not_ready = 0;
    u64 stat_copy_busy = 0;
    u64 stat_head_failed = 0;
    u64 stat_projection_frames = 0;
    u64 stat_stereo_frames = 0; // Projection frames built from the two eye chains
    u64 stat_quad_frames = 0;
    u64 stat_vignette_frames = 0; // Headset frames with the comfort vignette layer
    u64 settings_seen = ~0ull;    // The user-settings change count last applied (Compatibility tab)
    bool frame_hold = true;
    bool settings_snapshot_logged = false; // The first stereo frame's settings line (test check)
    float refresh_setting = 0.0f;
    std::vector<float> offered_rates;   // the runtime's refresh rates (RequestRefreshRate)
    bool cap_requested = false;          // ApplyRefreshCap ran its request
    Chain vignette;               // The vignette's swapchain (handle, images, size)
    vk::Buffer vignette_staging;
    int vignette_level = 0;       // the strength step now in the swapchain (0 = none uploaded)
    bool vignette_failed = false;
    u64 stat_pair_wait = 0;   // Mode C, the producer was between the two eyes of a frame
    u64 stat_pair_held = 0;   // Mode C gameplay frames kept 3D without a fresh same-frame pair
    u64 stat_pose_cached = 0; // A chain's pose from the cache (its ledger entry aged out)
    std::array<std::pair<u64, VIZR::PoseLedger::Rendered>, 3> rendered_cache{};
    std::array<XrCompositionLayerProjectionView, 2> last_good_views{};
    bool last_good_valid = false;
    u64 stat_missing_pose = 0;
    u64 stat_xr_missed = 0;
    u64 stat_xr_missed_in_hitch = 0;
    std::array<s64, CallCount> max_hold_us{};
    std::chrono::steady_clock::time_point last_stats{};
};

Presenter::Presenter(Runtime& runtime, const vk::Instance& instance, const Device& device,
                     Scheduler& scheduler, MemoryAllocator& memory_allocator,
                     PresentManager& present_manager, Swapchain& swapchain,
                     Tegra::MaxwellDeviceMemoryManager& device_memory,
                     RasterizerVulkan& rasterizer) {
    if (!runtime.IsActive()) {
        return; // no Impl at all: stock Eden
    }
    impl = std::make_unique<Impl>(runtime, instance, device, scheduler, memory_allocator,
                                  present_manager, swapchain, device_memory, rasterizer);
    impl->Start();
}

Presenter::~Presenter() {
    Stop();
}

void Presenter::Publish(std::span<const Tegra::FramebufferConfig> framebuffers, u64 tag) {
    if (impl) {
        impl->Publish(framebuffers, tag);
    }
}

void Presenter::Stop() {
    if (!impl) {
        return;
    }
    impl->Stop();
    if (impl->leaked) {
        void(impl.release()); // the detached thread may still reference it
    }
}

} // namespace Vulkan::OpenXr

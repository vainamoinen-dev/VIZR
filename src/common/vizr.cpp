// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>

#include "common/logging.h"
#include "common/vizr.h"

namespace VIZR {

s64 SteadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

s64 GamePeriodNs() {
    const u32 hz = std::max<u32>(g_present_hz.load(std::memory_order_relaxed), 1);
    return 1'000'000'000LL / hz;
}

bool GameHitchActive(s64 now_ns) {
    const s64 last = g_last_published_ns.load(std::memory_order_relaxed);
    return (last != 0 && now_ns - last > 2 * GamePeriodNs()) ||
           now_ns < g_xr_drops.hitch_until_ns.load(std::memory_order_relaxed);
}

void NoteGameFramePublished() {
    // M4 hitch tracking, with or without a profile.
    const s64 now = SteadyNowNs();
    const s64 last = g_last_published_ns.exchange(now, std::memory_order_relaxed);
    if (last != 0 && now - last > 2 * GamePeriodNs()) {
        g_xr_drops.game_hitches.fetch_add(1, std::memory_order_relaxed);
        g_xr_drops.hitch_until_ns.store(now + GamePeriodNs(), std::memory_order_relaxed);
    }

    auto& s = g_hook_state;
    if (!s.installed.load(std::memory_order_relaxed)) {
        return;
    }
    const u32 frames = s.frames_since_hook.fetch_add(1, std::memory_order_relaxed) + 1;
    const u32 limit = s.silence_frames.load(std::memory_order_relaxed);
    // Hotfix: frames published around a hitch or a short transition could reach the frame
    // limit within ~60 ms and flash the flat screen; a real pause, map or cutscene is silent far longer.
    constexpr s64 MinSilenceNs = 250'000'000;
    const s64 silent_ns = now - s.last_hook_ns.load(std::memory_order_relaxed);
    if (frames > limit && silent_ns > MinSilenceNs && s.camera_active.exchange(false, std::memory_order_acq_rel)) {
        s.passthrough_events.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO(Core, "VIZR hooks: hook silent {} frames ({} ms) -> passthrough (event {})", frames,
                 silent_ns / 1'000'000, s.passthrough_events.load());
    }
}

bool HookSilent() {
    const auto& s = g_hook_state;
    if (!s.installed.load(std::memory_order_relaxed)) {
        return false;
    }
    // A profile's explicit flat state (pause, map,...) counts as the menu too.
    return s.flat_state.load(std::memory_order_relaxed) ||
           (s.frames_since_hook.load(std::memory_order_relaxed) > s.silence_frames.load(std::memory_order_relaxed) &&
            SteadyNowNs() - s.last_hook_ns.load(std::memory_order_relaxed) > 250'000'000); // as the passthrough rule
}

void NoteHookHit() {
    g_hook_state.last_hook_ns.store(SteadyNowNs(), std::memory_order_relaxed);
    g_hook_state.frames_since_hook.store(0, std::memory_order_relaxed);
    g_hook_state.hits.fetch_add(1, std::memory_order_relaxed);
}

void ResetHookState() {
    auto& s = g_hook_state;
    s.installed = false;
    s.camera_active = false;
    s.frames_since_hook = 0;
    s.silence_frames = 3;
    s.flat_state = false;
    s.hits = 0;
    s.lag_probe_id = 0;
    s.pose_lag_frames = 0;
    g_present_hz = 60;
    g_stereo_mode = StereoMode::Mono;
    g_flat_config.Store(FlatConfig{});
    g_flat_active = false;
    g_draw_eye = Eye::Both;
    g_input_layout.Store(InputLayout{});
    g_snap_instant = false;
    g_torso_hold = false;
    {
        const std::scoped_lock lock(g_menu_labels_mutex);
        g_menu_labels.clear();
    }
}

} // namespace VIZR

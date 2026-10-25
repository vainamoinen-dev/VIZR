// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

namespace Tegra {
struct FramebufferConfig;
}

namespace VideoCore {
class RendererBase;
}

namespace VIZR {

/**
 * Flat-screen test capture in the rig's frames.jsonl + PNG format, so its
 * gate and metric scripts run unchanged on Eden runs. Off unless VIZR_RIG_OUT is set:
 *   VIZR_RIG_OUT            run folder (created if missing); receives frames.jsonl and PNGs
 *   VIZR_RIG_CAPTURE_EVERY  PNG every N composited frames (default 60, 0 = no PNGs)
 *   VIZR_RIG_CAPTURE_SCALE  PNG size as a multiple of 1280x720 (default 1)
 * phases.csv is written by the test script, not here. Extra frames.jsonl keys (M3): pose_id (the
 * frame's pose ledger id, 0 = no hook camera) and hooks (total hook hits so far).
 * VIZR_LAG_PROBE=N (with a profile): every N camera writes the hook rolls one frame 90 degrees;
 * here a 10-frame capture burst finds that frame and logs the pose lag (A7).
 *
 * Also counts published game frames for the hook silence rule, with capture on or off.
 *
 * Replaces a direct renderer.Composite(layers) call; with capture off it is exactly that call.
 */
void Composite(VideoCore::RendererBase& renderer,
               std::span<const Tegra::FramebufferConfig> framebuffers);

} // namespace VIZR

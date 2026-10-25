// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/stb.h"
#include "common/vizr.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/framebuffer_config.h"
#include "video_core/renderer_base.h"
#include "video_core/vizr/rig_capture.h"

namespace VIZR {
namespace {

constexpr std::size_t MaxQueuedPngs = 8;

/// One requested PNG. The renderer copies the pixels into `pixels` before it clears its
/// screenshot-pending flag; `invert_y` arrives later through the renderer's async callback.
struct CaptureJob {
    std::vector<unsigned char> pixels; // BGRA, as the renderers write screenshots
    unsigned width{};
    unsigned height{};
    unsigned long long frame{};
    std::chrono::system_clock::time_point wall_time{};
    std::atomic<int> invert_y{-1}; // -1 = callback not run yet
};

class RigCapture {
public:
    static RigCapture* Get() {
        static std::optional<RigCapture> instance;
        static const bool created = [] {
            const char* out = std::getenv("VIZR_RIG_OUT");
            if (out != nullptr && *out != '\0') {
                instance.emplace(out);
            }
            return true;
        }();
        return created && instance && instance->enabled ? &*instance : nullptr;
    }

    explicit RigCapture(const char* out)
        : out_dir{out}, every{EnvUnsigned("VIZR_RIG_CAPTURE_EVERY", 60)} {
        const unsigned scale = std::max(1u, EnvUnsigned("VIZR_RIG_CAPTURE_SCALE", 1));
        layout = Layout::DefaultFrameLayout(Layout::ScreenUndocked::Width * scale,
                                            Layout::ScreenUndocked::Height * scale);
        // One run folder = one session: never mix frame numbers or PNGs of two runs.
        std::error_code ec;
        if (std::filesystem::exists(out_dir / "frames.jsonl", ec)) {
            LOG_ERROR(Render, "VIZR rig capture: off, {} already holds a frames.jsonl (use a fresh run folder)",
                      out);
            return;
        }
        std::filesystem::create_directories(out_dir, ec);
        frames_out.open(out_dir / "frames.jsonl", std::ios::trunc);
        if (!frames_out) {
            LOG_ERROR(Render, "VIZR rig capture: off, could not create {}/frames.jsonl", out);
            return;
        }
        enabled = true;
        LOG_INFO(Render, "VIZR rig capture: out={} every={} size={}x{}", out, every,
                 layout.width, layout.height);
        writer = std::jthread([this](std::stop_token stop) { WriterLoop(stop); });
    }

    void Composite(VideoCore::RendererBase& renderer,
                   std::span<const Tegra::FramebufferConfig> framebuffers) {
        if (session_renderer == nullptr) {
            session_renderer = &renderer;
        } else if (session_renderer != &renderer) {
            // Eden builds a new renderer for each game launch. Keep one session per run folder.
            // A new renderer at the old address is not detected; frame numbers then keep counting
            // up in one frames.jsonl. Bot rule: one game launch per Eden process.
            LOG_WARNING(Render, "VIZR rig capture: a second game session started in this process; capture stopped (one session per run folder)");
            in_flight.reset();
            enabled = false;
            renderer.Composite(framebuffers);
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (frame == 0) {
            start = now;
        }
        ++frame; // counts Composite calls, including ones a hidden window skips
        const u64 tag = Tegra::FramePoseId(framebuffers);

        // Lag probe (A7, M3): the hook arms a pose id a few frames ahead; capture every frame
        // from now on for a short burst and find where the probe's roll shows up.
        const u64 probe = g_hook_state.lag_probe_id.load(std::memory_order_relaxed);
        if (probe != 0 && probe != probe_seen) {
            probe_seen = probe;
            burst_left = BurstFrames;
            burst.clear();
        }
        const bool in_burst = burst_left > 0;

        // The renderer has one screenshot slot, shared with the game's album capture
        // (caps_su) and the user's screenshot key. While our request is pending (only possible
        // with a hidden window) theirs are refused; rare in bot runs, accepted.
        if (((every != 0 && frame % every == 0) || in_burst) && !in_flight &&
            !renderer.IsScreenshotPending()) {
            auto job = std::make_shared<CaptureJob>();
            job->width = layout.width;
            job->height = layout.height;
            job->pixels.resize(static_cast<std::size_t>(layout.width) * layout.height * 4);
            renderer.RequestScreenshot(
                job->pixels.data(),
                // Runs on a detached thread, possibly after shutdown: touch only the job.
                [job](bool invert_y) { job->invert_y = invert_y ? 1 : 0; }, layout);
            // Another requester can claim the slot between our check and our request; ours is
            // then ignored. Keep the job only if the slot really points at our buffer.
            if (IsOurs(renderer, *job)) {
                in_flight = std::move(job);
            }
        }

        renderer.Composite(framebuffers);

        // A hidden window skips the screenshot, so a request can stay pending for several
        // frames; it belongs to whichever frame finally draws it.
        bool captured = false;
        if (in_flight && !renderer.IsScreenshotPending()) {
            if (IsOurs(renderer, *in_flight)) {
                in_flight->frame = frame;
                in_flight->wall_time = std::chrono::system_clock::now();
                if (in_burst) {
                    burst.push_back({frame, tag, Thumbnail(*in_flight)});
                }
                captured = Enqueue(std::move(in_flight));
            } else {
                // The slot moved on: another requester took it right after ours completed, or a
                // new renderer landed at the old address. Never keep the job, or every later
                // request would be blocked.
                ++lost;
                LOG_WARNING(Render, "VIZR rig capture: request lost at frame {} ({} so far)",
                            frame, lost);
            }
            in_flight.reset();
        }

        if (in_burst && --burst_left == 0) {
            AnalyzeProbe();
        }

        // pose_id and hooks are extra keys (the test scripts ignore unknown keys).
        const double t = std::chrono::duration<double>(now - start).count();
        frames_out << fmt::format(
            "{{\"frame\":{},\"t\":{:.3f},\"layers\":0,\"capture\":{},\"items\":[],\"pose_id\":{},"
            "\"hooks\":{},\"xr_missed\":{},\"xr_missed_in_hitch\":{},\"game_hitches\":{}}}\n",
            frame, t, captured ? "true" : "false", tag,
            g_hook_state.hits.load(std::memory_order_relaxed),
            g_xr_drops.xr_missed.load(std::memory_order_relaxed),
            g_xr_drops.xr_missed_in_hitch.load(std::memory_order_relaxed),
            g_xr_drops.game_hitches.load(std::memory_order_relaxed));
        frames_out.flush();
    }

private:
    static constexpr int BurstFrames = 10;
    static constexpr unsigned ThumbW = 32;
    static constexpr unsigned ThumbH = 18;

    struct BurstFrame {
        unsigned long long frame{};
        u64 tag{};
        std::vector<u8> thumb;
    };

    /// 32x18 grey image of a capture, enough to see a 90-degree roll against its neighbours.
    static std::vector<u8> Thumbnail(const CaptureJob& job) {
        std::vector<u8> thumb(ThumbW * ThumbH);
        for (unsigned ty = 0; ty < ThumbH; ++ty) {
            for (unsigned tx = 0; tx < ThumbW; ++tx) {
                unsigned sum = 0;
                unsigned count = 0;
                for (unsigned y = ty * job.height / ThumbH; y < (ty + 1) * job.height / ThumbH; y += 4) {
                    for (unsigned x = tx * job.width / ThumbW; x < (tx + 1) * job.width / ThumbW; x += 4) {
                        const unsigned char* p = &job.pixels[(static_cast<std::size_t>(y) * job.width + x) * 4];
                        sum += (p[0] + p[1] * 2u + p[2]) / 4u;
                        ++count;
                    }
                }
                thumb[ty * ThumbW + tx] = static_cast<u8>(count != 0 ? sum / count : 0);
            }
        }
        return thumb;
    }

    /// The first frame that differs sharply from the one before it carries the probe's camera;
    /// its pose_id minus the probe id is the pose lag in hook frames (A7).
    void AnalyzeProbe() {
        std::vector<double> diffs;
        for (std::size_t i = 1; i < burst.size(); ++i) {
            double sum = 0;
            for (std::size_t p = 0; p < burst[i].thumb.size(); ++p) {
                sum += std::abs(static_cast<int>(burst[i].thumb[p]) - static_cast<int>(burst[i - 1].thumb[p]));
            }
            diffs.push_back(sum / static_cast<double>(burst[i].thumb.size()));
        }
        std::string list;
        for (std::size_t i = 0; i < diffs.size(); ++i) {
            list += fmt::format("{}{}:{:.1f}", i != 0 ? " " : "", burst[i + 1].frame, diffs[i]);
        }
        if (diffs.size() < 3) {
            LOG_WARNING(Render, "VIZR lag probe {}: only {} frames captured (window hidden?)",
                        probe_seen, burst.size());
            return;
        }
        std::vector<double> sorted = diffs;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[sorted.size() / 2];
        const double threshold = std::max(3.0 * median, median + 4.0);
        for (std::size_t i = 0; i < diffs.size(); ++i) {
            if (diffs[i] > threshold) {
                const BurstFrame& hit = burst[i + 1];
                const s64 lag = static_cast<s64>(hit.tag) - static_cast<s64>(probe_seen);
                LOG_INFO(Render,
                         "VIZR lag probe {}: rolled image first at frame {} (pose_id {}) -> pose lag {} frame(s); diffs [{}]",
                         probe_seen, hit.frame, hit.tag, lag, list);
                return;
            }
        }
        LOG_WARNING(Render, "VIZR lag probe {}: no rolled frame found (threshold {:.1f}); diffs [{}]",
                    probe_seen, threshold, list);
    }

    static bool IsOurs(const VideoCore::RendererBase& renderer, const CaptureJob& job) {
        return renderer.Settings().screenshot_bits == job.pixels.data();
    }

    static unsigned EnvUnsigned(const char* name, unsigned fallback) {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0') {
            return fallback;
        }
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (*end != '\0') {
            LOG_WARNING(Render, "VIZR rig capture: ignoring {}='{}' (not a number)", name,
                        value);
            return fallback;
        }
        return static_cast<unsigned>(parsed);
    }

    /// Returns false (and the frame is not marked as captured) if the PNG cannot be queued.
    bool Enqueue(std::shared_ptr<CaptureJob> job) {
        {
            std::scoped_lock lk{mutex};
            if (queue.size() >= MaxQueuedPngs) {
                ++dropped;
                LOG_WARNING(Render, "VIZR rig capture: PNG queue full, dropped frame {} ({} so far)",
                            job->frame, dropped);
                return false;
            }
            queue.push_back(std::move(job));
        }
        cv.notify_all();
        return true;
    }

    void WriterLoop(std::stop_token stop) {
        while (true) {
            std::shared_ptr<CaptureJob> job;
            {
                std::unique_lock lk{mutex};
                if (!cv.wait(lk, stop, [this] { return !queue.empty(); })) {
                    // Process exit: drop what is left without encoding or logging, since the
                    // logging backend may already be gone during static destruction.
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
            }
            // The pixels are already copied; only invert_y comes through the renderer's
            // detached callback, normally within microseconds.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (job->invert_y.load() == -1 && std::chrono::steady_clock::now() < deadline) {
                if (stop.stop_requested()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            if (job->invert_y.load() == -1) {
                LOG_WARNING(Render, "VIZR rig capture: no callback for frame {}, assuming upright",
                            job->frame);
            }
            WritePng(*job);
        }
    }

    void WritePng(const CaptureJob& job) {
        // BGRA -> RGB, flipped when the renderer says so (OpenGL).
        const bool invert = job.invert_y.load() == 1;
        std::vector<unsigned char> rgb(static_cast<std::size_t>(job.width) * job.height * 3);
        for (unsigned y = 0; y < job.height; ++y) {
            const unsigned src_y = invert ? job.height - 1 - y : y;
            const unsigned char* src = &job.pixels[static_cast<std::size_t>(src_y) * job.width * 4];
            unsigned char* dst = &rgb[static_cast<std::size_t>(y) * job.width * 3];
            for (unsigned x = 0; x < job.width; ++x) {
                dst[x * 3 + 0] = src[x * 4 + 2];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 0];
            }
        }

        std::vector<unsigned char> png;
        stbi_write_png_to_func(
            [](void* context, void* data, int size) {
                auto* out = static_cast<std::vector<unsigned char>*>(context);
                const auto* bytes = static_cast<const unsigned char*>(data);
                out->insert(out->end(), bytes, bytes + size);
            },
            &png, static_cast<int>(job.width), static_cast<int>(job.height), 3, rgb.data(),
            static_cast<int>(job.width * 3));

        // Same name as the test runtime uses for a quad layer: Eden's flat window becomes the
        // quad layer at M2, so captures keep one name before and after.
        const auto path = out_dir / fmt::format("frame_{:06}_layer0_quad.png", job.frame);
        {
            std::ofstream file{path, std::ios::binary | std::ios::trunc};
            file.write(reinterpret_cast<const char*>(png.data()),
                       static_cast<std::streamsize>(png.size()));
            if (!file) {
                // frames.jsonl already says capture:true for this frame; QA matches on this line.
                LOG_ERROR(Render, "VIZR rig capture: PNG for frame {} not written ({})",
                          job.frame, path.string());
                return;
            }
        }
        // The test scripts map session time to wall time through PNG mtimes, so stamp the capture time,
        // not the moment the encoder finished. (Portable form; file_clock::from_sys is missing
        // in some libc++ versions.)
        using std::chrono::file_clock;
        const auto mtime = file_clock::now() + std::chrono::duration_cast<file_clock::duration>(
                                                   job.wall_time - std::chrono::system_clock::now());
        std::error_code ec;
        std::filesystem::last_write_time(path, mtime, ec);
        if (ec) {
            LOG_WARNING(Render, "VIZR rig capture: could not set mtime on {}: {}",
                        path.string(), ec.message());
        }
    }

    const std::filesystem::path out_dir;
    const unsigned every;
    Layout::FramebufferLayout layout{};
    std::ofstream frames_out;
    bool enabled{};

    // Composite (GPU) thread only.
    const VideoCore::RendererBase* session_renderer{};
    unsigned long long frame{};
    std::chrono::steady_clock::time_point start{};
    std::shared_ptr<CaptureJob> in_flight;
    unsigned long long lost{};
    u64 probe_seen{};
    int burst_left{};
    std::vector<BurstFrame> burst;

    // Shared with the writer thread.
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<std::shared_ptr<CaptureJob>> queue;
    unsigned long long dropped{};
    std::jthread writer; // last member: stopped and joined before the members it uses go
};

} // namespace

void Composite(VideoCore::RendererBase& renderer,
               std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!framebuffers.empty()) {
        NoteGameFramePublished(); // M3 hook_silence_frames count
    }
    if (RigCapture* capture = RigCapture::Get()) {
        capture->Composite(renderer, framebuffers);
    } else {
        renderer.Composite(framebuffers);
    }
}

} // namespace VIZR

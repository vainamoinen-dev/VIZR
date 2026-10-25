// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "common/elf.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/vizr.h"
#include "common/vizr_settings.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "core/hle/service/bcat/news/builtin_news.h"
#include "core/memory.h"
#include "core/vizr/hook_runtime.h"
#include "core/vizr/profile.h"
#include "core/vizr/user_settings.h"

namespace VIZR::Hooks {
namespace {

using Profile::Camera;
using Profile::Field;
using Profile::FieldType;
using Profile::RootKind;
using Profile::When;
using Mat3 = std::array<std::array<float, 3>, 3>;

constexpr float TestRollDegrees = 30.0f;  // --vr-test-roll
constexpr float ProbeRollDegrees = 90.0f; // VIZR_LAG_PROBE: unmistakable in a capture

u32 FieldSize(FieldType type) {
    switch (type) {
    case FieldType::U8:
        return 1;
    case FieldType::U16:
        return 2;
    case FieldType::U32:
    case FieldType::F32:
        return 4;
    case FieldType::U64:
    case FieldType::Ptr:
    case FieldType::Addr: // the address must be mapped for the slot reads that follow
        return 8;
    case FieldType::Vec3f:
        return 12;
    case FieldType::Quat:
        return 16;
    case FieldType::Mat3x4:
        return 48;
    case FieldType::Mat4:
        return 64;
    }
    return 0;
}

Mat3 Multiply(const Mat3& a, const Mat3& b) {
    Mat3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        }
    }
    return out;
}

Mat3 Transpose(const Mat3& m) {
    Mat3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            out[i][j] = m[j][i];
        }
    }
    return out;
}

/// Roll about the view axis in the game's camera-local frame: B * Rz(angle) * B^T, where B's
/// Columns are the game vectors for OpenXR right, up and back.
Mat3 LocalRoll(const Profile::Space& space, float degrees) {
    const float r = degrees * std::numbers::pi_v<float> / 180.0f;
    const Mat3 rz{{{std::cos(r), -std::sin(r), 0.0f}, {std::sin(r), std::cos(r), 0.0f}, {0.0f, 0.0f, 1.0f}}};
    Mat3 b{};
    for (std::size_t col = 0; col < 3; ++col) {
        for (std::size_t row = 0; row < 3; ++row) {
            b[row][col] = space.local_basis[col][row];
        }
    }
    return Multiply(Multiply(b, rz), Transpose(b));
}

struct FieldState {
    const Field* field{};
    u64 root_address{}; // Static / Sym roots, resolved at install
    std::atomic<bool> null_logged{false};
    std::atomic<bool> addr_read_logged{false}; // A value read of an address-only field (logged once)
};

struct HookEntry {
    std::string name;
    u64 pc{};
    std::vector<const Camera*> cameras; // evaluated in profile order at this hook
    std::atomic<bool> disabled{false};
    // Per-second QA line.
    std::atomic<u64> window_hits{0};
    std::atomic<u64> window_ns{0};
    std::atomic<u64> window_max_ns{0};
    std::atomic<u64> window_writes{0};
    // Whole-run histogram of callback time, 1 us buckets, last = 200 us and above (p99 QA gate).
    std::array<std::atomic<u64>, 201> histogram_us{};

    double P99Us() const {
        u64 total = 0;
        for (const auto& bucket : histogram_us) {
            total += bucket.load(std::memory_order_relaxed);
        }
        if (total == 0) {
            return 0.0;
        }
        const u64 rank = total - total / 100; // the p99 sample, counted from the fast end
        u64 seen = 0;
        for (std::size_t us = 0; us < histogram_us.size(); ++us) {
            seen += histogram_us[us].load(std::memory_order_relaxed);
            if (seen >= rank) {
                return static_cast<double>(us + 1); // upper edge of the bucket
            }
        }
        return static_cast<double>(histogram_us.size());
    }
};

/// user.toml writes off the guest threads: a hook posts the settings' change count, this thread saves (disk I/O never
/// blocks a hook). A count counts as saved only after a successful save; a failed save is tried again at the next
/// change, its error logged once. The thread is stopped and joined with the runtime.
class SettingsSaver {
public:
    void Start(std::filesystem::path path) {
        path_ = std::move(path);
        thread_ = std::jthread([this](std::stop_token stop) { Run(stop); });
    }
    /// The settings as loaded are already on disk: no save for this count.
    void MarkSaved(u64 changes) {
        const std::scoped_lock lock(mutex_);
        saved_ = changes;
    }
    void Request(u64 changes) {
        {
            const std::scoped_lock lock(mutex_);
            if (changes == saved_ || changes == failed_ || (pending_ && changes == wanted_)) {
                return;
            }
            wanted_ = changes;
            pending_ = true;
        }
        cv_.notify_one();
    }

private:
    void Run(std::stop_token stop) {
        std::unique_lock lock(mutex_);
        while (cv_.wait(lock, stop, [&] { return pending_; })) {
            const u64 changes = wanted_;
            pending_ = false;
            lock.unlock();
            std::string error;
            const bool ok = UserToml::Save(path_, g_user_settings.Load(), error);
            lock.lock();
            if (ok) {
                saved_ = changes;
                last_error_.clear();
                LOG_INFO(Core, "VIZR hooks: player settings saved to {}", path_.string());
            } else {
                failed_ = changes;
                if (error != last_error_) {
                    last_error_ = error;
                    LOG_ERROR(Core, "VIZR hooks: player settings NOT saved (tried again at the next change): {}", error);
                }
            }
        }
    }

    std::filesystem::path path_;
    std::mutex mutex_;
    std::condition_variable_any cv_;
    u64 wanted_{}, saved_{}, failed_{~0ULL};
    bool pending_{};
    std::string last_error_;
    std::jthread thread_; // last: stopped and joined first
};

/// Threading contract.
/// - Install builds the Runtime on the emulation thread and publishes it once (g_runtime, release); Shutdown deletes it
///   after the guest cores stop. Nothing else creates, replaces or frees it.
/// - Hooks run on the guest's own threads (the game thread for the camera hooks, the guest render thread for the
///   render-job and HUD hooks), possibly at the same time. Every hook callback holds `hook_mutex` for its whole run, so
///   the plain fields below are only ever touched by one hook at a time; nothing else reads or writes them.
/// - The XR thread and the frontend never touch the Runtime: they exchange data through the globals in common/vizr.h
///   (SeqLocked values, atomics, g_user_settings under g_user_settings_write).
/// - The settings saver thread (SettingsSaver) reads g_user_settings only.
struct Runtime {
    Profile::GameProfile profile;
    std::map<std::string, std::pair<u64, u64>, std::less<>> modules; // name -> {base, size}
    std::vector<std::unique_ptr<FieldState>> fields;                  // same order as profile.fields
    std::vector<std::unique_ptr<HookEntry>> hooks;                    // sorted by pc
    std::vector<u64> pcs;                                             // sorted, parallel to hooks
    const void* process{};                                            // the application KProcess
    Profile::Space space;
    Mat3 test_roll{};
    Mat3 probe_roll{};
    Mat3 basis{};     // columns = game vectors for OpenXR right, up, back (W = L)
    float psi{};      // last game yaw (kept when the game looks straight up/down)
    float psi_min{1e9f}, psi_max{-1e9f};  // per-second range of psi (QA: yaw wander)
    // Test (head-bob check): the game camera's own forward, per-second yaw / pitch range (deg).
    float gcam_yaw_min{1e9f}, gcam_yaw_max{-1e9f}, gcam_pitch_min{1e9f}, gcam_pitch_max{-1e9f};
    const Field* yaw_field{};             // Game yaw source (player body), null = the camera
    std::array<float, 3> yaw_forward{};   // its local forward vector (game coords)
    u64 probe_every{};
    Eye next_eye{Eye::Left};              // M5 AFR: the eye the next camera write renders
    // Mode C: the draw camera node's centre transform this frame and the last eye
    // transform written (graphics thread only).
    std::array<float, 12> modec_centre{};
    // The centre camera (eye, target, up) saved at eye L's hook, and what was last written. (look_at_copy)
    std::array<float, 9> modec_look_centre{};
    std::array<float, 9> modec_look_written{};
    bool modec_look_have_centre{};
    bool modec_look_have_written{};
    std::array<float, 12> modec_written{};
    bool modec_have_centre{};
    bool modec_have_written{};
    std::atomic<bool> modec_refused_logged{false};
    std::atomic<bool> modec_zero_logged{false}; // A zero_fields entry found on
    // What hud_draw_enter changed, restored at hud_draw_exit (game thread only).
    struct HudSaved {
        u64 widget;
        std::array<float, 12> xf;
        u32 alpha;
    };
    std::vector<HudSaved> hud_saved;
    bool hud_pending{};
    u64 hud_restores_at_enter{};
    std::filesystem::path user_toml;  // Where the player's settings live
    // The camera-to-world the camera hook wrote last (row-major 3x4) and the hand's
    // world pose the hand hook wrote last (for the fire log).
    std::array<float, 12> camera_world{};
    bool camera_world_valid{};
    s64 camera_world_ns{};               // when the camera hook last wrote camera_world (first eye)
    // [camera_now]: the game camera at the camera hook, for the hands placed before the next one.
    u64 cam_now_ptr{};
    std::array<float, 3> cam_now_at_write{};
    bool cam_now_ok{};                   // the chain matched the hook's camera at the last write
    bool cam_shift_applied{};            // camera_world already moved to this tick's camera position
    std::array<float, 3> cam_shift{};    // ... by this much (game units)
    float cam_now_err_max{};             // the check's worst error this second (log)
    u32 cam_shifts{}, cam_shift_skips{};
    u64 yaw_xf_at{};                     // the [snap] yaw_field's 3x4 address at the camera hook
    std::array<float, 3> yaw_axis_at_write{}; // ... its forward then (the view's body)
    bool yaw_at_write_ok{};
    std::array<float, 3> player_t_at_write{}; // test probe: the player's feet at the camera hook
    s64 cam_probe_logged_ns{};
    u64 cannon_measure_hits{};           // test measurement M4 (the drawn cannon)
    u64 render_cam_writes_seen{};        // camera writes at the last Render entry
    std::array<u32, 3> render_ticks{};   // game ticks (camera writes) per drawn frame this second: 0, 1, 2+
    // Build 48: the left arm's draw window and the rig's order probe (one frame's events a second).
    bool left_window{};
    u32 left_window_solves{};             // left-arm solves inside / outside the window since the last probe line
    u32 left_other_solves{};
    struct OrderEvent {
        char kind{};                       // P PreRender, R Render entry, r rain draw, C closed, S solve in the window, s out
        u32 caller{};                      // solves: the returns of the first three frame records (main offsets): Solve's,
        u32 caller2{};                     // BuildPose's, and the one that tells the paths apart (0xc7f12c PreRender,
        u32 caller3{};                     // 0xfa7e90 / 0xfa6d38 Render)
        u64 cam_writes{};
    };
    std::array<OrderEvent, 32> order_ev{};
    // Every session, 1 s: window opens by path (PreRender, Render, rain), closes, the left-arm solves inside / outside,
    // and camera hooks that ran with the window open (a window left open: its tick's solves get the IK).
    std::array<u32, 3> lw_opens{};
    u32 lw_closes{};
    u32 lw_solves_in{};
    u32 lw_solves_out{};
    u32 lw_open_at_camera{};
    // The arm's state probe: camera hooks seen, those with the arm shown (flags bit0), entries into the
    // shown state, the last flags / state / player words, and every state seen (bit per state below 32).
    u32 la_ticks{};
    u32 la_shown_ticks{};
    u32 la_entries{};
    u32 la_flags_last{};
    u32 la_state_last{};
    u32 la_state_prev{0xffffffffu};
    u32 la_states_seen{};
    std::array<u32, 4> la_player_last{};
    std::array<std::array<float, 12>, 4> la_device{};   // the device parts' locals (and more) at the last Render entry
    std::array<u32, 4> la_words{};                      // arm_words at the last camera hook
    u32 la_device_n{};
    s64 la_owned{-1};                                   // the beam-owned word at the last Render entry (-1: unread)
    u32 la_device_writes{};                             // Render entries that wrote the device locals (this second)
    bool dev_solve_pending{};                           // our window solve wrote the left pose; the compose hook captures
    bool dev_elbow_valid{};
    u32 dev_renders_since_capture{};                    // Render entries since the capture (1 = the previous drawn frame)
    u64 dev_elbow_char{};                               // the left char the capture was read from
    std::array<float, 12> dev_elbow_abs{};              // L_elbow's abs right after our window solve's abs program
    std::array<std::array<float, 10>, 4> dev_part_rel{}; // the written parts' locator rels (CCoords) at that moment
    u32 dev_captures{};                                 // captures this second
    bool la_device_ours{};                              // the logged locals are ours (after the write), not the game's
    std::string la_elbow_space;                         // rig: the drawn parts in the live L_elbow's frame (A/B with the game)
    std::array<u32, 3> dev_nodeid{0xffffffffu, 0xffffffffu, 0xffffffffu}; // rig: the game's GetNodeId per part (105.7)
    std::array<u32, 3> dev_nodeid_hits{};
    std::array<std::array<float, 9>, 4> dev_bind_m{};   // each written part's legacy m (rotation rows)
    std::array<bool, 4> dev_bind_ok{};
    bool dev_bind_logged{};
    // The rig's decisive read: the abs-program run count (pose+0x22) and pose+4 at the capture and at
    // the Render entry, the op's parent / dst bases against bank0 + 8, the live +0xc8's columns, and
    // inv(live +0xc8) x live grapLocator slot against grapLocator's rel (one Solve composed both when they agree).
    u32 dev_cap_runs{}, dev_cap_pose4{};
    u64 dev_cap_parent_base{}, dev_cap_dst_base{}, dev_cap_bank0{};
    std::string dev_render_read;
    u32 la_device_skips{};                              // ... that left them to the game (grappling, or no pose)
    u32 order_n{};
    bool order_rec{};
    s64 order_last_ns{};
    double cam_shift_sum_cm{};
    u64 camera_writes{};                 // camera_world writes (first eye), for the cannon's staleness line
    u64 gun_cam_writes_seen{};           // ... at the last gun write
    std::array<u32, 3> gun_cam_between{}; // camera writes between gun writes this second: 0, 1, 2+
    float gun_cam_age_max_ms{};
    double gun_cam_age_sum_ms{};
    u32 gun_cam_samples{};
    s64 gun_cam_logged_ns{};
    std::array<float, 3> hand_world{};
    std::array<std::array<float, 3>, 3> hand_axes{}; // world: left, up, forward of the hand written last
    bool hand_world_valid{};
    u64 cursor_ptr{};          // M6 [aim]: the aiming cursor object seen at the ray hook (this tick or the last)
    bool ray_seen{};           // the ray hook ran since the last camera hook
    u32 scan_state_mask{};     // [aim] scan_state's bit (the ray stays the head's while it holds)
    u32 fade_mask{};           // [fade] states' bits
    std::array<std::vector<std::pair<u64, std::array<float, 3>>>, VIZR::HudMaxElements> corners_saved; // per element
    bool corners_logged{};
    std::array<float, 12> game_camera{}; // The game's own camera before our write (row-major 3x4)
    bool game_camera_valid{};
    // Cut fades
    const Camera* cut_camera{};             // the camera rule that matched last
    std::optional<u64> cut_value;           // its cut_field last tick
    std::array<float, 12> cut_prev{};       // its game matrix last tick (jump guard)
    bool cut_prev_valid{};
    // The comfort vignette's motion tracking (the game camera's own matrix, not the head).
    std::array<float, 12> vig_prev{};
    bool vig_prev_valid{};
    s64 vig_prev_ns{};
    float vig_level{};
    u32 grapple_logged_game{0xfffffffe}; // Test log once per change of the camera candidate
    u32 points_area_logged{0xfffffffe};  // [grapple_aim.points]: the player's area at the last "grapple points" line
    u32 points_pick_logged{0xfffffffe};  // ... the last pick logged
    s64 points_resolve_logged_ns{};      // ... the last "does not resolve" line (once a second at most)
    u32 scan_pick_logged{0xfffffffe};    // Test log once per change of the head's scan pick
    std::array<float, 3> scan_pick_pos{}; // The head pick's world position (the brackets go there)
    bool scan_pick_valid{};
    float bracket_max_deg{};             // test: the widest bracket angle this second
    s64 bracket_logged_ns{};
    u32 scan_list_logged_count{0xffffffff};
    u32 scan_list_logs{};
    std::vector<u32> scan_ids_logged;    // test: ids already logged by the scan pick
    s64 scan_state_logged_ns{};          // test: the once-a-second lock / scan state line
    u32 scan_orbit{};                    // test: the orbit state (PL+0x4e0) at the last scan pick
    float body_yaw_deg{};                // test: the body (yaw source) forward's yaw at the last camera write
    bool body_yaw_valid{};
    s64 scan_yaw_logged_ns{};
    s64 grapple_zl_logged_ns{};          // The [grapple_aim.zl_log] line, twice a second at most
    // Who supplied PL+candidate_off at the last RunGrappleAim (the "grapple zl: press" line at each ZL press edge).
    const char* grapple_source{};
    std::string_view grapple_source_hook;
    u32 grapple_source_game{};           // the game's own candidate at that hook
    s64 grapple_source_ns{};
    s64 grapple_press_logged_ns{};
    u64 hint_hits{}, hint_ball_hits{};   // [hint_camera] hits (any camera, the hint camera in ball form)
    s64 hint_logged_ns{};
    // The scan lens rewrite. The last transform written (the four queue calls share it)
    // and the hits by caller (which call sites reach the hook; the last bucket = any other caller).
    std::array<float, 12> lens_last{};
    u64 lens_last_at{};
    std::array<u64, 5> lens_hits{};
    u64 lens_writes{};
    bool lens_write{true};               // VIZR_LENS_REWRITE=0: count only (the diagnostic)
    int lens_mode{3};                    // test knob (VIZR_LENS_MODE): 0 line, 1 rigid, 2 ray, 3 head
    std::array<float, 3> head_room{}; // The head's capped room offset (recentered frame, m)
    bool head_room_valid{};
    std::array<float, 2> body_plant{}; // Where the feet stand (recentered frame x, z; m)
    float crouch_alpha{};               // The hip flex of the crouch fold (rad), for RunLegs
    std::array<std::array<float, 3>, 4> root_base{}; // The animation's root translation (Body size)
    std::array<std::array<float, 3>, 4> root_last{};
    std::array<bool, 4> root_base_valid{};
    s64 head_room_log_ns{};
    s64 lean_log_ns{};
    s64 body_log_ns{};
    bool body_camera{}; // A [arms] body_cameras rule matched this tick (the body's head is the view)
    // The gait (camera tick), the legs' animation base per leg node (no compounding, as the arm)
    std::array<float, 3> feet_prev{};
    s64 feet_prev_ns{};
    bool feet_prev_valid{};
    float gait_speed{};      // world units/s, smoothed
    float gait_forward{1.0f}; // body-space direction of travel (forward, side), unit
    float gait_side{};
    float gait_phase{};
    bool gait_air{};
    s64 gait_log_ns{};
    std::array<std::array<float, 4>, 6> leg_base{};
    std::array<std::array<float, 4>, 6> leg_last{};
    std::array<bool, 6> leg_base_valid{};
    u64 cut_fades{};
    std::array<s64, VIZR::HudMaxElements> hud_extent_ns{};
    u64 pitch_logs{};
    std::string camera_logged_id;
    s64 camera_log_ns{};
    s64 modec_log_ns{};
    std::vector<std::pair<std::string, u32>> rig_writes; // VIZR_RIG_WRITES (test runs only)
    float rig_cam_back{}; // VIZR_RIG_CAM_BACK (test runs only), world units behind the body view
    bool rig_writes_logged{};
    std::array<float, 12> gun_xf{};    // The cannon model's world transform written this frame
    std::array<float, 3> shoulder_world{}; // ... and the body's shoulder point
    // The left arm's two-bone solve: the body's yaw frame and left shoulder (set with shoulder_world), the
    // arm's character and the transform we wrote at its render entry, and the animation's base coords.
    // The HUD pass's eye for the GUI camera hooks (render thread; set by RunModeC), and the
    // world camera's vertical FOV as written (the GUI projection matches it).
    int gui_eye{0};   // -1 = eye L, +1 = eye R, 0 = not in the HUD pass of an eye
    float gui_shift_scale{1.0f}; // World / game horizontal projection scale (the eye shift on screen)
    float gui_vfov{};
    u64 gui_writes{};
    std::array<float, 3> shoulder_left_world{};
    std::array<float, 3> body_right{}, body_up{}, body_fwd{};
    std::array<float, 3> neck_right{}, neck_fwd{}; // The left arm's body frame (the neck, not the head)
    std::array<float, 3> body_axis{};    // The game body's forward column (the player yaw field), world
    s64 body_axis_ns{};
    s64 neck_ns{};                       // the neck yaw's low-pass state (neck_fwd)
    bool neck_from_body{};               // The last neck came from the body yaw (not the hands fallback)
    std::array<float, 3> body_pos{};     // test: the body transform's translation, for the locomotion check
    s64 release_until_ns{}, release_logged_ns{}; // The scan-lock release log window
    float torso_applied{};               // The torso yaw (rad, LOCAL, left +) the game body has been turned by
    float torso_view{};                  // rig cross-check: the part the game camera's yaw changes have shown
    std::array<float, 3> gcam_prev{};    // the game camera's flat forward at the last (first-eye) camera hook
    bool gcam_prev_valid{};
    u32 steer_frame_lines{};             // rig: the per-frame steer lines written
    float lock_view{};                   // The game's own body turn during a lock (LOCAL, left +): the view subtracts it
    bool lock_return{};                  // After a lock, in the modes the torso does not drive: the body turns back
    float lock_return_target{};          // ... to this torso_applied (the value before the lock's turn was handed in)
    bool locked{};                       // [orbit_head] lock_field was 1..4 at the last camera hook
    std::array<float, 3> lock_axis0{};   // the body's flat forward when the lock began
    std::array<float, 3> lock_axis_prev{}; // ... and at the last camera hook without a lock
    bool lock_axis_prev_valid{};
    s64 lock_axis_prev_ns{};             // ... when (a start older than HeadFreshNs spans a gap: not used)
    bool orbit_head_ready{};             // the [orbit_head] buffer holds this frame's head camera (a body camera)
    bool orbit_facing_valid{};           // the [orbit_head] facing_buffer's flag as last written
    s64 ring_logged_ns{};
    float ring_depth{};                  // test log: the lock ring's last depth (units from the head)
    s64 head_reticle_logged_ns{};        // test log: the scan reticle's depth line
    s64 zl_last_ns{};                    // the last held-ZL hook call (a gap = a new press)
    u32 tick_hz{};                       // [tick_rate]: the rate written (0 = none yet; the original bytes)
    bool tick_failed{};                  // the sites did not hold the expected bytes: never written
    u32 tick_capped_logged{};            // the offered rate last logged as capped
    u32 pose_lag_default{};              // the first view camera's pose_lag_frames ...
    const std::map<u32, u32>* pose_lag_at{}; // ... and its per-tick-rate table (pose_lag_frames_at)
    u32 zl_followup{};                   // ticks until the after-press line
    std::vector<u64> zl_trace_last;      // [grapple_aim.zl_trace]: each entry's last logged value
    s64 zl_trace_until{};                // the open trace window (every call logs until then)
    u32 zl_trace_lines{};
    s64 view_step_logged_ns{};           // rig: the view-step line once a second
    std::array<float, 3> move_prev_pos{};  // rig: the game camera's position at the last first-eye camera hook
    s64 move_prev_ns{};
    u32 move_lines{};
    s64 move_until_ns{};                 // the movement log runs while the stick is held and 2 s after
    float view_prev_yaw{}, head_prev_yaw{}, view_step_max{};
    u32 view_still_frames{};
    bool view_prev_valid{};
    u64 orbit_table{}, orbit_player{};   // the entity table and player of the last grapple-aim call (the lock ring)
    u32 torso_recenter_count{};
    s64 torso_ns{}, torso_logged_ns{};
    bool last_camera_right_eye{};        // AFR: the last camera write was the right eye (the torso steps on the left)
    std::array<float, 3> move_logged_pos{};
    s64 move_logged_ns{};
    s64 move_frame_logged_ns{};          // the once-a-second "move frame" line (every session)
    s64 anchor_logged_ns{};
    std::array<float, 3> m2_shoulder{};
    u64 left_char{};
    std::array<float, 12> left_xf{};
    bool left_xf_valid{};
    s64 beam_check_ns{};                 // rig: the last beam frame vs drawn arm line
    u64 states_held_set{};               // rig: set state bits kept at a hook that cannot read their fields
    s64 states_held_logged_ns{};
    std::array<float, 12> beam_frame{};  // rig: base x local as written at the grapple arm's UpdateState this tick
    bool beam_frame_valid{};
    std::array<float, 3> lik_shoulder_pose{}; // the live shoulder (pose space), for the next placement
    std::array<float, 3> lik_base_tw{}, lik_last_tw{}; // The wrist's original / last-held translation
    std::array<std::array<float, 3>, 3> lik_intended{}; // test measurement: the solve's shoulder / elbow / wrist (model space)
    bool lik_intended_valid{};
    u64 measure_hits{}, render_measure_hits{};
    u64 left_buildpose_hits{}, lik_solves_logged{};
    u64 left_arm_ptr{};                  // test measurement M3: the left arm object (render entry)
    std::array<float, 3> left_S{1.0f, 1.0f, 1.0f}; // The arm's draw scale (arm + 0x28)
    float lik_model_reach{};             // the arm's two bones (model units), from the last solve
    std::array<float, 3> lik_want_fwd_world{}, lik_want_back_world{}; // test: the hand's wanted axes (world)
    std::array<float, 9> m2_wrist_rot{}; // test: M2's wrist skin rotation (model space, row-major 3x3)
    std::array<float, 3> m2_elbow{};
    bool lik_index_logged{};
    std::array<float, 3> m2_wrist{};     // M2's last skinned wrist (model space)
    bool m2_wrist_valid{};
    std::array<float, 3> lik_target_world{}; // the solve's wrist target in the world
    u64 draw_measure_hits{};
    bool lik_tw_valid{};
    bool lik_shoulder_valid{};
    std::array<float, 10> lik_base_s{}, lik_base_e{};
    std::array<float, 4> lik_last_s{}, lik_last_e{};
    bool lik_base_valid{};
    u64 lik_solves{};
    bool gun_xf_valid{};
    u64 ik_solves{};
    u64 dial_calls{};
    int dial_slot{-1};          // The previewed slot the override last saw
    u32 dial_frames_left{};
    u64 ik_skips{};
    std::array<float, 4> ik_last_elbow{};   // the elbow rotation the solve wrote last
    std::array<float, 4> ik_last_abs{1.0f};
    std::array<float, 10> ik_base_e{};      // the animation's own elbow / cannon coords (the cannon pose is
    std::array<float, 10> ik_base_g{};      // rebuilt from the animation only now and then; between, it holds ours)
    bool ik_base_valid{};
    std::array<u64, 3> arm_chars{}; // [arms]: the characters to log (read at the camera hook)
    std::array<u64, 3> arm_hits{};
    u64 gun_logs{};
    u64 left_arm_writes{};
    u64 ray_samples{};
    std::vector<u64> reticle_lrs_seen; // diagnostic: the callers seen at the reticle hook (first 32)
    u32 reticle_logs{};
    bool rig_logs{};           // VIZR_RIG_OUT set: test-only logs (cursor distance 6/s, snap results)
    bool rig_anchor_applied{}; // VIZR_RIG_ANCHOR_APPLIED (test runs only): the shoulder faces the applied body
    u64 cursor_calls{};
    int snap_check_frames{};   // camera-hook frames until the snap result is logged
    float snap_psi_before{};
    u64 snaps{};
    u64 hand_writes{};
    u64 fire_logs{};
    s64 hook_lines_ns{};              // the last per-hook counts line (players: every 10 s)
    std::atomic<u64> writes{0};
    std::atomic<s64> last_log_ns{0};
    std::atomic<bool> camera_type_logged{false};
    std::atomic<bool> afr_refused_logged{false};
    std::mutex hook_mutex; // held by every hook callback (the threading contract above)
    SettingsSaver saver;   // last member: its thread stops before the runtime's state goes
};

std::atomic<Runtime*> g_runtime{nullptr};

void InstallImpl(Core::System& system, Loader::AppLoader& loader);

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string Hex(std::span<const u8> bytes) {
    std::string out;
    for (const u8 b : bytes) {
        out += fmt::format("{:02X} ", b);
    }
    if (!out.empty()) {
        out.pop_back();
    }
    return out;
}

// ---- symbol resolution ---------------------------------------------------

/// Counts and locates the given mangled names in a module's .dynsym. Our own pass, because
/// Core::Symbols keeps one entry per name and cannot report duplicates.
std::map<std::string, std::vector<u64>, std::less<>> FindSymbols(Core::Memory::Memory& memory,
                                                                 u64 base,
                                                                 const std::vector<std::string>& names) {
    using namespace Common::ELF;
    std::map<std::string, std::vector<u64>, std::less<>> found;
    for (const auto& name : names) {
        found[name];
    }
    if (names.empty()) {
        return found;
    }
    // Every read is range-checked and every failure says why (else the hooks report only "symbol not found").
    const auto fail = [&](std::string_view why) {
        LOG_WARNING(Core, "VIZR hooks: symbols of the module at {:#x} not readable: {}", base, why);
        return found;
    };
    const auto ok = [&](u64 at, u64 size) { return memory.IsValidVirtualAddressRange(at, size); };
    if (!ok(base + 4, 4)) {
        return fail("no module header");
    }
    const u32 mod_offset = memory.Read32(base + 4);
    const u64 mod0 = base + mod_offset;
    if (!ok(mod0, 8) || memory.Read32(mod0) != Common::MakeMagic('M', 'O', 'D', '0')) {
        return fail("no MOD0 header");
    }
    u64 strtab = 0;
    u64 symtab = 0;
    u64 strsz = 0;
    u64 syment = sizeof(Elf64_Sym);
    u64 dyn = mod0 + memory.Read32(mod0 + 4);
    for (int n = 0;; ++n, dyn += sizeof(Elf64_Dyn)) {
        if (n == 1024 || !ok(dyn, sizeof(Elf64_Dyn))) {
            return fail("the dynamic table has no end inside mapped memory");
        }
        Elf64_Dyn entry{};
        memory.ReadBlock(dyn, &entry, sizeof(entry));
        if (entry.d_tag == ElfDtNull) {
            break;
        }
        if (entry.d_tag == ElfDtStrtab) {
            strtab = entry.d_un.d_ptr;
        } else if (entry.d_tag == ElfDtSymtab) {
            symtab = entry.d_un.d_ptr;
        } else if (entry.d_tag == ElfDtStrsz) {
            strsz = entry.d_un.d_val;
        } else if (entry.d_tag == ElfDtSyment) {
            syment = entry.d_un.d_val;
        }
    }
    if (strtab == 0 || symtab == 0 || strsz == 0 || syment < sizeof(Elf64_Sym) || symtab >= strtab) {
        return fail("no usable symbol or string table");
    }
    if (strsz > 16 * 1024 * 1024 || !ok(base + strtab, strsz) || !ok(base + symtab, strtab - symtab)) {
        return fail(fmt::format("string table {:#x} bytes / symbol table not mapped", strsz));
    }
    std::vector<char> strings(strsz);
    memory.ReadBlock(base + strtab, strings.data(), strings.size());
    for (u64 at = symtab; at + sizeof(Elf64_Sym) <= strtab; at += syment) {
        Elf64_Sym sym{};
        memory.ReadBlock(base + at, &sym, sizeof(sym));
        if (sym.st_value == 0 || sym.st_name >= strsz) {
            continue; // imports and junk
        }
        const char* name = strings.data() + sym.st_name;
        const std::string_view name_view{name, ::strnlen(name, strsz - sym.st_name)};
        if (const auto it = found.find(name_view); it != found.end()) {
            it->second.push_back(base + sym.st_value);
        }
    }
    return found;
}

// ---- field access at a hook ---------------------------------------------------------------------

struct HitContext {
    Runtime& rt;
    HookEntry& hook;
    const RegReader& regs;
    Core::Memory::Memory& memory;
};

FieldState* StateOf(Runtime& rt, const Field* field) {
    for (auto& state : rt.fields) {
        if (state->field == field) {
            return state.get();
        }
    }
    return nullptr;
}

/// Address of the field for this hit; empty when it belongs to another hook or a link is null.
std::optional<u64> FieldAddress(const HitContext& ctx, FieldState& state) {
    const Field& f = *state.field;
    if (!f.hook.empty() && f.hook != ctx.hook.name) {
        return std::nullopt;
    }
    u64 address = 0;
    switch (f.root) {
    case RootKind::Reg:
        address = ctx.regs.x(ctx.regs.ctx, f.reg);
        break;
    case RootKind::Sp:
        address = ctx.regs.sp(ctx.regs.ctx);
        break;
    case RootKind::Static:
    case RootKind::Sym:
        address = state.root_address;
        break;
    }
    // Each chain step: add the offset, then dereference (address-file chain rule).
    for (const s64 step : f.chain) {
        address += static_cast<u64>(step);
        if (address == 0 || !ctx.memory.IsValidVirtualAddressRange(address, 8)) {
            address = 0;
            break;
        }
        address = ctx.memory.Read64(address);
        if (address == 0) {
            break;
        }
    }
    if (address == 0) {
        if (!state.null_logged.exchange(true)) {
            LOG_WARNING(Core, "VIZR hooks: field '{}' has a null link at hook '{}' (skipped; logged once)",
                        f.name, ctx.hook.name);
        }
        return std::nullopt;
    }
    address += static_cast<u64>(f.off);
    if (!ctx.memory.IsValidVirtualAddressRange(address, FieldSize(f.type))) {
        if (!state.null_logged.exchange(true)) {
            LOG_WARNING(Core, "VIZR hooks: field '{}' points at unmapped memory {:#x} (skipped; logged once)",
                        f.name, address);
        }
        return std::nullopt;
    }
    return address;
}

/// Scalar value for `when` (integers zero-extended, f32 as raw bits).
std::optional<u64> ReadScalar(const HitContext& ctx, std::string_view name) {
    const Field* field = ctx.rt.profile.FindField(name);
    FieldState* state = field ? StateOf(ctx.rt, field) : nullptr;
    if (state == nullptr) {
        return std::nullopt;
    }
    const auto address = FieldAddress(ctx, *state);
    if (!address) {
        return std::nullopt;
    }
    switch (field->type) {
    case FieldType::U8:
        return ctx.memory.Read8(*address);
    case FieldType::U16:
        return ctx.memory.Read16(*address);
    case FieldType::U32:
    case FieldType::F32:
        return ctx.memory.Read32(*address);
    case FieldType::U64:
    case FieldType::Ptr:
        return ctx.memory.Read64(*address);
    case FieldType::Vec3f:
    case FieldType::Quat:
    case FieldType::Mat3x4:
    case FieldType::Mat4:
        return std::nullopt; // not comparable
    case FieldType::Addr:
        // An address-only field (the object list is inline in its parent): reading a value here is the build-26/33
        // bug (one dereference too many). Fail loudly, once.
        if (!state->addr_read_logged) {
            state->addr_read_logged = true;
            LOG_ERROR(Core, "VIZR hooks: field '{}' is address-only (type addr): a value read is a profile bug", name);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

/// A `when` whose field cannot be read is false.
bool Eval(const HitContext& ctx, const When& when) {
    switch (when.op) {
    case When::Op::Always:
        return true;
    case When::Op::Not:
        return !Eval(ctx, when.children[0]);
    case When::Op::AllOf:
        return std::all_of(when.children.begin(), when.children.end(),
                           [&](const When& c) { return Eval(ctx, c); });
    case When::Op::AnyOf:
        return std::any_of(when.children.begin(), when.children.end(),
                           [&](const When& c) { return Eval(ctx, c); });
    case When::Op::State: // format v0.2: the named state's latest published bit
        return ((g_input_states.load(std::memory_order_relaxed) >> when.state_index) & 1u) != 0;
    case When::Op::Equals:
    case When::Op::In:
    case When::Op::Mask:
    case When::Op::EqualsAddr:
    case When::Op::EqualsField:
    case When::Op::IsNull:
        break;
    }
    const auto value = ReadScalar(ctx, when.field);
    if (!value) {
        return false;
    }
    switch (when.op) {
    case When::Op::Equals:
        return *value == when.value;
    case When::Op::In:
        return std::find(when.values.begin(), when.values.end(), *value) != when.values.end();
    case When::Op::Mask:
        return ((*value & when.mask) != 0) == when.set;
    case When::Op::EqualsAddr: {
        const auto it = ctx.rt.modules.find(when.addr_module);
        return it != ctx.rt.modules.end() && *value == it->second.first + when.addr_offset;
    }
    case When::Op::EqualsField: {
        const auto other = ReadScalar(ctx, when.other_field);
        return other && *other == *value;
    }
    case When::Op::IsNull:
        return *value == 0;
    case When::Op::Always:
    case When::Op::Not:
    case When::Op::AllOf:
    case When::Op::AnyOf:
    case When::Op::State:
        break;
    }
    return false;
}

constexpr s64 HeadFreshNs = 100'000'000; // older head pose = no head

std::array<float, 3> Column(const Mat3& m, std::size_t c) {
    return {m[0][c], m[1][c], m[2][c]};
}

float Dot(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/// [orbit_head] facing_buffer: a 3x4 (row-major) whose column 2 is the head camera's forward, flattened about the
/// world up, and a u32 at +0x30: 1 while it holds this frame's head, 0 without a body camera (the game's own facing
/// then). Read by a patch in place of the body's transform where the game tests the body's facing. Written only on a
/// change of the flag or with a valid head. While the lock target (facing_target_off) is a grapple point the flag is 0:
/// the facing test then runs while the arm extends, and a head turned away would break the grapple.
u64 SlotSpan(const Profile::GrappleAim& g);
void WriteOrbitFacing(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& oh = rt.profile.orbit_head;
    if (!oh.facing) {
        return;
    }
    const auto module = rt.modules.find(oh.facing_module);
    const u64 at = module != rt.modules.end() ? module->second.first + oh.facing_offset : 0;
    if (at == 0 || !ctx.memory.IsValidVirtualAddressRange(at, 0x34)) {
        return;
    }
    const auto& c = rt.camera_world;
    const auto up = Column(rt.basis, 1);
    std::array<float, 3> f{c[2], c[6], c[10]};
    const float along = Dot(f, up);
    for (std::size_t i = 0; i < 3; ++i) {
        f[i] -= along * up[i];
    }
    const float n = std::sqrt(Dot(f, f));
    bool grapple_lock = false;
    if (const auto& g = rt.profile.grapple_aim; oh.facing_target_off != 0 && g.present && rt.orbit_player != 0 &&
                                                rt.orbit_table != 0) {
        auto& mem = ctx.memory;
        const auto read64 = [&](u64 a) -> u64 { return mem.IsValidVirtualAddressRange(a, 8) ? mem.Read64(a) : 0; };
        const auto vt_module = rt.modules.find(g.vtable_module);
        if (vt_module != rt.modules.end() && mem.IsValidVirtualAddressRange(rt.orbit_player + oh.facing_target_off, 4)) {
            const u32 id = mem.Read32(rt.orbit_player + oh.facing_target_off);
            const u64 e = read64(rt.orbit_table + (id & g.id_index_mask) * g.stride);
            if (e != 0 && mem.IsValidVirtualAddressRange(e, SlotSpan(g)) && mem.Read32(e + g.id_check_off) == id) {
                const u64 object = read64(e + g.entity_off);
                grapple_lock = object != 0 && read64(object) == vt_module->second.first + g.vtable_offset;
            }
        }
    }
    const bool valid = rt.orbit_head_ready && rt.camera_world_valid && n > 1e-3f && !grapple_lock;
    if (valid) {
        for (float& v : f) {
            v /= n;
        }
        const std::array<float, 3> l{up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0]};
        const std::array<float, 12> m{l[0], up[0], f[0], c[3], l[1], up[1], f[1], c[7], l[2], up[2], f[2], c[11]};
        ctx.memory.WriteBlock(at, m.data(), sizeof(m));
    }
    if (valid != rt.orbit_facing_valid || valid) {
        ctx.memory.Write32(at + 0x30, valid ? 1u : 0u);
        rt.orbit_facing_valid = valid;
    }
}

Mat3 FromQuat(const std::array<float, 4>& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return Mat3{{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                 {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                 {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}}};
}

/// Rotation by `angle` (rad) about the unit `axis` (Rodrigues).
Mat3 AxisAngle(const std::array<float, 3>& axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    const float x = axis[0], y = axis[1], z = axis[2];
    return Mat3{{{t * x * x + c, t * x * y - s * z, t * x * z + s * y},
                 {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
                 {t * x * z - s * y, t * y * z + s * x, t * z * z + c}}};
}

/// R = Rot_up(psi) * W * R_h * L^T, with W = L = the [space] basis (columns = game
/// vectors for OpenXR right, up, back). psi = the game's yaw about up, from its forward vector.
Mat3 HeadRotation(Runtime& rt, const Camera& camera, const Mat3& game, const HeadPose& head,
                  const Mat3* body = nullptr, const std::array<float, 3>* body_forward = nullptr) {
    const auto up = Column(rt.basis, 1);
    const auto back = Column(rt.basis, 2);
    const std::array<float, 3> f0{-back[0], -back[1], -back[2]}; // game vector for OpenXR forward
    // Head rotation in OpenXR LOCAL, recentered about +Y.
    // The body already turned by the torso's yaw, so the head counts from the torso (view = body + head - torso).
    const Mat3 rh =
        Multiply(AxisAngle({0.0f, 1.0f, 0.0f}, -(head.recenter_yaw + rt.torso_applied + rt.lock_view)), FromQuat(head.orientation));
    Mat3 result = Multiply(Multiply(rt.basis, rh), Transpose(rt.basis));
    if (camera.combine == "game_yaw_plus_head") {
        // Game forward, horizontal part: from the yaw source (e.g. the player body, so camera bob
        // never reaches the head) when the profile has one, else from the camera itself.
        const Mat3& src = body != nullptr ? *body : game;
        const std::array<float, 3>& local_fwd = body_forward != nullptr ? *body_forward : f0;
        std::array<float, 3> f{};
        for (std::size_t i = 0; i < 3; ++i) {
            f[i] = src[i][0] * local_fwd[0] + src[i][1] * local_fwd[1] + src[i][2] * local_fwd[2];
        }
        if (camera.game_look_column >= 0 && body == nullptr) {
            // A camera with its own convention (Prime's ball camera: LookAt, column 2 = look): its yaw from that.
            const auto c = static_cast<std::size_t>(camera.game_look_column);
            f = {game[0][c], game[1][c], game[2][c]};
        }
        const float vertical = Dot(f, up);
        if (std::abs(vertical) < 0.99f) { // nearly vertical: keep the previous yaw
            std::array<float, 3> fh{f[0] - vertical * up[0], f[1] - vertical * up[1], f[2] - vertical * up[2]};
            const float len = std::sqrt(Dot(fh, fh));
            for (auto& v : fh) {
                v /= len;
            }
            const std::array<float, 3> cross{f0[1] * fh[2] - f0[2] * fh[1], f0[2] * fh[0] - f0[0] * fh[2],
                                             f0[0] * fh[1] - f0[1] * fh[0]};
            rt.psi = std::atan2(Dot(cross, up), Dot(f0, fh));
            rt.psi_min = std::min(rt.psi_min, rt.psi);
            rt.psi_max = std::max(rt.psi_max, rt.psi);
        }
        Mat3 pitch = AxisAngle(up, 0.0f);
        if (camera.game_pitch_cap_deg > 0.0f && std::abs(vertical) < 0.99f) {
            // The game's pitch, capped, about the yawed camera's right axis (forward x up). The camera's
            // look direction is its column 2 (it read -0.26 vertical looking down at the ball while the yaw vector
            // above read +0.45), so the pitch comes from column 2, not from the yaw vector.
            const float cap = camera.game_pitch_cap_deg * std::numbers::pi_v<float> / 180.0f;
            const std::size_t lc = camera.game_look_column >= 0 ? static_cast<std::size_t>(camera.game_look_column) : 2;
            const std::array<float, 3> look{game[0][lc], game[1][lc], game[2][lc]};
            const float look_v = Dot(look, up) / std::max(1e-6f, std::sqrt(Dot(look, look)));
            const float angle = std::clamp(std::asin(std::clamp(look_v, -1.0f, 1.0f)), -cap, cap);
            const std::array<float, 3> right{f0[1] * up[2] - f0[2] * up[1], f0[2] * up[0] - f0[0] * up[2],
                                             f0[0] * up[1] - f0[1] * up[0]};
            pitch = AxisAngle(right, angle);
            if (rt.rig_logs && ++rt.pitch_logs % 60 == 1) {
                LOG_INFO(Core, "VIZR hooks: camera '{}' game forward ({:.2f},{:.2f},{:.2f}) look vertical {:.2f} -> pitch {:.1f} deg "
                         "(cap {:.0f}), game cols ({:.2f},{:.2f},{:.2f}) ({:.2f},{:.2f},{:.2f}) ({:.2f},{:.2f},{:.2f})",
                         camera.id, f[0], f[1], f[2], look_v, angle * 57.29578f, camera.game_pitch_cap_deg,
                         game[0][0], game[1][0], game[2][0], game[0][1], game[1][1], game[2][1], game[0][2], game[1][2],
                         game[2][2]);
            }
        }
        result = Multiply(AxisAngle(up, rt.psi), Multiply(pitch, result));
    }
    return result;
}

/// Reads the rotation part of a mat3x4 / mat4 field as camera-to-world (rows = game axes).
struct MatrixIo {
    std::array<float, 16> raw{};
    u32 count{};
    bool row_major{};
    u32 stride{}; // floats per row (row-major) or per column (column-major)

    float& At(std::size_t row, std::size_t col) {
        return row_major ? raw[row * stride + col] : raw[col * stride + row];
    }
};

/// A [comfort] setting by its user.toml key (0 if unknown; profile load already checked the names).
float ComfortSetting(std::string_view key) {
    UserSettings settings = g_user_settings.Load();
    for (const auto& spec : Settings::Table) {
        if (spec.scope == Settings::Scope::Comfort && key == spec.key) {
            return *spec.field(settings, 0);
        }
    }
    return 0.0f;
}

constexpr float VignetteFullDegPerS = 120.0f; // A turn this fast = the full tunnel
constexpr float VignetteFullMPerS = 4.0f;     // the same for travel (the rolling ball, a camera dolly)
constexpr float VignetteCutDeg = 25.0f;       // a bigger turn in one tick is a cut or a snap, not motion

/// The tunnel's strength from the game camera's own
/// turn rate and travel speed (not the head: the player's own motion never darkens the view), rising in ~0.15 s and
/// fading in ~0.5 s, times the camera's vignette setting. Cuts and snaps (> 25 deg in one tick) count as no motion.
void UpdateVignette(Runtime& rt, const Camera& camera) {
    const float want = camera.vignette_setting.empty() ? 0.0f : ComfortSetting(camera.vignette_setting);
    const s64 now = SteadyNowNs();
    const float dt = rt.vig_prev_valid ? std::clamp(static_cast<float>(now - rt.vig_prev_ns) * 1e-9f, 0.0f, 0.25f) : 0.0f;
    float motion = 0.0f;
    if (want > 0.0f && rt.game_camera_valid && rt.vig_prev_valid && dt > 0.001f) {
        const auto& a = rt.vig_prev;
        const auto& b = rt.game_camera;
        const float dx = b[3] - a[3], dy = b[7] - a[7], dz = b[11] - a[11];
        const float metres = std::sqrt(dx * dx + dy * dy + dz * dz) / std::max(rt.space.units_per_meter, 1e-3f);
        float trace = 0.0f;
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                trace += a[i * 4 + j] * b[i * 4 + j];
            }
        }
        const float deg = std::acos(std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.2957795f;
        if (deg <= VignetteCutDeg) {
            motion = std::clamp(std::max(deg / dt / VignetteFullDegPerS, metres / dt / VignetteFullMPerS), 0.0f, 1.0f);
        }
    }
    const float k = std::min(1.0f, dt / (motion > rt.vig_level ? 0.15f : 0.5f));
    rt.vig_level += (motion - rt.vig_level) * k;
    rt.vig_prev = rt.game_camera;
    rt.vig_prev_valid = rt.game_camera_valid;
    rt.vig_prev_ns = now;
    g_vignette.store(want * rt.vig_level, std::memory_order_relaxed);
}

/// Black over a camera cut for `ms` (the presenter skips the game layer until then).
void CutFade(Runtime& rt, u32 ms, std::string_view why) {
    const s64 until = SteadyNowNs() + static_cast<s64>(ms) * 1'000'000;
    if (until > g_fade_until_ns.load(std::memory_order_relaxed)) {
        g_fade_until_ns.store(until, std::memory_order_relaxed);
    }
    ++rt.cut_fades;
    LOG_INFO(Core, "VIZR hooks: cut fade {} ms ({}, #{})", ms, why, rt.cut_fades);
}

/// A camera rule matched this tick: fade on a change of rule (into or out of a cutscene) or of its
/// cut_field (a new shot's camera).
void NoteCameraForCuts(const HitContext& ctx, const Camera& camera) {
    Runtime& rt = ctx.rt;
    if (rt.cut_camera != &camera) {
        const u32 ms = std::max(camera.cut_fade_ms, rt.cut_camera ? rt.cut_camera->cut_fade_ms : 0u);
        if (rt.cut_camera != nullptr && ms > 0) {
            CutFade(rt, ms, fmt::format("'{}' -> '{}'", rt.cut_camera->id, camera.id));
        }
        rt.cut_camera = &camera;
        rt.cut_value.reset();
        rt.cut_prev_valid = false;
    }
    if (camera.cut_fade_ms == 0 || camera.cut_field.empty()) {
        return;
    }
    const auto value = ReadScalar(ctx, camera.cut_field);
    if (value && rt.cut_value && *value != *rt.cut_value) {
        CutFade(rt, camera.cut_fade_ms, "shot");
        rt.cut_prev_valid = false; // the new shot's first matrix is not a jump
    }
    if (value) {
        rt.cut_value = value;
    }
}

void RunLockView(const HitContext& ctx);
void UpdateArmAnchors(Runtime& rt, const HandPose& hand);

/// [camera_now]: the game camera's world position from the camera manager `manager` (nothing while a skip count is
/// set: a cinematic camera, which the host cannot resolve).
std::optional<std::array<float, 3>> CameraNowTranslation(const HitContext& ctx, u64 manager) {
    const auto& cn = ctx.rt.profile.camera_now;
    auto& mem = ctx.memory;
    if (manager == 0) {
        return std::nullopt;
    }
    for (const u64 off : cn.skip_counts) {
        if (!mem.IsValidVirtualAddressRange(manager + off, 4) || mem.Read32(manager + off) != 0) {
            return std::nullopt;
        }
    }
    u64 at = mem.IsValidVirtualAddressRange(manager + cn.camera_off, 8) ? mem.Read64(manager + cn.camera_off) : 0;
    for (const u64 step : cn.chain) { // each step a pointer read (CActorMP1::GetTransform: the 3x4 is at the end)
        at = at != 0 && mem.IsValidVirtualAddressRange(at + step, 8) ? mem.Read64(at + step) : 0;
    }
    if (at == 0 || !mem.IsValidVirtualAddressRange(at, 48)) {
        return std::nullopt;
    }
    std::array<float, 12> xf{};
    mem.ReadBlock(at, xf.data(), sizeof(xf));
    std::array<float, 3> t{xf[3], xf[7], xf[11]};
    if (cn.shake_off >= 0 && mem.IsValidVirtualAddressRange(manager + static_cast<u64>(cn.shake_off), 12)) {
        std::array<float, 3> sh{};
        mem.ReadBlock(manager + static_cast<u64>(cn.shake_off), sh.data(), sizeof(sh));
        for (std::size_t i = 0; i < 3; ++i) {
            t[i] += xf[i * 4 + 0] * sh[0] + xf[i * 4 + 1] * sh[1] + xf[i * 4 + 2] * sh[2];
        }
    }
    if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2])) {
        return std::nullopt;
    }
    return t;
}

/// [camera_now] at the hand hook: how far the game camera moved since the camera hook (rt.cam_shift), once per tick.
/// The cannon is placed that much further (RunHand); the shoulder anchors and the left arm keep the camera hook's
/// placement (rig, build 47: moving them made the left arm lead the motion). A failed check at the camera hook, a
/// missing camera or a jump over 2 units (a teleport, a camera change) leaves no shift. The body's turn does not reach
/// the player's transform before this hook (rig: a snap showed 0 deg here), so the turn keeps the camera hook's.
void ShiftCameraToNow(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    if (!rt.profile.camera_now.present || rt.cam_shift_applied || !rt.cam_now_ok || !rt.camera_world_valid) {
        return;
    }
    rt.cam_shift_applied = true;
    if (rt.rig_logs && SteadyNowNs() - rt.cam_probe_logged_ns > 1'000'000'000) {
        // Test probe: what moved since the camera hook, as seen at this (the hand) hook.
        rt.cam_probe_logged_ns = SteadyNowNs();
        auto& m = ctx.memory;
        const float u = std::max(rt.space.units_per_meter * g_user_settings.Load().world_scale, 1e-3f);
        const auto cam = CameraNowTranslation(ctx, rt.cam_now_ptr);
        std::array<float, 12> y{};
        const bool y_ok = rt.yaw_xf_at != 0 && m.IsValidVirtualAddressRange(rt.yaw_xf_at, 48);
        if (y_ok) {
            m.ReadBlock(rt.yaw_xf_at, y.data(), sizeof(y));
        }
        const auto dist = [&](const std::array<float, 3>& a, float x, float yy, float z) {
            return 100.0f * std::sqrt((a[0] - x) * (a[0] - x) + (a[1] - yy) * (a[1] - yy) + (a[2] - z) * (a[2] - z)) / u;
        };
        const u32 lock = rt.orbit_player != 0 && rt.profile.camera_now.lock_state_off >= 0 &&
                                 m.IsValidVirtualAddressRange(rt.orbit_player + static_cast<u64>(rt.profile.camera_now.lock_state_off), 4)
                             ? m.Read32(rt.orbit_player + static_cast<u64>(rt.profile.camera_now.lock_state_off))
                             : 0xffffffffu;
        LOG_INFO(Core, "VIZR hooks: camera probe: since the camera hook the camera moved {} cm, the player {} cm, its "
                 "forward {:.2f} deg; yaw ok {}, yaw xf {:#x}, player {:#x}, lock state {:#x}",
                 cam ? fmt::format("{:.2f}", dist(*cam, rt.cam_now_at_write[0], rt.cam_now_at_write[1], rt.cam_now_at_write[2]))
                     : std::string{"-"},
                 y_ok ? fmt::format("{:.2f}", dist(rt.player_t_at_write, y[3], y[7], y[11])) : std::string{"-"},
                 y_ok ? std::acos(std::clamp(rt.yaw_axis_at_write[0] * y[2] + rt.yaw_axis_at_write[1] * y[6] +
                                                 rt.yaw_axis_at_write[2] * y[10], -1.0f, 1.0f)) * 57.29578f
                      : -1.0f,
                 rt.yaw_at_write_ok, rt.yaw_xf_at, rt.orbit_player, lock);
    }
    const auto t = CameraNowTranslation(ctx, rt.cam_now_ptr);
    if (!t) {
        ++rt.cam_shift_skips;
        return;
    }
    const std::array<float, 3> d{(*t)[0] - rt.cam_now_at_write[0], (*t)[1] - rt.cam_now_at_write[1],
                                 (*t)[2] - rt.cam_now_at_write[2]};
    const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n > 2.0f) {
        ++rt.cam_shift_skips;
        return;
    }
    rt.cam_shift = d;
    ++rt.cam_shifts;
    rt.cam_shift_sum_cm += 100.0 * n / std::max(rt.space.units_per_meter * g_user_settings.Load().world_scale, 1e-3f);
}

/// The left arm's state at each camera hook: shown (flags bit0), the arm state (shown_state = the
/// free-look entry the game's show path makes), and the player's grapple / morph words; logged on the window line.
void ProbeLeftArmState(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& w = rt.profile.left_window;
    const u64 arm = rt.left_arm_ptr;
    auto& mem = ctx.memory;
    if (!w.present || w.state_off == 0 || arm == 0 || !mem.IsValidVirtualAddressRange(arm + w.prerender_flags_off, 4) ||
        !mem.IsValidVirtualAddressRange(arm + w.state_off, 4)) {
        return;
    }
    const u32 flags = mem.Read32(arm + w.prerender_flags_off);
    const u32 state = mem.Read32(arm + w.state_off);
    ++rt.la_ticks;
    rt.la_shown_ticks += (flags & 1u) != 0 ? 1 : 0;
    rt.la_entries += state == w.shown_state && rt.la_state_prev != w.shown_state ? 1 : 0;
    rt.la_state_prev = state;
    rt.la_flags_last = flags;
    rt.la_state_last = state;
    rt.la_states_seen |= state < 32 ? 1u << state : 0u;
    for (std::size_t i = 0; i < w.arm_words.size() && i < rt.la_words.size(); ++i) {
        rt.la_words[i] = mem.IsValidVirtualAddressRange(arm + w.arm_words[i], 4) ? mem.Read32(arm + w.arm_words[i])
                                                                                  : 0xffffffffu;
    }
    for (std::size_t i = 0; i < w.player_offs.size() && i < rt.la_player_last.size(); ++i) {
        const u64 at = rt.orbit_player + w.player_offs[i];
        rt.la_player_last[i] = rt.orbit_player != 0 && mem.IsValidVirtualAddressRange(at, 4) ? mem.Read32(at) : 0xffffffffu;
    }
}

void WriteCamera(const HitContext& ctx, const Camera& camera) {
    Runtime& rt = ctx.rt;
    rt.head_room_valid = false; // Set again only from a fresh head this tick
    const Field* source = rt.profile.FindField(camera.source);
    const Field* target = rt.profile.FindField(camera.target);
    // A look-at camera = three vec3f (eye, target, up) on each side.
    const bool look = camera.look_at;
    const auto vec3 = [&](const std::string& name) {
        const Field* f = rt.profile.FindField(name);
        return f != nullptr && f->type == FieldType::Vec3f ? f : nullptr;
    };
    const Field* src_at = look ? vec3(camera.source_at) : nullptr;
    const Field* src_up = look ? vec3(camera.source_up) : nullptr;
    const Field* dst_at = look ? vec3(camera.target_at) : nullptr;
    const Field* dst_up = look ? vec3(camera.target_up) : nullptr;
    const bool supported =
        look ? source && target && source->type == FieldType::Vec3f && target->type == FieldType::Vec3f && src_at &&
                   src_up && dst_at && dst_up
             : source && target && source->type == target->type &&
                   (source->type == FieldType::Mat3x4 || source->type == FieldType::Mat4);
    if (!supported) {
        if (!rt.camera_type_logged.exchange(true)) {
            LOG_ERROR(Core, "VIZR hooks: camera '{}' needs mat3x4 or mat4 source and target of one type, or vec3f "
                            "{{ pos, at, up }} triples (look_at); passthrough",
                      camera.id);
        }
        g_hook_state.camera_active = false;
        return;
    }
    const auto src_address = FieldAddress(ctx, *StateOf(rt, source));
    const auto dst_address = FieldAddress(ctx, *StateOf(rt, target));
    const auto address_of = [&](const Field* f) {
        return f != nullptr ? FieldAddress(ctx, *StateOf(rt, f)) : std::optional<u64>{};
    };
    const auto src_at_address = address_of(src_at), src_up_address = address_of(src_up);
    const auto dst_at_address = address_of(dst_at), dst_up_address = address_of(dst_up);
    if (!src_address || !dst_address ||
        (look && (!src_at_address || !src_up_address || !dst_at_address || !dst_up_address))) {
        g_hook_state.camera_active = false; // a skipped target field: no write this frame
        return;
    }
    // From here a look-at camera is handled as a row-major camera-to-world 3x4 (the Prime path).
    const bool mat34 = look || source->type == FieldType::Mat3x4;

    // Read the game's camera first, then write (format core rule 2).
    MatrixIo m;
    m.count = mat34 ? 12 : 16;
    m.row_major = look || rt.space.row_major;
    m.stride = m.row_major ? 4 : (mat34 ? 3 : 4);
    float look_distance = 0.0f; // |at - pos|: the written target keeps the game's distance
    if (look) {
        const auto read3 = [&](u64 a) {
            std::array<float, 3> v{};
            ctx.memory.ReadBlock(a, v.data(), sizeof(v));
            return v;
        };
        const auto pos = read3(*src_address);
        const auto at = read3(*src_at_address);
        const auto up_in = read3(*src_up_address);
        std::array<float, 3> back{pos[0] - at[0], pos[1] - at[1], pos[2] - at[2]};
        look_distance = std::sqrt(Dot(back, back));
        const auto cross = [](const std::array<float, 3>& u, const std::array<float, 3>& v) {
            return std::array<float, 3>{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        };
        auto right = cross(up_in, back);
        const float right_len = std::sqrt(Dot(right, right));
        if (!(look_distance > 1e-4f) || !(right_len > 1e-6f * look_distance) || !std::isfinite(look_distance)) {
            g_hook_state.camera_active = false; // degenerate (eye on the target, up along the view): leave it
            return;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            back[i] /= look_distance;
            right[i] /= right_len;
        }
        const auto up = cross(back, right); // the anim cameras' own roll stays: up comes from the game's
        for (std::size_t i = 0; i < 3; ++i) {
            m.At(i, 0) = right[i];
            m.At(i, 1) = up[i];
            m.At(i, 2) = back[i];
            m.At(i, 3) = pos[i];
        }
    } else {
        ctx.memory.ReadBlock(*src_address, m.raw.data(), m.count * sizeof(float));
    }
    const auto game_raw = m.raw; // Test log: the game's matrix before ours
    if (mat34 && rt.space.camera_to_world) {
        // The scan reticle's frame, in the same layout as camera_world (m.At, row-major 3x4)
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 4; ++j) {
                rt.game_camera[i * 4 + j] = m.At(i, j);
            }
        }
        rt.game_camera_valid = true;
        if (rt.rig_logs && rt.profile.snap.present && !rt.last_camera_right_eye) {
            // Rig cross-check (the view itself subtracts torso_applied as it stands here, before this tick's RunTorso:
            // a step written at the camera hook of tick N shows in the game camera at the hook of N+1):
            // the camera's own yaw changes, taken as our steer up to what is still pending.
            const auto up = Column(rt.basis, 1);
            std::array<float, 3> f{rt.game_camera[2], rt.game_camera[6], rt.game_camera[10]};
            const float along = f[0] * up[0] + f[1] * up[1] + f[2] * up[2];
            for (std::size_t i = 0; i < 3; ++i) {
                f[i] -= along * up[i];
            }
            const float fn = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            if (fn > 1e-4f) {
                for (float& v : f) {
                    v /= fn;
                }
                constexpr float Pi = std::numbers::pi_v<float>;
                const auto wrap_pi = [](float x) { return std::remainder(x, 2.0f * Pi); };
                float shown = 0.0f;
                const float pending = wrap_pi(rt.torso_applied - rt.torso_view);
                if (rt.gcam_prev_valid) {
                    const auto& a = rt.gcam_prev;
                    const std::array<float, 3> c{a[1] * f[2] - a[2] * f[1], a[2] * f[0] - a[0] * f[2], a[0] * f[1] - a[1] * f[0]};
                    const float world = std::atan2(c[0] * up[0] + c[1] * up[1] + c[2] * up[2], a[0] * f[0] + a[1] * f[1] + a[2] * f[2]);
                    const float local = -rt.profile.snap.sign * world; // RotateBodyYaw's rule backwards
                    if (pending * local > 0.0f) {
                        shown = std::copysign(std::min(std::abs(local), std::abs(pending)), pending);
                        rt.torso_view = wrap_pi(rt.torso_view + shown);
                    }
                    if (rt.rig_logs && rt.steer_frame_lines < 600 && (std::abs(pending) > 1e-4f || std::abs(local) > 1e-4f)) {
                        ++rt.steer_frame_lines;
                        LOG_INFO(Core, "VIZR hooks: steer frame: game camera {:+.3f} deg, taken {:+.3f}; applied {:.2f} view {:.2f} "
                                 "(pending was {:+.3f})", local * 57.29578f, shown * 57.29578f, rt.torso_applied * 57.29578f,
                                 rt.torso_view * 57.29578f, pending * 57.29578f);
                    }
                }
                rt.gcam_prev = f;
                rt.gcam_prev_valid = true;
            }
        }
        {
            // Test (head-bob): the game camera's forward (column 2, the layout camera_world uses), yaw about the
            // world up (Y) and pitch.
            const float fx = rt.game_camera[2], fy = rt.game_camera[6], fz = rt.game_camera[10];
            const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
            if (fl > 1e-6f) {
                const float yaw = std::atan2(fx, -fz) * 57.29578f;
                const float pitch = std::asin(std::clamp(fy / fl, -1.0f, 1.0f)) * 57.29578f;
                rt.gcam_yaw_min = std::min(rt.gcam_yaw_min, yaw);
                rt.gcam_yaw_max = std::max(rt.gcam_yaw_max, yaw);
                rt.gcam_pitch_min = std::min(rt.gcam_pitch_min, pitch);
                rt.gcam_pitch_max = std::max(rt.gcam_pitch_max, pitch);
            }
        }
        if (camera.cut_fade_ms > 0 && (camera.cut_jump_units > 0.0f || camera.cut_jump_deg > 0.0f)) {
            const auto& a = rt.cut_prev;
            const auto& b = rt.game_camera;
            if (rt.cut_prev_valid) {
                const float dx = b[3] - a[3], dy = b[7] - a[7], dz = b[11] - a[11];
                const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
                float trace = 0.0f; // trace(A^T B) over the 3x3 (rigid: 1 + 2 cos angle)
                for (std::size_t i = 0; i < 3; ++i) {
                    for (std::size_t j = 0; j < 3; ++j) {
                        trace += a[i * 4 + j] * b[i * 4 + j];
                    }
                }
                const float angle_deg =
                    std::acos(std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.2957795f;
                if ((camera.cut_jump_units > 0.0f && step > camera.cut_jump_units) ||
                    (camera.cut_jump_deg > 0.0f && angle_deg > camera.cut_jump_deg)) {
                    CutFade(rt, camera.cut_fade_ms, "jump");
                }
            }
            rt.cut_prev = b;
            rt.cut_prev_valid = true;
        }
    }

    // Lag probe: arm a pose id a few writes ahead so the capture records frames before it.
    const u64 write_index = rt.writes.fetch_add(1, std::memory_order_relaxed) + 1;
    const u64 next_id = g_pose_ledger.LatestId() + 1;
    if (rt.probe_every != 0 && write_index % rt.probe_every == 0) {
        g_hook_state.lag_probe_id = next_id + 2;
        LOG_INFO(Core, "VIZR lag probe: armed for pose id {}", next_id + 2);
    }
    const bool probe = g_hook_state.lag_probe_id.load(std::memory_order_relaxed) == next_id;

    // Camera-to-world rotation of the game's camera (world_to_view stores the transpose).
    Mat3 r{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            r[i][j] = m.At(i, j);
        }
    }
    if (!rt.space.camera_to_world) {
        r = Transpose(r);
    }

    // The FOV the game renders with this frame (read before any write).
    FieldState* fov_state = camera.fov_target.empty() ? nullptr : StateOf(rt, rt.profile.FindField(camera.fov_target));
    const auto fov_address = fov_state ? FieldAddress(ctx, *fov_state) : std::nullopt;
    float render_vfov = 0.0f;
    if (fov_address) {
        u32 bits = ctx.memory.Read32(*fov_address);
        std::memcpy(&render_vfov, &bits, sizeof(float));
        if (camera.fov_kind == "deg_vertical") {
            render_vfov *= std::numbers::pi_v<float> / 180.0f;
        }
    }

    // The head drives the rotation while a fresh pose exists; otherwise loopback.
    HeadPose head = g_latest_head.Load();
    const bool head_mode = camera.combine == "game_yaw_plus_head" || camera.combine == "head_replaces_rotation";
    const bool fresh = head.valid && SteadyNowNs() - head.sample_ns < HeadFreshNs;
    if (head_mode && fresh) {
        // Yaw source: a body matrix field read at this hook, if the profile has one.
        std::optional<Mat3> body;
        if (rt.yaw_field != nullptr && camera.yaw_source != "camera") { // Cutscenes turn with their camera
            FieldState* yaw_state = StateOf(rt, rt.yaw_field);
            if (const auto yaw_address = yaw_state ? FieldAddress(ctx, *yaw_state) : std::nullopt) {
                MatrixIo b;
                b.count = rt.yaw_field->type == FieldType::Mat3x4 ? 12 : 16;
                b.row_major = rt.space.row_major;
                b.stride = b.row_major ? 4 : (rt.yaw_field->type == FieldType::Mat3x4 ? 3 : 4);
                ctx.memory.ReadBlock(*yaw_address, b.raw.data(), b.count * sizeof(float));
                Mat3 m3{};
                for (std::size_t i = 0; i < 3; ++i) {
                    for (std::size_t j = 0; j < 3; ++j) {
                        m3[i][j] = b.At(i, j);
                    }
                }
                body = rt.space.camera_to_world ? m3 : Transpose(m3);
                rt.body_yaw_deg = std::atan2((*body)[0][2], -(*body)[2][2]) * 57.29578f;
                rt.body_yaw_valid = true;
                rt.body_axis = {(*body)[0][2], (*body)[1][2], (*body)[2][2]};
                rt.body_axis_ns = SteadyNowNs();
                if (!rt.last_camera_right_eye) {
                    rt.yaw_xf_at = *yaw_address;
                    rt.yaw_axis_at_write = rt.body_axis;
                    rt.player_t_at_write = {b.At(0, 3), b.At(1, 3), b.At(2, 3)};
                    rt.yaw_at_write_ok = rt.yaw_field->type == FieldType::Mat3x4 && rt.space.row_major;
                }
                rt.body_pos = {b.At(0, 3), b.At(1, 3), b.At(2, 3)};
            }
        }
        // The lock's turn from this frame's body, before the view uses it (after the write, the view lagged the
        // game's turn by one frame: 3.3 deg steps at a lock on the rig).
        RunLockView(ctx);
        r = HeadRotation(rt, camera, r, head, body ? &*body : nullptr, body ? &rt.yaw_forward : nullptr);
        // Translation: the game's (shake/bob included) plus the profile's height offset.
        const UserSettings user = g_user_settings.Load();
        const float height_m = camera.height_offset_m + user.eye_height_m; // + the player's eye height
        if (height_m != 0.0f && mat34 && rt.space.camera_to_world) {
            const auto up = Column(rt.basis, 1);
            const float lift = height_m * rt.space.units_per_meter * user.world_scale;
            for (std::size_t i = 0; i < 3; ++i) {
                m.At(i, 3) += up[i] * lift;
            }
        }
        // The head's room position (6DoF), not only its rotation
        // offset from the recenter point, capped at the lean limit, moves the first-person view. In the head's
        // own frame it maps through the camera rotation just written (game yaw + head), so it turns with the body.
        if (camera.mode == "first_person" && user.head_position >= 0.5f && mat34 &&
            rt.space.camera_to_world) {
            std::array<float, 3> off{head.position[0] - head.recenter_position[0],
                                     head.position[1] - head.recenter_position[1],
                                     head.position[2] - head.recenter_position[2]};
            const float len = std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
            const float limit = std::max(0.0f, user.lean_limit_m);
            if (len > limit && len > 1e-6f) {
                for (float& v : off) {
                    v *= limit / len;
                }
            }
            const auto apply = [](const Mat3& a, const std::array<float, 3>& v) {
                std::array<float, 3> o{};
                for (std::size_t i = 0; i < 3; ++i) {
                    o[i] = a[i][0] * v[0] + a[i][1] * v[1] + a[i][2] * v[2];
                }
                return o;
            };
            const auto head_local = apply(Transpose(FromQuat(head.orientation)), off); // the offset in the head's frame
            const auto world = apply(r, apply(rt.basis, head_local));
            const float units = rt.space.units_per_meter * user.world_scale;
            for (std::size_t i = 0; i < 3; ++i) {
                m.At(i, 3) += world[i] * units;
            }
            if (rt.rig_logs && SteadyNowNs() - rt.head_room_log_ns > 1'000'000'000) {
                rt.head_room_log_ns = SteadyNowNs();
                const float moved = std::sqrt(world[0] * world[0] + world[1] * world[1] + world[2] * world[2]) * units;
                LOG_INFO(Core, "VIZR hooks: head offset {:.3f} m (head frame {:+.3f},{:+.3f},{:+.3f}) -> camera moved "
                         "{:.3f} units ({:+.3f},{:+.3f},{:+.3f}) = {:.3f} m at {:.2f} units/m",
                         std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]), head_local[0], head_local[1],
                         head_local[2], moved, world[0] * units, world[1] * units, world[2] * units, moved / units, units);
            }
            // For the body: the same offset in the recentered room frame (x right, y up, z back), metres.
            rt.head_room = apply(AxisAngle({0.0f, 1.0f, 0.0f}, -(head.recenter_yaw + rt.torso_applied + rt.lock_view)), off);
            rt.head_room_valid = true;
        }
        if (fov_address && camera.fov_mode == "headset" && head.headset_vfov > 0.0f) {
            render_vfov = std::min(head.headset_vfov, camera.fov_max_deg * std::numbers::pi_v<float> / 180.0f);
            const float stored = camera.fov_kind == "deg_vertical"
                                     ? render_vfov * 180.0f / std::numbers::pi_v<float>
                                     : render_vfov;
            u32 bits = 0;
            std::memcpy(&bits, &stored, sizeof(float));
            ctx.memory.Write32(*fov_address, bits);
        }
    } else if (!fresh) {
        head.valid = false; // the presenter shows this frame as the flat quad
    }

    if (probe || g_test_roll_flag.load(std::memory_order_relaxed)) {
        r = Multiply(r, probe ? rt.probe_roll : rt.test_roll); // roll about the view axis
    }
    // M5 mode A (AFR): each camera write renders one eye, alternating. The camera moves by half
    // the runtime's IPD along its own right axis; the ledger records the eye.
    Eye eye = Eye::Both;
    const bool afr = g_stereo_mode.load(std::memory_order_relaxed) == StereoMode::Afr;
    if (afr && !rt.space.camera_to_world && !rt.afr_refused_logged.exchange(true)) {
        // Column 3 of a world_to_view matrix is the view translation, not the camera position.
        LOG_ERROR(Core, "VIZR hooks: stereo afr needs rotation_sense = camera_to_world; mono for this game");
    }
    if (head_mode && fresh && afr && rt.space.camera_to_world && head.ipd_m > 0.0f &&
        mat34) {
        eye = rt.next_eye;
        rt.next_eye = eye == Eye::Left ? Eye::Right : Eye::Left;
        rt.last_camera_right_eye = eye == Eye::Right;
        const float half = (eye == Eye::Left ? -0.5f : 0.5f) * head.ipd_m * rt.space.units_per_meter *
                           g_user_settings.Load().world_scale; // World scale
        const auto local_right = Column(rt.basis, 0); // game camera-local vector for OpenXR right
        for (std::size_t i = 0; i < 3; ++i) {
            const float world_right = r[i][0] * local_right[0] + r[i][1] * local_right[1] + r[i][2] * local_right[2];
            m.At(i, 3) += world_right * half;
        }
    }
    if (!rt.space.camera_to_world) {
        r = Transpose(r);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            m.At(i, j) = r[i][j];
        }
    }
    if (rt.rig_cam_back > 0.0f && rt.body_camera && rt.space.camera_to_world) {
        // Test runs only (VIZR_RIG_CAM_BACK): the view pulled back and up, to see the body's legs from behind.
        const auto back = Column(rt.basis, 2);
        const auto up = Column(rt.basis, 1);
        for (std::size_t i = 0; i < 3; ++i) {
            float wb = 0.0f;
            for (std::size_t j = 0; j < 3; ++j) {
                wb += m.At(i, j) * back[j];
            }
            m.At(i, 3) += wb * rt.rig_cam_back + up[i] * 0.3f * rt.rig_cam_back;
        }
    }
    // Without head, flag or probe the same bytes go back (loopback; proves the write path).
    if (look) {
        // Eye = column 3, target = eye + forward (-back) at the game's own distance, up = column 1.
        std::array<float, 3> pos{}, at{}, up{};
        for (std::size_t i = 0; i < 3; ++i) {
            pos[i] = m.At(i, 3);
            at[i] = pos[i] - m.At(i, 2) * look_distance;
            up[i] = m.At(i, 1);
        }
        ctx.memory.WriteBlock(*dst_address, pos.data(), sizeof(pos));
        ctx.memory.WriteBlock(*dst_at_address, at.data(), sizeof(at));
        ctx.memory.WriteBlock(*dst_up_address, up.data(), sizeof(up));
    } else {
        ctx.memory.WriteBlock(*dst_address, m.raw.data(), m.count * sizeof(float));
    }
    if (rt.rig_logs && mat34 && camera.id != rt.camera_logged_id) {
        rt.camera_logged_id = camera.id;
        rt.camera_log_ns = 0;
    }
    if (rt.rig_logs && mat34 && SteadyNowNs() - rt.camera_log_ns > 1'000'000'000) {
        // The game's camera and ours, rows of the 3x4, once a second per camera. (the ball view)
        rt.camera_log_ns = SteadyNowNs();
        const auto& g = game_raw;
        const auto& w = m.raw;
        LOG_INFO(Core, "VIZR hooks: camera '{}' game [{:.2f} {:.2f} {:.2f} {:.1f} | {:.2f} {:.2f} {:.2f} {:.1f} | {:.2f} {:.2f} {:.2f} "
                 "{:.1f}] ours [{:.2f} {:.2f} {:.2f} {:.1f} | {:.2f} {:.2f} {:.2f} {:.1f} | {:.2f} {:.2f} {:.2f} {:.1f}]",
                 camera.id, g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9], g[10], g[11], w[0], w[1], w[2],
                 w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11]);
    }
    if (mat34 && rt.space.camera_to_world) {
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 4; ++j) {
                rt.camera_world[i * 4 + j] = m.At(i, j); // The hand hook places the hand from it
            }
        }
        rt.camera_world_valid = true;
        if (!rt.last_camera_right_eye) {
            rt.camera_world_ns = SteadyNowNs();
            ++rt.camera_writes;
            rt.lw_open_at_camera += rt.left_window ? 1 : 0;
            ProbeLeftArmState(ctx);
            rt.cam_shift_applied = false;
            rt.cam_shift = {};
            if (const auto& cn = rt.profile.camera_now; cn.present) {
                const u64 ptr = ctx.regs.x(ctx.regs.ctx, cn.reg);
                const auto t = CameraNowTranslation(ctx, ptr);
                rt.cam_now_ok = false;
                if (t) {
                    const auto& g = rt.game_camera; // the game's camera this hook writes from
                    const float err = std::sqrt((g[3] - (*t)[0]) * (g[3] - (*t)[0]) + (g[7] - (*t)[1]) * (g[7] - (*t)[1]) +
                                                (g[11] - (*t)[2]) * (g[11] - (*t)[2]));
                    rt.cam_now_err_max = std::max(rt.cam_now_err_max, err);
                    rt.cam_now_ok = err <= cn.max_error;
                    rt.cam_now_ptr = ptr;
                    rt.cam_now_at_write = *t;
                }
            }
        }
        if (rt.rig_logs && !rt.last_camera_right_eye) {
            // Test: while the stick is held (and 2 s after: a fall, a landing), each tick's time step and the
            // BODY's position change (the [snap] yaw_field translation; the camera adds the walk bob and the landing
            // dip), with the profile's [move_log] fields.
            const s64 log_now = SteadyNowNs();
            if (g_move_stick_held.load(std::memory_order_relaxed)) {
                rt.move_until_ns = log_now + 2'000'000'000;
            }
            std::array<float, 3> cam_pos{rt.game_camera[3], rt.game_camera[7], rt.game_camera[11]};
            if (const Field* body_field = rt.profile.FindField(rt.profile.snap.yaw_field);
                body_field != nullptr && body_field->type == FieldType::Mat3x4) {
                if (const auto body_at = FieldAddress(ctx, *StateOf(rt, body_field));
                    body_at && ctx.memory.IsValidVirtualAddressRange(*body_at, 48)) {
                    std::array<float, 12> bx{};
                    ctx.memory.ReadBlock(*body_at, bx.data(), sizeof(bx));
                    cam_pos = {bx[3], bx[7], bx[11]};
                }
            }
            const s64 tick_ns = SteadyNowNs();
            if (rt.move_prev_ns != 0) {
                const float mdx = cam_pos[0] - rt.move_prev_pos[0], mdy = cam_pos[1] - rt.move_prev_pos[1],
                            mdz = cam_pos[2] - rt.move_prev_pos[2];
                const float move_delta = std::sqrt(mdx * mdx + mdy * mdy + mdz * mdz);
                const float dt_ms = static_cast<float>(tick_ns - rt.move_prev_ns) * 1e-6f;
                if (log_now < rt.move_until_ns && move_delta < 2.0f && rt.move_lines < 2000) {
                    ++rt.move_lines;
                    std::string move_fields;
                    for (const auto& field_name : rt.profile.move_log_fields) {
                        const auto field_value = ReadScalar(ctx, field_name);
                        move_fields += fmt::format(" {}={}", field_name,
                                             field_value ? fmt::format("{:#x}", *field_value) : std::string{"-"});
                    }
                    LOG_INFO(Core, "VIZR hooks: move tick: dt {:.2f} ms, delta {:.4f} u ({:.2f} u/s), y {:.3f}, game tick 1/{};{}",
                             dt_ms, move_delta, dt_ms > 0.01f ? move_delta * 1000.0f / dt_ms : 0.0f, cam_pos[1],
                             rt.tick_hz == 0 ? 60u : rt.tick_hz, move_fields);
                }
            }
            rt.move_prev_pos = cam_pos;
            rt.move_prev_ns = tick_ns;
        }
        if (rt.rig_logs && !rt.last_camera_right_eye) {
            // Test: the view's yaw change per frame while the head's yaw is still (the torso steer must not move the
            // view: under 0.1 deg per frame), once a second.
            const auto col_right = Column(rt.basis, 0), col_back = Column(rt.basis, 2);
            const std::array<float, 3> fw{rt.camera_world[2], rt.camera_world[6], rt.camera_world[10]};
            const float view_now = std::atan2(fw[0] * col_right[0] + fw[1] * col_right[1] + fw[2] * col_right[2],
                                              fw[0] * col_back[0] + fw[1] * col_back[1] + fw[2] * col_back[2]);
            const HeadPose hp = g_latest_head.Load();
            const auto& hq = hp.orientation;
            const float hfx = -2.0f * (hq[0] * hq[2] + hq[1] * hq[3]), hfz = -(1.0f - 2.0f * (hq[0] * hq[0] + hq[1] * hq[1]));
            const float head_now = std::atan2(-hfx, -hfz);
            constexpr float Pi = std::numbers::pi_v<float>;
            const auto wrap_pi = [](float x) { return std::remainder(x, 2.0f * Pi); };
            if (rt.view_prev_valid && std::abs(wrap_pi(head_now - rt.head_prev_yaw)) < 0.0002f) {
                rt.view_step_max = std::max(rt.view_step_max, std::abs(wrap_pi(view_now - rt.view_prev_yaw)));
                ++rt.view_still_frames;
            }
            rt.view_prev_yaw = view_now;
            rt.head_prev_yaw = head_now;
            rt.view_prev_valid = true;
            if (const s64 now_ns = SteadyNowNs(); now_ns - rt.view_step_logged_ns > 1'000'000'000) {
                rt.view_step_logged_ns = now_ns;
                LOG_INFO(Core, "VIZR hooks: view step: max {:.3f} deg per frame over {} head-still frame(s)",
                         rt.view_step_max * 57.29578f, rt.view_still_frames);
                rt.view_step_max = 0.0f;
                rt.view_still_frames = 0;
            }
        }
        // [orbit_head]: the head camera for the game's lock-on / grapple candidate box (read at the orbit hook).
        if (const auto& oh = rt.profile.orbit_head; oh.present) {
            const auto module = rt.modules.find(oh.buffer_module);
            const u64 buffer = module != rt.modules.end() ? module->second.first + oh.buffer_offset : 0;
            rt.orbit_head_ready = false;
            if (buffer != 0 && rt.body_camera && ctx.memory.IsValidVirtualAddressRange(buffer, 48)) {
                ctx.memory.WriteBlock(buffer, rt.camera_world.data(), 48);
                rt.orbit_head_ready = true;
            }
            WriteOrbitFacing(ctx);
        }
        if (!rt.last_camera_right_eye && rt.profile.arms.ik && rt.gun_xf_valid && rt.body_camera) {
            // The arms' anchors from this camera: the frame is drawn with it.
            const HandPose right = g_right_hand.Load();
            if (right.valid && SteadyNowNs() - right.sample_ns <= HeadFreshNs) {
                UpdateArmAnchors(rt, right);
            }
        }
    }
    if (const s64 now = SteadyNowNs(); rt.scan_state_mask != 0 && rt.game_camera_valid && rt.camera_world_valid &&
                                         (g_input_states.load(std::memory_order_relaxed) & rt.scan_state_mask) != 0 &&
                                         now - rt.scan_yaw_logged_ns > (rt.rig_logs ? 100'000'000 : 1'000'000'000)) {
        // In the Scan Visor, once a second (test runs: ten times), which yaw moves: the game camera's (look adjust at
        // 0xd71634 with a lock), the body's (the snap turn), ours, or the head's (headset space, left turn positive).
        rt.scan_yaw_logged_ns = now;
        const auto& g = rt.game_camera;
        const auto& c = rt.camera_world;
        const auto& q = head.orientation; // x, y, z, w; forward = q (0, 0, -1)
        const float fx = -2.0f * (q[0] * q[2] + q[3] * q[1]);
        const float fz = -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
        // The orbit state: the [orbit_head] lock_field as read here (the head's own scan pick otherwise).
        const auto& lock_field = rt.profile.orbit_head.lock_field;
        const auto orbit_now = lock_field.empty() ? std::nullopt : ReadScalar(ctx, lock_field);
        std::string scan_fields;
        for (const auto& field_name : rt.profile.scan_log_fields) {
            const auto v = ReadScalar(ctx, field_name);
            float f = 0.0f;
            const u32 bits = v ? static_cast<u32>(*v) : 0u;
            std::memcpy(&f, &bits, sizeof(f));
            scan_fields += v ? fmt::format(" {}={:#x} ({:.3f})", field_name, bits, f) : fmt::format(" {}=-", field_name);
        }
        LOG_INFO(Core, "VIZR hooks: scan yaw: game camera {:.2f} body {} ours {:.2f} head {:.2f}; orbit {};{}",
                 std::atan2(g[2], -g[10]) * 57.29578f,
                 rt.body_yaw_valid ? fmt::format("{:.2f}", rt.body_yaw_deg) : std::string{"-"},
                 std::atan2(c[2], -c[10]) * 57.29578f,
                 head.valid ? std::atan2(-fx, -fz) * 57.29578f : 0.0f, orbit_now ? *orbit_now : rt.scan_orbit, scan_fields);
    }

    const u64 id = g_pose_ledger.Append(head, render_vfov, eye);
    if (render_vfov > 0.0f) {
        rt.gui_vfov = render_vfov; // The GUI projection matches the world's
    }
    if (probe) {
        LOG_INFO(Core, "VIZR lag probe: pose id {} rolled {} degrees", id, ProbeRollDegrees);
    }
    if (!g_hook_state.camera_active.exchange(true)) {
        LOG_INFO(Core, "VIZR hooks: camera '{}' active (hook '{}', pose id {})", camera.id,
                 ctx.hook.name, id);
    }
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
}

/// Mode C: the render-job hook. Runs where the game's patch is about to rebuild
/// The draw camera from its node (Prime: the cave in front of CGameRenderJob::Execute);
/// Every game-specific number comes from the address file [mode_c]. Only the transform
/// changes: the projection stays the game's (symmetric, the headset FOV the camera hook wrote), and
/// the presenter crops each eye like AFR does.
/// A guest range a hook is about to read or write. A bad one (null, unmapped) is skipped: the caller returns without
/// touching it, and the site logs it once.
bool GuestOk(const HitContext& ctx, u64 at, u64 size, std::atomic<bool>& logged, std::string_view site) {
    if (at != 0 && ctx.memory.IsValidVirtualAddressRange(at, size)) {
        return true;
    }
    if (!logged.exchange(true)) {
        LOG_WARNING(Core, "VIZR hooks: {}: guest range {:#x} (+{:#x}) not mapped; skipped (logged once)", site, at, size);
    }
    return false;
}

void RunModeC(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const Profile::ModeC& spec = rt.profile.mode_c;
    if (g_stereo_mode.load(std::memory_order_relaxed) != StereoMode::ModeC) {
        return;
    }
    if (!rt.space.row_major || !rt.space.camera_to_world) {
        if (!rt.modec_refused_logged.exchange(true)) {
            LOG_ERROR(Core, "VIZR hooks: mode C needs row_major camera_to_world matrices; no eye cameras");
        }
        return;
    }
    const u64 job = ctx.regs.x(ctx.regs.ctx, spec.job_reg);
    const u64 scene = ctx.regs.x(ctx.regs.ctx, spec.scene_reg);
    static std::atomic<bool> job_logged, scene_logged, node_logged;
    const u64 job_span = std::max({spec.pass_offset, spec.eye_offsets[0], spec.eye_offsets[1]}) + 4;
    if (!GuestOk(ctx, job, job_span, job_logged, "mode C render job") ||
        !GuestOk(ctx, scene + spec.draw_camera_offset, 8, scene_logged, "mode C scene")) {
        return;
    }
    const u32 pass = ctx.memory.Read32(job + spec.pass_offset);
    {
        // The GUI camera hooks that follow in this pass (same thread, command order) need its eye.
        const bool gl = ctx.memory.Read32(job + spec.eye_offsets[0]) == spec.marker_bits;
        const bool gr = ctx.memory.Read32(job + spec.eye_offsets[1]) == spec.marker_bits;
        rt.gui_eye = (rt.profile.gui_eye.present && pass == rt.profile.gui_eye.eye_pass && gl != gr) ? (gl ? -1 : 1) : 0;
    }
    if (std::ranges::find(spec.eye_passes, pass) == spec.eye_passes.end()) {
        return; // Prime: world (0) and HUD (2); each eye runs both
    }
    const u64 node = ctx.memory.Read64(scene + spec.draw_camera_offset);
    if (node == 0 || !GuestOk(ctx, node + spec.node_transform_offset, 48, node_logged, "mode C camera node")) {
        return;
    }
    const bool left = ctx.memory.Read32(job + spec.eye_offsets[0]) == spec.marker_bits;
    const bool right = ctx.memory.Read32(job + spec.eye_offsets[1]) == spec.marker_bits;
    if (left == right) {
        return; // no eye marker (patch off, or another job)
    }
    std::array<float, 12> xf{};
    ctx.memory.ReadBlock(node + spec.node_transform_offset, xf.data(), sizeof(xf));
    if (left && pass == spec.eye_passes.front()) {
        // A node that still holds our last write was not refreshed by the
        // game (PullTransferData); its centre is the saved one, so the offset never adds up.
        if (rt.modec_have_written && xf == rt.modec_written) {
            g_modec_stats.centre_kept.fetch_add(1, std::memory_order_relaxed);
        } else {
            rt.modec_centre = xf;
            rt.modec_have_centre = true;
            if (rt.rig_logs && SteadyNowNs() - rt.modec_log_ns > 1'000'000'000) {
                rt.modec_log_ns = SteadyNowNs(); // What mode C draws the eyes from
                LOG_INFO(Core, "VIZR hooks: mode C draw camera [{:.2f} {:.2f} {:.2f} {:.1f} | {:.2f} {:.2f} {:.2f} {:.1f} | "
                         "{:.2f} {:.2f} {:.2f} {:.1f}]",
                         xf[0], xf[1], xf[2], xf[3], xf[4], xf[5], xf[6], xf[7], xf[8], xf[9], xf[10], xf[11]);
            }
        }
    }
    if (!rt.modec_have_centre) {
        return;
    }
    const HeadPose head = g_latest_head.Load();
    const float ipd = head.ipd_m > 0.0f ? head.ipd_m : 0.0f;
    // The eye moves by half the IPD along the camera's right axis: the rotation times the game
    // camera-local vector for OpenXR right.
    std::array<float, 12> eye = rt.modec_centre;
    const float half = (left ? -0.5f : 0.5f) * ipd * rt.space.units_per_meter *
                       g_user_settings.Load().world_scale; // World scale
    const auto local_right = Column(rt.basis, 0);
    for (std::size_t i = 0; i < 3; ++i) {
        const float world_right = eye[i * 4 + 0] * local_right[0] + eye[i * 4 + 1] * local_right[1] +
                                  eye[i * 4 + 2] * local_right[2];
        eye[i * 4 + 3] += world_right * half;
    }
    ctx.memory.WriteBlock(node + spec.node_transform_offset, eye.data(), sizeof(eye));
    rt.modec_written = eye;
    rt.modec_have_written = true;
    g_modec_stats.eye_sets.fetch_add(1, std::memory_order_relaxed);
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
}

/// The visor HUD's GUI camera for the current eye. `proj_at` = a CMatrix4f (row-major): its
/// m00/m02/m11/m12 become the world camera's symmetric projection (vertical FOV gui_vfov, the frame's aspect), near
/// and far stay the GUI camera's. `view_at` = the view-to-world 3x4 (row-major): its translation moves by -/+ half
/// the IPD along column 0, in GUI units at the HUD pivot's depth. Either address may be 0 (not written).
void WriteGuiEye(const HitContext& ctx, u64 proj_at, u64 view_at) {
    Runtime& rt = ctx.rt;
    const auto& g = rt.profile.gui_eye;
    if (rt.gui_eye == 0 || g_stereo_mode.load(std::memory_order_relaxed) != StereoMode::ModeC) {
        return;
    }
    auto& mem = ctx.memory;
    // The world's FOV for the GUI projection: the profile's choice, or the player's (HUD tab "Visor size: True helmet").
    const bool world_fov = g.projection == "world" || g_user_settings.Load().visor_true_helmet >= 0.5f;
    // With the game's own projection the HUD is drawn at the game's FOV inside
    // an eye with the world's, so a shift in GUI units lands smaller on screen; the shift is scaled by the ratio of the
    // world's horizontal projection scale to the game's (1 when the world's projection is written).
    if (proj_at != 0 && !world_fov && rt.gui_vfov > 0.0f && mem.IsValidVirtualAddressRange(proj_at, 64)) {
        u32 bits = mem.Read32(proj_at + 0x00);
        float game_p00{};
        std::memcpy(&game_p00, &bits, sizeof(game_p00));
        const float world_p00 = 1.0f / (std::tan(rt.gui_vfov * 0.5f) * g.aspect);
        rt.gui_shift_scale = game_p00 > 1e-3f ? std::clamp(world_p00 / game_p00, 0.25f, 4.0f) : 1.0f;
    } else if (proj_at != 0) {
        rt.gui_shift_scale = 1.0f;
    }
    if (proj_at != 0 && world_fov && rt.gui_vfov > 0.0f && mem.IsValidVirtualAddressRange(proj_at, 64)) {
        const float t = std::tan(rt.gui_vfov * 0.5f);
        const std::array<float, 2> row0{1.0f / (t * g.aspect), 0.0f}; // m00, m01
        mem.WriteBlock(proj_at + 0x00, row0.data(), sizeof(row0));
        mem.Write32(proj_at + 0x08, 0);                                 // m02 = 0 (symmetric, like the world)
        const std::array<float, 2> m11_m12{1.0f / t, 0.0f};
        mem.WriteBlock(proj_at + 0x14, m11_m12.data(), sizeof(m11_m12));
    }
    if (view_at != 0 && mem.IsValidVirtualAddressRange(view_at, 48)) {
        const HeadPose head = g_latest_head.Load();
        const float hud_depth = std::max(rt.profile.hud_depth_m, 0.1f);
        const float shift = static_cast<float>(rt.gui_eye) * g.eye_sign * 0.5f * std::max(head.ipd_m, 0.0f) *
                            g.units_per_m / hud_depth * rt.gui_shift_scale;
        std::array<float, 12> v{};
        mem.ReadBlock(view_at, v.data(), sizeof(v));
        for (std::size_t r = 0; r < 3; ++r) {
            v[r * 4 + 3] += shift * v[r * 4 + 0];
        }
        mem.WriteBlock(view_at, v.data(), sizeof(v));
    }
    if (++rt.gui_writes == 1) {
        LOG_INFO(Core, "VIZR hooks: GUI camera per eye (visor depth): vfov {:.1f} deg, aspect {:.3f}, eye {:+d}, {:.2f} "
                 "GUI units per metre at {:.2f} m, eye_sign {:+.0f}, shift scale {:.3f} (projection {})",
                 rt.gui_vfov * 57.29578f, g.aspect, rt.gui_eye, g.units_per_m / std::max(rt.profile.hud_depth_m, 0.1f),
                 rt.profile.hud_depth_m, g.eye_sign, rt.gui_shift_scale, world_fov ? "world" : "game");
    }
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
}

/// Mode C backend "look_at_copy". The game's patch draws the world twice per frame
/// (eye L as view 0, eye R as view 1), each draw from a stack copy of the look-at camera; this hook runs just before
/// each draw call and writes that copy: the eye = the centre (the game camera with the head already applied by the
/// camera hook) moved by half the IPD along its right axis, target moved with it (parallel eyes), up kept, and the
/// view matrix (rows right, up, back; translation -row . eye) rebuilt. The projection stays the game's (the headset FOV
/// the camera hook wrote); the presenter crops each eye as for Prime.
void RunModeCLookAt(const HitContext& ctx, bool left) {
    Runtime& rt = ctx.rt;
    const Profile::ModeC& spec = rt.profile.mode_c;
    if (g_stereo_mode.load(std::memory_order_relaxed) != StereoMode::ModeC) {
        return;
    }
    auto& mem = ctx.memory;
    const u64 cam = ctx.regs.x(ctx.regs.ctx, spec.camera_reg);
    const u64 span = std::max({spec.cam_matrix_off + 48, spec.cam_pos_off + 12, spec.cam_at_off + 12, spec.cam_up_off + 12});
    if (cam == 0 || !mem.IsValidVirtualAddressRange(cam, span)) {
        return;
    }
    std::array<float, 9> now{};
    mem.ReadBlock(cam + spec.cam_pos_off, now.data(), 12);
    mem.ReadBlock(cam + spec.cam_at_off, now.data() + 3, 12);
    mem.ReadBlock(cam + spec.cam_up_off, now.data() + 6, 12);
    if (left) {
        // A copy that still holds our last write was not refreshed; keep the saved
        // centre so the offset never adds up.
        if (rt.modec_look_have_written && now == rt.modec_look_written) {
            g_modec_stats.centre_kept.fetch_add(1, std::memory_order_relaxed);
        } else {
            rt.modec_look_centre = now;
            rt.modec_look_have_centre = true;
        }
        for (const std::string& name : spec.zero_fields) { // once per frame (TemporalInterlace off)
            const Field* field = rt.profile.FindField(name);
            FieldState* state = field != nullptr ? StateOf(rt, field) : nullptr;
            if (const auto at = state != nullptr ? FieldAddress(ctx, *state) : std::nullopt;
                at && mem.IsValidVirtualAddressRange(*at, 1) && mem.Read8(*at) != 0) {
                mem.Write8(*at, 0);
                if (!rt.modec_zero_logged.exchange(true)) {
                    LOG_INFO(Core, "VIZR hooks: mode C: field '{}' was on; set to 0 every frame", name);
                }
            }
        }
    }
    if (!rt.modec_look_have_centre) {
        return;
    }
    const auto& c = rt.modec_look_centre;
    std::array<float, 3> back{c[0] - c[3], c[1] - c[4], c[2] - c[5]};
    const std::array<float, 3> up_in{c[6], c[7], c[8]};
    const float dist = std::sqrt(Dot(back, back));
    std::array<float, 3> right{up_in[1] * back[2] - up_in[2] * back[1], up_in[2] * back[0] - up_in[0] * back[2],
                               up_in[0] * back[1] - up_in[1] * back[0]};
    const float right_len = std::sqrt(Dot(right, right));
    if (!(dist > 1e-4f) || !(right_len > 1e-6f * dist) || !std::isfinite(dist)) {
        return; // degenerate camera: the game's own copy draws
    }
    for (std::size_t i = 0; i < 3; ++i) {
        back[i] /= dist;
        right[i] /= right_len;
    }
    const std::array<float, 3> up{back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2],
                                  back[0] * right[1] - back[1] * right[0]};
    const HeadPose head = g_latest_head.Load();
    const float ipd = head.ipd_m > 0.0f ? head.ipd_m : 0.0f;
    const float half = (left ? -0.5f : 0.5f) * ipd * rt.space.units_per_meter * g_user_settings.Load().world_scale;
    std::array<float, 9> eye = c;
    for (std::size_t i = 0; i < 3; ++i) {
        eye[i] += right[i] * half;     // eye
        eye[3 + i] += right[i] * half; // target: parallel eyes
    }
    const std::array<float, 3> pos{eye[0], eye[1], eye[2]};
    std::array<float, 12> view{};
    const std::array<const std::array<float, 3>*, 3> rows{&right, &up, &back};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t j = 0; j < 3; ++j) {
            view[r * 4 + j] = (*rows[r])[j];
        }
        view[r * 4 + 3] = -Dot(*rows[r], pos);
    }
    mem.WriteBlock(cam + spec.cam_pos_off, eye.data(), 12);
    mem.WriteBlock(cam + spec.cam_at_off, eye.data() + 3, 12);
    mem.WriteBlock(cam + spec.cam_matrix_off, view.data(), sizeof(view));
    rt.modec_look_written = eye;
    rt.modec_look_have_written = true;
    g_modec_stats.eye_sets.fetch_add(1, std::memory_order_relaxed);
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
    if (rt.rig_logs && left && SteadyNowNs() - rt.modec_log_ns > 1'000'000'000) {
        rt.modec_log_ns = SteadyNowNs();
        LOG_INFO(Core, "VIZR hooks: mode C draw camera eye ({:.1f},{:.1f},{:.1f}) target ({:.1f},{:.1f},{:.1f}) up "
                       "({:.2f},{:.2f},{:.2f}), half IPD {:.2f} units",
                 c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], half);
    }
}

/// A "corners" HUD element (Prime's minimap window): move and scale its corner points
/// about their centre like a widget (same slider units), restore them at the exit hook.
void RunHudCorners(const HitContext& ctx, const Profile::HudWidgets::Element& element, std::size_t index, bool enter) {
    Runtime& rt = ctx.rt;
    const auto& w = rt.profile.hud_widgets;
    auto& mem = ctx.memory;
    if (index >= rt.corners_saved.size()) {
        return;
    }
    auto& saved = rt.corners_saved[index]; // per element (one list would let another element restore it)
    if (!enter) {
        for (const auto& [at, xyz] : saved) {
            if (mem.IsValidVirtualAddressRange(at, sizeof(xyz))) { // the points may be gone since the enter hook
                mem.WriteBlock(at, xyz.data(), sizeof(xyz));
            }
        }
        saved.clear();
        return;
    }
    if (!saved.empty()) {
        RunHudCorners(ctx, element, index, false); // the exit hook was missed: never let an offset build up
    }
    const u64 obj = ctx.regs.x(ctx.regs.ctx, element.corners_reg);
    if (obj == 0 || !mem.IsValidVirtualAddressRange(obj + element.engaged_off, 1) ||
        !mem.IsValidVirtualAddressRange(obj + element.data_off, 8) ||
        !mem.IsValidVirtualAddressRange(obj + element.count_off, 4) || mem.Read8(obj + element.engaged_off) == 0) {
        return;
    }
    const u32 count = std::min<u32>(mem.Read32(obj + element.count_off), 16);
    const u64 data = mem.Read64(obj + element.data_off);
    if (count == 0 || data == 0 || !mem.IsValidVirtualAddressRange(data, count * element.stride)) {
        return;
    }
    std::vector<std::array<float, 3>> points(count);
    std::array<float, 3> centre{};
    for (u32 k = 0; k < count; ++k) {
        mem.ReadBlock(data + k * element.stride, points[k].data(), sizeof(points[k]));
        for (std::size_t c = 0; c < 3; ++c) {
            centre[c] += points[k][c] / static_cast<float>(count);
        }
    }
    if (rt.rig_logs && !rt.corners_logged) {
        rt.corners_logged = true;
        LOG_INFO(Core, "VIZR hooks: hud '{}' corners: {} points, first ({:.2f},{:.2f},{:.2f}) centre ({:.2f},{:.2f},{:.2f})",
                 element.name, count, points[0][0], points[0][1], points[0][2], centre[0], centre[1], centre[2]);
    }
    const UserSettings user = g_user_settings.Load();
    VIZR::HudElement e = index < user.hud.count ? user.hud.elements[index] : VIZR::HudElement{};
    e.offset[0] += element.base_offset[0];
    e.offset[1] += element.base_offset[1];
    e.scale *= element.base_scale;
    if (e.Identity()) {
        return;
    }
    const std::array<float, 3> move{e.offset[0] * w.units_per_screen_height, e.offset[1] * w.units_per_screen_height,
                                    -e.depth};
    if (rt.rig_logs && w.extent_camera && index < rt.hud_extent_ns.size() &&
        SteadyNowNs() - rt.hud_extent_ns[index] > 3'000'000'000) {
        rt.hud_extent_ns[index] = SteadyNowNs(); // the per-eye crop gate (as RunHudEnter)
        const auto& [vfov_deg, plane_z, aspect] = *w.extent_camera;
        const float t = std::tan(vfov_deg * 0.5f * std::numbers::pi_v<float> / 180.0f);
        float x0 = 9.0f, x1 = -9.0f, y0 = 9.0f, y1 = -9.0f;
        for (const auto& q : points) {
            std::array<float, 3> p{};
            for (std::size_t c = 0; c < 3; ++c) {
                p[c] = centre[c] + e.scale * (q[c] - centre[c]) + move[c];
            }
            const float depth = plane_z - p[2];
            if (depth > 0.1f) {
                x0 = std::min(x0, p[0] / (depth * t * aspect)), x1 = std::max(x1, p[0] / (depth * t * aspect));
                y0 = std::min(y0, p[1] / (depth * t)), y1 = std::max(y1, p[1] / (depth * t));
            }
        }
        LOG_INFO(Core, "VIZR hooks: hud extent '{}' ndc x [{:.2f}, {:.2f}] y [{:.2f}, {:.2f}] render vfov {:.1f} deg",
                 element.name, x0, x1, y0, y1, g_latest_head.Load().headset_vfov * 57.29578f);
    }
    for (u32 k = 0; k < count; ++k) {
        const u64 at = data + k * element.stride;
        saved.emplace_back(at, points[k]);
        std::array<float, 3> p{};
        for (std::size_t c = 0; c < 3; ++c) {
            p[c] = centre[c] + e.scale * (points[k][c] - centre[c]) + move[c];
        }
        mem.WriteBlock(at, p.data(), sizeof(p));
    }
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
}

/// Every byte of a widget the HUD code reads or writes: its world transform, child and sibling links, alpha, and the
/// drawn-flag bytes. A widget is only touched when this whole span is mapped.
u64 WidgetSpan(const Profile::HudWidgets& w) {
    u64 span = std::max({w.world_xf_offset + 48, w.first_child_offset + 8, w.next_sibling_offset + 8, w.alpha_offset + 4});
    for (const auto& [off, mask] : w.drawn_flags) {
        span = std::max(span, off + 1);
    }
    if (w.alpha_bypass) {
        span = std::max(span, w.alpha_bypass->first + 1);
    }
    return span;
}

/// Write back everything the last hud_draw_enter changed, newest first.
void HudRestore(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& w = rt.profile.hud_widgets;
    for (auto it = rt.hud_saved.rbegin(); it != rt.hud_saved.rend(); ++it) {
        if (!ctx.memory.IsValidVirtualAddressRange(it->widget, WidgetSpan(w))) {
            continue; // freed or moved since the enter hook: nothing to put back
        }
        ctx.memory.WriteBlock(it->widget + w.world_xf_offset, it->xf.data(), sizeof(it->xf));
        ctx.memory.Write32(it->widget + w.alpha_offset, it->alpha);
    }
    rt.hud_saved.clear();
    rt.hud_pending = false;
}

/// Before the HUD copies its widgets into render commands, move, scale and fade each
/// configured element's subtree (rigidly, about the element root). Mode C replays the copies per eye.
void RunHudEnter(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& w = rt.profile.hud_widgets;
    if (rt.hud_pending) {
        HudRestore(ctx); // the exit hook was missed: never let an offset build up
        if (rt.hud_restores_at_enter++ == 0) {
            LOG_WARNING(Core, "VIZR hooks: HUD exit hook missed once; restored at enter (logged once)");
        }
    }
    const UserSettings user = g_user_settings.Load();
    const VIZR::HudConfig& cfg = user.hud;
    std::optional<u64> state;
    if (!w.state_field.empty()) {
        state = ReadScalar(ctx, w.state_field);
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(cfg.count, w.elements.size()); ++i) {
        VIZR::HudElement e = cfg.elements[i];
        const auto& element = w.elements[i];
        if (element.kind != "widget") {
            continue; // Corner elements move at their own hooks (RunHudCorners)
        }
        e.offset[0] += element.base_offset[0]; // The profile's placement under the player's
        e.offset[1] += element.base_offset[1];
        e.scale *= element.base_scale;
        // A state may give several roots (the Scan Visor's deco is three widget trees and its
        // scan box two); they move and scale together about the first one's pivot.
        std::vector<const std::string*> root_fields;
        for (const auto& [st, field] : element.root_field_in_state) {
            if (state && *state == st) {
                root_fields.push_back(&field);
            }
        }
        const bool state_root = !root_fields.empty();
        if (!state_root) {
            root_fields.push_back(&element.root_field);
        }
        if (!state_root && element.only_in_state && (!state || *state != *element.only_in_state)) {
            continue;
        }
        // The test logs every element's on-screen extent every 3 s (the per-eye crop gate), moved or not.
        const bool log_extent = rt.rig_logs && w.extent_camera && i < rt.hud_extent_ns.size() &&
                                SteadyNowNs() - rt.hud_extent_ns[i] > 3'000'000'000;
        if (e.Identity() && !log_extent) {
            continue;
        }
        std::vector<u64> roots;
        for (const std::string* field : root_fields) {
            const auto r = ReadScalar(ctx, *field);
            if (r && *r != 0 && ctx.memory.IsValidVirtualAddressRange(*r, WidgetSpan(w)) &&
                std::ranges::find(roots, *r) == roots.end()) {
                roots.push_back(*r);
            }
        }
        if (roots.empty()) {
            continue;
        }
        const u64* root = &roots.front(); // the pivot
        // Each root, then its descendants: first child, next sibling (at most 512 widgets in all).
        std::vector<u64> nodes;
        std::unordered_set<u64> visited; // a cycle (or a root under another) never transforms a widget twice
        std::vector<u64> todo;
        for (const u64 r : roots) {
            if (visited.insert(r).second) {
                nodes.push_back(r);
                if (const u64 child = ctx.memory.Read64(r + w.first_child_offset); child != 0) {
                    todo.push_back(child);
                }
            }
        }
        while (!todo.empty() && nodes.size() < 512) {
            const u64 node = todo.back();
            todo.pop_back();
            if (!visited.insert(node).second || !ctx.memory.IsValidVirtualAddressRange(node, WidgetSpan(w))) {
                continue;
            }
            nodes.push_back(node);
            if (const u64 next = ctx.memory.Read64(node + w.next_sibling_offset); next != 0) {
                todo.push_back(next);
            }
            if (const u64 child = ctx.memory.Read64(node + w.first_child_offset); child != 0) {
                todo.push_back(child);
            }
        }
        std::array<float, 12> root_xf{};
        ctx.memory.ReadBlock(*root + w.world_xf_offset, root_xf.data(), sizeof(root_xf));
        const float s = e.scale;
        const std::array<float, 3> pivot{root_xf[3], root_xf[7], root_xf[11]};
        const std::array<float, 3> move{e.offset[0] * w.units_per_screen_height,
                                        e.offset[1] * w.units_per_screen_height, -e.depth};
        if (log_extent) {
            // Every widget's placed translation through the HUD camera (fov 65 vertical, aspect 1.778,
            // camera at z = 20 looking down -Z) to frame NDC; min/max = the element's extent.
            rt.hud_extent_ns[i] = SteadyNowNs();
            const auto& [vfov_deg, plane_z, aspect] = *w.extent_camera;
            const float t = std::tan(vfov_deg * 0.5f * std::numbers::pi_v<float> / 180.0f);
            float x0 = 9.0f, x1 = -9.0f, y0 = 9.0f, y1 = -9.0f;
            u32 drawn = 0;
            for (const u64 node : nodes) {
                // Only widgets drawn this frame (hidden ones are parked off-screen).
                if (!w.drawn_flags.empty()) {
                    bool on = true;
                    for (const auto& [off, mask] : w.drawn_flags) {
                        on &= ctx.memory.IsValidVirtualAddressRange(node + off, 1) &&
                              (ctx.memory.Read8(node + off) & mask) != 0;
                    }
                    u32 alpha_bits = ctx.memory.Read32(node + w.alpha_offset);
                    float alpha{};
                    std::memcpy(&alpha, &alpha_bits, sizeof(alpha));
                    const bool bypass = w.alpha_bypass && ctx.memory.IsValidVirtualAddressRange(node + w.alpha_bypass->first, 1) &&
                                        (ctx.memory.Read8(node + w.alpha_bypass->first) & w.alpha_bypass->second) != 0;
                    if (!on || (std::abs(alpha) < 1e-5f && !bypass)) {
                        continue;
                    }
                }
                ++drawn;
                std::array<float, 12> nx{};
                ctx.memory.ReadBlock(node + w.world_xf_offset, nx.data(), sizeof(nx));
                std::array<float, 3> p{};
                for (std::size_t r = 0; r < 3; ++r) {
                    p[r] = pivot[r] + s * (nx[r * 4 + 3] - pivot[r]) + move[r];
                }
                const float depth = plane_z - p[2];
                if (depth <= 0.1f) {
                    continue;
                }
                const float nxc = p[0] / (depth * t * aspect), nyc = p[1] / (depth * t);
                x0 = std::min(x0, nxc), x1 = std::max(x1, nxc), y0 = std::min(y0, nyc), y1 = std::max(y1, nyc);
            }
            if (drawn == 0) {
                LOG_INFO(Core, "VIZR hooks: hud extent '{}' not drawn", element.name);
            } else {
                LOG_INFO(Core, "VIZR hooks: hud extent '{}' ndc x [{:.2f}, {:.2f}] y [{:.2f}, {:.2f}] render vfov {:.1f} deg",
                         element.name, x0, x1, y0, y1, g_latest_head.Load().headset_vfov * 57.29578f);
            }
        }
        if (e.Identity()) {
            continue;
        }
        for (const u64 node : nodes) {
            Runtime::HudSaved saved{.widget = node};
            ctx.memory.ReadBlock(node + w.world_xf_offset, saved.xf.data(), sizeof(saved.xf));
            saved.alpha = ctx.memory.Read32(node + w.alpha_offset);
            rt.hud_saved.push_back(saved);
            std::array<float, 12> xf = saved.xf;
            for (std::size_t r = 0; r < 3; ++r) {
                for (std::size_t c = 0; c < 3; ++c) {
                    xf[r * 4 + c] *= s;
                }
                xf[r * 4 + 3] = pivot[r] + s * (xf[r * 4 + 3] - pivot[r]) + move[r];
            }
            float alpha = 0.0f;
            std::memcpy(&alpha, &saved.alpha, sizeof(alpha));
            alpha *= e.alpha;
            u32 alpha_bits = 0;
            std::memcpy(&alpha_bits, &alpha, sizeof(alpha_bits));
            ctx.memory.WriteBlock(node + w.world_xf_offset, xf.data(), sizeof(xf));
            ctx.memory.Write32(node + w.alpha_offset, alpha_bits);
        }
        ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
    }
    // The whole HUD, after the elements: every widget under the frame roots, about the frame
    // centre (the GUI camera's axis, x = y = 0 in frame space).
    if (!user.whole.Identity()) {
        const float s = user.whole.scale;
        const std::array<float, 3> move{user.whole.offset[0] * w.units_per_screen_height,
                                        user.whole.offset[1] * w.units_per_screen_height, -user.whole.depth};
        for (const std::string& field : w.whole_roots) {
            const auto root = ReadScalar(ctx, field);
            if (!root || *root == 0 || !ctx.memory.IsValidVirtualAddressRange(*root, WidgetSpan(w))) {
                continue;
            }
            std::vector<u64> todo{*root};
            std::unordered_set<u64> visited;
            while (!todo.empty() && visited.size() < 2048) {
                const u64 node = todo.back();
                todo.pop_back();
                if (!visited.insert(node).second || !ctx.memory.IsValidVirtualAddressRange(node, WidgetSpan(w))) {
                    continue;
                }
                if (node != *root) {
                    if (const u64 next = ctx.memory.Read64(node + w.next_sibling_offset); next != 0) {
                        todo.push_back(next);
                    }
                }
                if (const u64 child = ctx.memory.Read64(node + w.first_child_offset); child != 0) {
                    todo.push_back(child);
                }
                Runtime::HudSaved saved{.widget = node};
                ctx.memory.ReadBlock(node + w.world_xf_offset, saved.xf.data(), sizeof(saved.xf));
                saved.alpha = ctx.memory.Read32(node + w.alpha_offset);
                rt.hud_saved.push_back(saved);
                std::array<float, 12> xf = saved.xf;
                for (std::size_t r = 0; r < 3; ++r) {
                    for (std::size_t c = 0; c < 3; ++c) {
                        xf[r * 4 + c] *= s;
                    }
                }
                xf[3] = xf[3] * s + move[0];
                xf[7] = xf[7] * s + move[1];
                xf[11] = xf[11] + move[2];
                ctx.memory.WriteBlock(node + w.world_xf_offset, xf.data(), sizeof(xf));
            }
        }
    }
    rt.hud_pending = !rt.hud_saved.empty();
}

/// A hand pose (relative to the head, OpenXR view space) in the game world, from the camera the camera hook wrote:
/// the point `local_offset` metres along the hand's own axes (right, up, back), and the hand's LookAt axes
/// (left = up x forward). Shared by the cannon (right hand) and the left arm.
struct HandWorld {
    std::array<float, 3> position{};
    std::array<float, 3> left{}, up{}, forward{};
};
/// The hand's axes turned by yaw_deg about the world's up (positive = toward the hand's left), so the turn is a
/// pure yaw whatever the hand's roll.
HandWorld TurnAim(const Runtime& rt, HandWorld hw, float yaw_deg) {
    if (yaw_deg == 0.0f) {
        return hw;
    }
    const auto u = Column(rt.basis, 1); // the world's up, as UpdateArmAnchors
    const auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    const auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    // The handedness from the hand itself: + turns the forward toward the hand's left.
    const float sign = dot(cross(u, hw.forward), hw.left) >= 0.0f ? 1.0f : -1.0f;
    const float a = sign * yaw_deg * std::numbers::pi_v<float> / 180.0f;
    const float c = std::cos(a), s = std::sin(a);
    const auto turn = [&](const std::array<float, 3>& v) { // Rodrigues about u
        const auto k = cross(u, v);
        const float d = dot(u, v) * (1.0f - c);
        return std::array<float, 3>{v[0] * c + k[0] * s + u[0] * d, v[1] * c + k[1] * s + u[1] * d,
                                    v[2] * c + k[2] * s + u[2] * d};
    };
    hw.forward = turn(hw.forward);
    hw.left = turn(hw.left);
    hw.up = turn(hw.up);
    return hw;
}

HandWorld HandToWorld(const Runtime& rt, const HandPose& hand, const std::array<float, 3>& local_offset) {
    const auto& cw = rt.camera_world;
    const auto cam = [&](const std::array<float, 3>& local) { // camera-local game vector -> world direction
        std::array<float, 3> out{};
        for (std::size_t i = 0; i < 3; ++i) {
            out[i] = cw[i * 4 + 0] * local[0] + cw[i * 4 + 1] * local[1] + cw[i * 4 + 2] * local[2];
        }
        return out;
    };
    const auto xr_to_local = [&](const std::array<float, 3>& v) { // OpenXR view-space vector -> camera-local
        std::array<float, 3> out{};
        for (std::size_t i = 0; i < 3; ++i) {
            out[i] = rt.basis[i][0] * v[0] + rt.basis[i][1] * v[1] + rt.basis[i][2] * v[2];
        }
        return out;
    };
    const auto rotate = [](const std::array<float, 4>& q, const std::array<float, 3>& v) {
        const std::array<float, 3> u{q[0], q[1], q[2]};
        const auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
            return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        };
        const auto t = cross(u, v);
        const std::array<float, 3> t2{2.0f * t[0], 2.0f * t[1], 2.0f * t[2]};
        const auto c = cross(u, t2);
        return std::array<float, 3>{v[0] + q[3] * t2[0] + c[0], v[1] + q[3] * t2[1] + c[1], v[2] + q[3] * t2[2] + c[2]};
    };
    const UserSettings user = g_user_settings.Load();
    const float units = rt.space.units_per_meter * user.world_scale;
    const auto offset = rotate(hand.orientation, local_offset);
    std::array<float, 3> pos_xr{hand.position[0] + offset[0], hand.position[1] + offset[1], hand.position[2] + offset[2]};
    for (float& p : pos_xr) {
        p *= units;
    }
    const auto pos = cam(xr_to_local(pos_xr));
    const std::array<float, 3> world{cw[3] + pos[0], cw[7] + pos[1], cw[11] + pos[2]};
    const auto forward = cam(xr_to_local(rotate(hand.orientation, {0.0f, 0.0f, -1.0f})));
    const auto up = cam(xr_to_local(rotate(hand.orientation, {0.0f, 1.0f, 0.0f})));
    const std::array<float, 3> left{up[1] * forward[2] - up[2] * forward[1], up[2] * forward[0] - up[0] * forward[2],
                                    up[0] * forward[1] - up[1] * forward[0]};
    return HandWorld{world, left, up, forward};
}

/// The left shoulder
/// from the headset. The neck sits below the head and faces the game body's yaw (the player's facing: snap turns and
/// locomotion), low-passed over 0.3 s, so the shoulders neither swing when the head turns nor turn when the player
/// leans. Without the body yaw
/// (ball, cutscenes: the arm is not solved there) the head-to-hand directions stand in, within 90 deg of the head.
/// The shoulder is out along the neck's left; beyond half the arm's reach it follows the hand a little, its
/// downward part damped.
/// Test log: the angle (deg, 0..90) between the shoulder's neck facing and the applied body's flat forward column.
float NeckBodyAngleDeg(const Runtime& rt) {
    const auto up = Column(rt.basis, 1);
    std::array<float, 3> b = rt.body_axis;
    const float along = b[0] * up[0] + b[1] * up[1] + b[2] * up[2];
    for (std::size_t i = 0; i < 3; ++i) {
        b[i] -= along * up[i];
    }
    const float nb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    const auto& n = rt.neck_fwd;
    const float nn = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nb < 1e-4f || nn < 1e-4f) {
        return 0.0f;
    }
    const float c = std::abs(n[0] * b[0] + n[1] * b[1] + n[2] * b[2]) / (nb * nn);
    return std::acos(std::clamp(c, 0.0f, 1.0f)) * 57.29578f;
}

template <typename Flat>
void LeftShoulderAnchor(Runtime& rt, const HandPose& right_hand, const std::array<float, 3>& head_fwd,
                        const std::array<float, 3>& up, const Flat& flat) {
    using V = std::array<float, 3>;
    const auto& a = rt.profile.arms;
    const UserSettings user = g_user_settings.Load();
    const float units = rt.space.units_per_meter * user.world_scale;
    const auto& cw = rt.camera_world;
    const V head{cw[3], cw[7], cw[11]};
    const auto dot = [](const V& p, const V& q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; };
    const auto cross = [](const V& p, const V& q) {
        return V{p[1] * q[2] - p[2] * q[1], p[2] * q[0] - p[0] * q[2], p[0] * q[1] - p[1] * q[0]};
    };
    const HandPose left_hand = g_left_hand.Load();
    V sum{};
    for (const HandPose* hp : {&left_hand, &right_hand}) {
        if (!hp->valid || SteadyNowNs() - hp->sample_ns > HeadFreshNs) {
            continue;
        }
        const auto p = HandToWorld(rt, *hp, {0.0f, 0.0f, 0.0f}).position;
        V d{p[0] - head[0], p[1] - head[1], p[2] - head[2]};
        const float along = dot(d, up);
        for (std::size_t i = 0; i < 3; ++i) {
            d[i] -= along * up[i];
        }
        if (std::sqrt(dot(d, d)) > 1e-3f * units) {
            const V n = flat(d);
            for (std::size_t i = 0; i < 3; ++i) {
                sum[i] += n[i];
            }
        }
    }
    V neck = std::sqrt(dot(sum, sum)) > 0.1f ? flat(sum) : head_fwd;
    if (dot(neck, head_fwd) < 0.0f) { // more than 90 deg from the head: the nearest edge of the allowed range
        V side{neck[0] - dot(neck, head_fwd) * head_fwd[0], neck[1] - dot(neck, head_fwd) * head_fwd[1],
               neck[2] - dot(neck, head_fwd) * head_fwd[2]};
        neck = std::sqrt(dot(side, side)) > 1e-4f ? flat(side) : head_fwd;
    }
    const s64 now_ns = SteadyNowNs();
    bool from_body = false;
    V body_now{}; // the body's facing this frame, unfiltered (the stick's frame; the low-pass is the shoulder's only)
    if (now_ns - rt.body_axis_ns < HeadFreshNs) {
        // The body column's sign against the view: the camera's own forward column and head_fwd are the same direction.
        const auto& cw2 = rt.camera_world;
        const float sign = dot(flat(V{cw2[2], cw2[6], cw2[10]}), head_fwd) >= 0.0f ? 1.0f : -1.0f;
        V b = rt.body_axis;
        const float along = dot(b, up);
        for (std::size_t i = 0; i < 3; ++i) {
            b[i] = sign * (b[i] - along * up[i]);
        }
        if (std::sqrt(dot(b, b)) > 1e-4f) {
            neck = flat(b);
            body_now = neck;
            from_body = true;
            // While the torso drives the body, the body still trails the torso estimate by the gap the dead band and
            // the 90 deg/s limit leave (several degrees at large turns), but the wrist target comes through the view,
            // which counts the head from the estimate. The shoulder faces the estimate: the remaining gap turns it on,
            // so the anchor and the target share a frame (a trailing anchor put the controller past the arm's reach).
            const HeadPose pose = g_latest_head.Load();
            const float mode = g_user_settings.Load().move_forward;
            const bool torso_mode = (mode > 1.5f && mode < 2.5f) || mode > 3.5f; // Body, the body follows the head
            if (!rt.rig_anchor_applied && pose.valid && pose.torso_valid && torso_mode &&
                !g_torso_hold.load(std::memory_order_relaxed)) {
                constexpr float Pi = std::numbers::pi_v<float>;
                float gap = pose.torso_offset - rt.torso_applied; // LOCAL yaw, left +
                while (gap > Pi) {
                    gap -= 2.0f * Pi;
                }
                while (gap < -Pi) {
                    gap += 2.0f * Pi;
                }
                // The same turn RotateBodyYaw writes for this yaw: a = sign * (-gap) about up; for a flat v,
                // v' = cos(a) v + sin(a) (up x v).
                const float ang = rt.profile.snap.sign * -gap;
                const V ux = cross(up, neck);
                V turned{};
                for (std::size_t i = 0; i < 3; ++i) {
                    turned[i] = std::cos(ang) * neck[i] + std::sin(ang) * ux[i];
                }
                neck = flat(turned);
            }
        }
    }
    // Low-pass, time constant 0.3 s.: the first body yaw (spawn, or back from the hands fallback) is taken at once:
    // the hands' direction can be 180 deg from the game's facing.
    const bool seed = from_body && !rt.neck_from_body;
    rt.neck_from_body = from_body;
    if (!seed && rt.neck_ns != 0 && std::sqrt(dot(rt.neck_fwd, rt.neck_fwd)) > 0.5f) {
        const float dt = std::clamp(static_cast<float>(now_ns - rt.neck_ns) * 1e-9f, 0.0f, 1.0f);
        const float k = 1.0f - std::exp(-dt / 0.3f);
        V mixed{};
        for (std::size_t i = 0; i < 3; ++i) {
            mixed[i] = rt.neck_fwd[i] + k * (neck[i] - rt.neck_fwd[i]);
        }
        if (std::sqrt(dot(mixed, mixed)) > 1e-3f) {
            neck = flat(mixed);
        }
    }
    rt.neck_ns = now_ns;
    const V neck_right = cross(neck, up);
    const auto& o = a.left_shoulder_anchor_m; // out, down, back; plus the menu's shoulder sliders, mirrored
    const float out = o[0] + user.shoulder_right_m, down = o[1] + user.shoulder_down_m, back = o[2] + user.shoulder_back_m;
    V s{};
    for (std::size_t i = 0; i < 3; ++i) {
        s[i] = head[i] + units * (-out * neck_right[i] - down * up[i] - back * neck[i]);
    }
    float nudge_m = 0.0f;
    if (left_hand.valid && SteadyNowNs() - left_hand.sample_ns <= HeadFreshNs && a.left_reach_nudge[0] > 0.0f) {
        const auto p = HandToWorld(rt, left_hand, {0.0f, 0.0f, 0.0f}).position;
        V d{p[0] - s[0], p[1] - s[1], p[2] - s[2]};
        const float dist_m = std::sqrt(dot(d, d)) / std::max(units, 1e-3f);
        const float start_m = a.left_reach_nudge[2] * user.arm_length_m;
        if (dist_m > start_m && dist_m > 1e-3f) {
            V n{};
            for (std::size_t i = 0; i < 3; ++i) {
                n[i] = d[i] / (dist_m * units) * (dist_m - start_m) * a.left_reach_nudge[0];
            }
            if (const float v = dot(n, up); v < 0.0f) { // the downward part damped
                for (std::size_t i = 0; i < 3; ++i) {
                    n[i] -= (1.0f - a.left_reach_nudge[1]) * v * up[i];
                }
            }
            nudge_m = std::sqrt(dot(n, n));
            for (std::size_t i = 0; i < 3; ++i) {
                s[i] += units * n[i];
            }
        }
    }
    rt.shoulder_left_world = s;
    rt.neck_fwd = neck;
    rt.neck_right = neck_right;
    // The left stick's frame. The game walks the stick in the body's frame (forward = the body); the player
    // means the head's (or the left controller's) facing: the stick goes through that facing's axes, expressed in the
    // body's. Without the body yaw (ball, cutscenes) the stick passes unchanged.
    {
        VIZR::StickXf xf{};
        xf.sample_ns = now_ns;
        const float mode = user.move_forward;
        V facing = head_fwd;
        // 1 = the left controller, 3 = the right one, 4 (the body follows the head) the left one; its flat facing turned
        // move_hand_offset_deg toward the body centre (a relaxed hand points outward; the offset is mirrored per hand,
        // so both sides walk straight).
        const bool head_body = mode > 3.5f;
        const bool left_mode = (mode > 0.5f && mode < 1.5f) || head_body, right_mode = mode > 2.5f && mode < 3.5f;
        const HandPose& hand = left_mode ? left_hand : right_hand;
        if ((left_mode || right_mode) && hand.valid && now_ns - hand.sample_ns <= HeadFreshNs) {
            const auto f = HandToWorld(rt, hand, {0.0f, 0.0f, 0.0f}).forward;
            V d{f[0], f[1], f[2]};
            const float a2 = dot(d, up);
            for (std::size_t i = 0; i < 3; ++i) {
                d[i] -= a2 * up[i];
            }
            if (std::sqrt(dot(d, d)) > 0.2f) { // pointing near straight up / down: keep the head's facing
                const V fd = flat(d);
                const V rd = cross(fd, up); // the facing's right
                const float t = user.move_hand_offset_deg * (left_mode ? 1.0f : -1.0f) / 57.29578f; // toward the centre
                facing = flat(V{std::cos(t) * fd[0] + std::sin(t) * rd[0], std::cos(t) * fd[1] + std::sin(t) * rd[1],
                                std::cos(t) * fd[2] + std::sin(t) * rd[2]});
            }
        }
        if (from_body && (mode < 1.5f || right_mode || head_body)) {
            // The game walks the stick in the body's frame of this instant; the low-passed neck
            // would rotate the stick by the lag after every snap turn.
            const V& bf = body_now;
            const V br = cross(bf, up), fr = cross(facing, up);
            // (move right, move forward) in the body's axes for a stick (x right, y forward) in the facing's axes
            xf.m = {dot(fr, br), dot(facing, br), dot(fr, bf), dot(facing, bf)};
        }
        VIZR::g_move_stick.Store(xf);
        if (now_ns - rt.move_frame_logged_ns > 1'000'000'000) {
            // Every session, once a second: where forward is, in world yaw (the presenter's body rules line has the
            // stick, the hands and the torso estimate for the same second).
            rt.move_frame_logged_ns = now_ns;
            const auto yaw_deg = [&](const V& v) { return std::atan2(v[0], -v[2]) * 57.29578f; };
            LOG_INFO(Core, "VIZR hooks: move frame 1s: forward follows {}; game body {} deg, view (head) {:.1f} deg, "
                     "facing used {:.1f} deg",
                     mode < 0.5f   ? "head"
                     : head_body   ? "left hand (the body follows the head)"
                     : left_mode   ? "left hand"
                     : right_mode  ? "right hand"
                                   : "body",
                     from_body ? fmt::format("{:.1f}", yaw_deg(body_now)) : std::string{"-"}, yaw_deg(head_fwd),
                     yaw_deg(facing));
        }
        if (rt.rig_logs && from_body && now_ns - rt.move_logged_ns > 500'000'000) {
            // Test: where the body moved in the last half second, against the head's and the
            // body's facing (signed, about the world up).
            V d{rt.body_pos[0] - rt.move_logged_pos[0], rt.body_pos[1] - rt.move_logged_pos[1],
                rt.body_pos[2] - rt.move_logged_pos[2]};
            const float along = dot(d, up);
            for (std::size_t i = 0; i < 3; ++i) {
                d[i] -= along * up[i];
            }
            const float dist_m = std::sqrt(dot(d, d)) / std::max(units, 1e-3f);
            if (rt.move_logged_ns != 0 && dist_m > 0.05f) {
                const V dn = flat(d);
                const auto signed_deg = [&](const V& a) {
                    return std::atan2(dot(cross(a, dn), up), dot(a, dn)) * 57.29578f;
                };
                LOG_INFO(Core, "VIZR hooks: move check: {:.2f} m in 0.5 s, {:.1f} deg from the head's facing, {:.1f} deg "
                         "from the body's (mode {:.0f}, head {:.1f} deg from the body)",
                         dist_m, signed_deg(head_fwd), signed_deg(body_now), user.move_forward,
                         std::atan2(dot(cross(body_now, head_fwd), up), dot(body_now, head_fwd)) * 57.29578f);
            }
            rt.move_logged_pos = rt.body_pos;
            rt.move_logged_ns = now_ns;
        }
    }
    if (const s64 now = SteadyNowNs(); now - rt.anchor_logged_ns > (rt.rig_logs ? 1'000'000'000 : 10'000'000'000)) {
        rt.anchor_logged_ns = now;
        const V rel{(s[0] - head[0]) / units, (s[1] - head[1]) / units, (s[2] - head[2]) / units};
        const V hr = cross(head_fwd, up);
        LOG_INFO(Core, "VIZR hooks: left shoulder anchor: neck ({}) yaw {:.1f} deg, {:.1f} deg from the head's, nudge {:.3f} m; "
                 "shoulder from the head (head frame, m): right {:.3f} up {:.3f} forward {:.3f}",
                 from_body ? "body" : "hands", std::atan2(neck[0], -neck[2]) * 57.29578f,
                 std::acos(std::clamp(dot(neck, head_fwd), -1.0f, 1.0f)) * 57.29578f, nudge_m, dot(rel, hr),
                 dot(rel, up), dot(rel, head_fwd));
    }
}

/// The arms' anchors from the head camera in camera_world: the body frame (the head's flat forward, its right, world
/// up), both shoulders (the profile's offset plus the player's sliders; the left mirrored) and the left shoulder anchor.
/// Set at the gun hook and again at every camera hook, so the arms drawn in a frame use that frame's camera
/// (the render-time left-arm write and the solves read these; from the gun hook alone they were a tick old).
void UpdateArmAnchors(Runtime& rt, const HandPose& hand) {
    const UserSettings user = g_user_settings.Load();
    const HandWorld head = HandToWorld(rt, HandPose{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, 0, true},
                                       {0.0f, 0.0f, 0.0f});
    const auto world_up = Column(rt.basis, 1);
    const auto flat = [&](std::array<float, 3> v) {
        const float d = v[0] * world_up[0] + v[1] * world_up[1] + v[2] * world_up[2];
        for (std::size_t i = 0; i < 3; ++i) {
            v[i] -= d * world_up[i];
        }
        const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (float& x : v) {
            x /= n > 1e-6f ? n : 1.0f;
        }
        return v;
    };
    const auto fwd = flat(head.forward);
    const std::array<float, 3> right{fwd[1] * world_up[2] - fwd[2] * world_up[1],
                                     fwd[2] * world_up[0] - fwd[0] * world_up[2],
                                     fwd[0] * world_up[1] - fwd[1] * world_up[0]};
    const auto& p = rt.profile.arms.shoulder_offset_m;
    const std::array<float, 3> o{p[0] + user.shoulder_right_m, p[1] - user.shoulder_down_m,
                                 p[2] + user.shoulder_back_m}; // + the menu's shoulder sliders
    const float units_ik = rt.space.units_per_meter * user.world_scale;
    for (std::size_t i = 0; i < 3; ++i) {
        rt.shoulder_world[i] = rt.camera_world[i * 4 + 3] +
                               units_ik * (o[0] * right[i] + o[1] * world_up[i] - o[2] * fwd[i]);
        // The left shoulder = the right one mirrored across the body's centre line.
        rt.shoulder_left_world[i] = rt.camera_world[i * 4 + 3] +
                                    units_ik * (-o[0] * right[i] + o[1] * world_up[i] - o[2] * fwd[i]);
    }
    rt.body_right = right;
    rt.body_up = world_up;
    rt.body_fwd = fwd;
    LeftShoulderAnchor(rt, hand, fwd, world_up, flat);
}

/// The right hand's pose in the game world, from the camera the camera hook wrote and the
/// hand relative to the head (XR thread). Columns follow the game's LookAt convention: col1 = up,
/// col2 = forward, col0 = up x forward; each target keeps its own column lengths (a scale it carries).
std::string NodeIdText(Runtime& rt); // the rig's GetNodeId read (defined with the device code below)

void RunHand(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& hands = rt.profile.hands;
    const HandPose hand = g_right_hand.Load();
    ShiftCameraToNow(ctx);
    {
        // Every session, once a second: how old the head camera is that the cannon is placed from (the camera hook's
        // last write) and how many camera writes came between gun writes (0 = the gun runs before this tick's camera:
        // the cannon trails a moving player by one tick).
        const s64 now = SteadyNowNs();
        const u64 between = rt.camera_writes - rt.gun_cam_writes_seen;
        rt.gun_cam_writes_seen = rt.camera_writes;
        ++rt.gun_cam_between[std::min<u64>(between, 2)];
        if (rt.camera_world_ns != 0) {
            const float age = static_cast<float>(now - rt.camera_world_ns) * 1e-6f;
            rt.gun_cam_age_max_ms = std::max(rt.gun_cam_age_max_ms, age);
            rt.gun_cam_age_sum_ms += age;
            ++rt.gun_cam_samples;
        }
        if (now - rt.gun_cam_logged_ns > 1'000'000'000 && rt.gun_cam_samples != 0) {
            rt.gun_cam_logged_ns = now;
            LOG_INFO(Core, "VIZR hooks: cannon base 1s: camera written {:.2f} ms (mean) / {:.2f} ms (max) before the gun "
                     "write; camera writes between gun writes: 0 x{}, 1 x{}, 2+ x{}; cannon moved to this tick's camera "
                     "x{} (mean {:.1f} cm), skipped x{}; camera check max error {:.4f} units; game ticks per drawn frame: "
                     "0 x{}, 1 x{}, 2+ x{}",
                     rt.gun_cam_age_sum_ms / rt.gun_cam_samples, rt.gun_cam_age_max_ms, rt.gun_cam_between[0],
                     rt.gun_cam_between[1], rt.gun_cam_between[2], rt.cam_shifts,
                     rt.cam_shifts != 0 ? rt.cam_shift_sum_cm / rt.cam_shifts : 0.0, rt.cam_shift_skips,
                     rt.cam_now_err_max, rt.render_ticks[0], rt.render_ticks[1], rt.render_ticks[2]);
            if (rt.profile.left_window.present) {
                LOG_INFO(Core, "VIZR hooks: left arm window 1s: opened at PreRender x{}, Render x{}, rain draw x{}; closed "
                         "x{}; left solves in the window x{}, outside x{}; camera hooks with the window open x{}",
                         rt.lw_opens[0], rt.lw_opens[1], rt.lw_opens[2], rt.lw_closes, rt.lw_solves_in, rt.lw_solves_out,
                         rt.lw_open_at_camera);
                rt.lw_opens = {};
                rt.lw_closes = rt.lw_solves_in = rt.lw_solves_out = rt.lw_open_at_camera = 0;
                if (rt.profile.left_window.state_off != 0) {
                    std::string player;
                    const auto& offs = rt.profile.left_window.player_offs;
                    for (std::size_t i = 0; i < offs.size() && i < rt.la_player_last.size(); ++i) {
                        player += fmt::format(" PL+{:#x}={:#x}", offs[i], rt.la_player_last[i]);
                    }
                    const auto& words = rt.profile.left_window.arm_words;
                    for (std::size_t i = 0; i < words.size() && i < rt.la_words.size(); ++i) {
                        player += fmt::format(" arm+{:#x}={:#x}", words[i], rt.la_words[i]);
                    }
                    LOG_INFO(Core, "VIZR hooks: left arm state 1s: shown in {} of {} camera hooks, entries into the shown "
                             "state {}; flags {:#x}, state {} (seen {:#x});{}", rt.la_shown_ticks, rt.la_ticks,
                             rt.la_entries, rt.la_flags_last, rt.la_state_last, rt.la_states_seen, player);
                    if (rt.la_device_n != 0) {
                        std::string parts;
                        for (u32 k = 0; k < rt.la_device_n; ++k) {
                            const auto& m = rt.la_device[k];
                            const auto col = [&](std::size_t c) {
                                return std::sqrt(m[c] * m[c] + m[4 + c] * m[4 + c] + m[8 + c] * m[8 + c]);
                            };
                            parts += fmt::format(" +{:#x} t ({:.3f},{:.3f},{:.3f}) cols {:.2f}/{:.2f}/{:.2f} rows "
                                                 "[{:.2f} {:.2f} {:.2f}; {:.2f} {:.2f} {:.2f}; {:.2f} {:.2f} {:.2f}]",
                                                 rt.profile.left_window.device_locals[k], m[3], m[7], m[11], col(0), col(1),
                                                 col(2), m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
                        }
                        LOG_INFO(Core, "VIZR hooks: left arm device locals (last Render entry, {}; beam owned word {}; "
                                 "written x{}, left to the game x{}; our elbow captured x{}):{}",
                                 rt.la_device_ours ? "ours as drawn" : "the game's", rt.la_owned, rt.la_device_writes,
                                 rt.la_device_skips, rt.dev_captures,
                                 parts + rt.dev_render_read + rt.la_elbow_space + NodeIdText(rt));
                        rt.la_device_writes = rt.la_device_skips = rt.dev_captures = 0;
                        rt.la_device_n = 0;
                    }
                    rt.la_ticks = rt.la_shown_ticks = rt.la_entries = rt.la_states_seen = 0;
                }
            }
            rt.render_ticks = {};
            rt.cam_shifts = 0;
            rt.cam_shift_skips = 0;
            rt.cam_shift_sum_cm = 0.0;
            rt.cam_now_err_max = 0.0f;
            rt.gun_cam_between = {};
            rt.gun_cam_age_max_ms = 0.0f;
            rt.gun_cam_age_sum_ms = 0.0;
            rt.gun_cam_samples = 0;
        }
    }
    if (!rt.camera_world_valid || !hand.valid || SteadyNowNs() - hand.sample_ns > HeadFreshNs ||
        !rt.space.camera_to_world || !rt.space.row_major) {
        rt.hand_world_valid = false; // no hand: the game's own cannon and aim (never a stale hand pose)
        rt.gun_xf_valid = false;     // .. and the game's own arm
        return;
    }
    const UserSettings user = g_user_settings.Load();
    // Position: hand + the object's offset along the hand's own axes (OpenXR: right, up, back), in metres -> game
    // units: the profile's anchor (Prime: the grip, not the model origin) plus the player's menu adjustment.
    const std::array<float, 3> local_offset{hands.offset_m[0], hands.offset_m[1] + user.cannon_up_m,
                                            hands.offset_m[2] - user.cannon_forward_m};
    const HandWorld hw = HandToWorld(rt, hand, local_offset);
    // [camera_now]: the cannon at this tick's camera position (the hook runs before the tick's camera hook).
    const std::array<float, 3> world{hw.position[0] + rt.cam_shift[0], hw.position[1] + rt.cam_shift[1],
                                     hw.position[2] + rt.cam_shift[2]};
    const auto& left = hw.left;
    const auto& up = hw.up;
    const auto& forward = hw.forward;
    for (const std::string& name : hands.write) {
        const Field* field = rt.profile.FindField(name);
        FieldState* state = field ? StateOf(rt, field) : nullptr;
        const auto address = state ? FieldAddress(ctx, *state) : std::nullopt;
        if (!address || field->type != FieldType::Mat3x4) {
            continue;
        }
        std::array<float, 12> xf{};
        ctx.memory.ReadBlock(*address, xf.data(), sizeof(xf));
        const std::array<std::array<float, 3>, 3> cols{left, up, forward};
        for (std::size_t c = 0; c < 3; ++c) {
            const float length = std::sqrt(xf[0 * 4 + c] * xf[0 * 4 + c] + xf[1 * 4 + c] * xf[1 * 4 + c] +
                                           xf[2 * 4 + c] * xf[2 * 4 + c]);
            const float keep = (length > 1e-3f ? length : 1.0f) * std::clamp(user.cannon_scale, 0.5f, 1.5f);
            for (std::size_t r = 0; r < 3; ++r) {
                xf[r * 4 + c] = cols[c][r] * keep;
            }
        }
        xf[3] = world[0];
        xf[7] = world[1];
        xf[11] = world[2];
        ctx.memory.WriteBlock(*address, xf.data(), sizeof(xf));
    }
    rt.hand_world = world;
    if (rt.profile.arms.ik && !hands.write.empty()) {
        // The model transform (the first written target) and the body's shoulder (yaw only: the head's
        // horizontal right/back and world up), for the arm solve at the pose build.
        if (const Field* f = rt.profile.FindField(hands.write.front())) {
            if (FieldState* st = StateOf(rt, f)) {
                if (const auto at = FieldAddress(ctx, *st)) {
                    ctx.memory.ReadBlock(*at, rt.gun_xf.data(), sizeof(rt.gun_xf));
                    UpdateArmAnchors(rt, hand); // again at the camera hook, from that camera (the one the arms are drawn with)
                    rt.gun_xf_valid = true;
                }
            }
        }
    }
    if (rt.rig_logs && ++rt.gun_logs % 60 == 1) {
        // Test runs only: where the cannon's origin went, in metres from the camera along world axes, and the
        // hand's axes, so the grip anchor can be checked by numbers instead of by eye.
        const float u = rt.space.units_per_meter * user.world_scale;
        LOG_INFO(Core, "VIZR hooks: cannon origin ({:.2f},{:.2f},{:.2f}) m from the camera, forward ({:.2f},{:.2f},{:.2f}) "
                 "up ({:.2f},{:.2f},{:.2f}), camera forward ({:.2f},{:.2f},{:.2f})",
                 (world[0] - rt.camera_world[3]) / u, (world[1] - rt.camera_world[7]) / u,
                 (world[2] - rt.camera_world[11]) / u, forward[0], forward[1], forward[2], up[0], up[1], up[2],
                 rt.camera_world[2], rt.camera_world[6], rt.camera_world[10]);
    }
    rt.hand_axes = {left, up, forward};
    rt.hand_world_valid = true;
    ++rt.hand_writes;
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
}

/// At each shot, the object's pose against the hand pose written this frame, in metres (a test check).
void RunFire(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const Field* field = rt.profile.FindField(rt.profile.hands.fire_field);
    FieldState* state = field ? StateOf(rt, field) : nullptr;
    const auto address = state ? FieldAddress(ctx, *state) : std::nullopt;
    if (!address || ++rt.fire_logs > 200) {
        return;
    }
    std::array<float, 12> xf{};
    ctx.memory.ReadBlock(*address, xf.data(), sizeof(xf));
    if (rt.profile.hands.fire_origin_forward != 0.0f) {
        // Prime: the shot starts set back along its own column 2 (penetration check); start it at the
        // muzzle of the hand-held cannon instead.
        for (std::size_t r = 0; r < 3; ++r) {
            xf[r * 4 + 3] += rt.profile.hands.fire_origin_forward * xf[r * 4 + 2];
        }
        ctx.memory.WriteBlock(*address, xf.data(), sizeof(xf));
    }
    const float units = rt.space.units_per_meter * g_user_settings.Load().world_scale;
    const float dx = (xf[3] - rt.hand_world[0]) / units, dy = (xf[7] - rt.hand_world[1]) / units,
                dz = (xf[11] - rt.hand_world[2]) / units;
    const float cx = (xf[3] - rt.camera_world[3]) / units, cy = (xf[7] - rt.camera_world[7]) / units,
                cz = (xf[11] - rt.camera_world[11]) / units;
    LOG_INFO(Core,
             "VIZR hooks: shot {}: object at ({:.2f}, {:.2f}, {:.2f}) m from the camera; {:.3f} m from the hand pose "
             "written this frame{}; forward ({:.2f}, {:.2f}, {:.2f})",
             rt.fire_logs, cx, cy, cz, std::sqrt(dx * dx + dy * dy + dz * dz),
             rt.hand_world_valid ? "" : " (no hand written yet)", xf[2], xf[6], xf[10]);
}

/// M6 [aim]: the game's aiming cursor casts its ray once per tick from the camera transform
/// it just fetched; replace that transform with the hand's (origin = the cannon, forward along the hand), so the
/// cast, the cursor's hit point and the shots (FirePrimary aims at it) follow the hand.
void RunCursorRay(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& aim = rt.profile.aim;
    const u64 cursor = ctx.regs.x(ctx.regs.ctx, aim.cursor_reg);
    const u64 cursor_end = std::max({aim.cursor_on_plane_off, aim.cursor_hit_off, aim.cursor_distance_off}) + 12;
    rt.cursor_ptr = cursor != 0 && ctx.memory.IsValidVirtualAddressRange(cursor, cursor_end) ? cursor : 0;
    rt.ray_seen = true;
    // Test runs only: the hit distance the game measured last tick (the wall walk stops on it).
    if (rt.rig_logs && aim.cursor_distance_off != 0 && ++rt.cursor_calls % 10 == 0 && rt.cursor_ptr != 0) {
        u32 bits = ctx.memory.Read32(rt.cursor_ptr + aim.cursor_distance_off);
        float distance = 0.0f;
        std::memcpy(&distance, &bits, sizeof(distance));
        std::array<float, 3> hit{};
        if (aim.cursor_hit_off != 0) {
            ctx.memory.ReadBlock(rt.cursor_ptr + aim.cursor_hit_off, hit.data(), sizeof(hit));
        }
        LOG_INFO(Core, "VIZR hooks: cursor distance {:.2f} units ({:.2f} m) hit ({:.2f},{:.2f},{:.2f}) states {:#x}",
                 distance, distance / (rt.space.units_per_meter * g_user_settings.Load().world_scale), hit[0], hit[1],
                 hit[2], g_input_states.load(std::memory_order_relaxed));
    }
    if (!rt.hand_world_valid) {
        return;
    }
    // In the Scan Visor the ray is cast from the head (the camera the camera hook wrote): left alone, the game's ray
    // there follows the stick-driven body view, not the head.
    const bool from_head = rt.scan_state_mask != 0 && rt.camera_world_valid &&
                           (g_input_states.load(std::memory_order_relaxed) & rt.scan_state_mask) != 0;
    const u64 address = ctx.regs.x(ctx.regs.ctx, aim.ray_xform_reg) + static_cast<u64>(aim.ray_xform_off);
    if (!ctx.memory.IsValidVirtualAddressRange(address, 48)) {
        return;
    }
    std::array<float, 12> xf{};
    ctx.memory.ReadBlock(address, xf.data(), sizeof(xf));
    if (const u64 n = ++rt.ray_samples; n == 1 || n == 300 || n == 900) {
        // The convention check: the game's transform before ours, a few times into gameplay
        LOG_INFO(Core,
                 "VIZR hooks: cursor ray: game xf cols ({:.2f},{:.2f},{:.2f}) ({:.2f},{:.2f},{:.2f}) ({:.2f},{:.2f},{:.2f}) "
                 "pos ({:.2f},{:.2f},{:.2f}); camera cols2 ({:.2f},{:.2f},{:.2f}) pos ({:.2f},{:.2f},{:.2f})",
                 xf[0], xf[4], xf[8], xf[1], xf[5], xf[9], xf[2], xf[6], xf[10], xf[3], xf[7], xf[11],
                 rt.camera_world[2], rt.camera_world[6], rt.camera_world[10], rt.camera_world[3], rt.camera_world[7],
                 rt.camera_world[11]);
    }
    std::array<std::array<float, 3>, 3> axes = rt.hand_axes;
    std::array<float, 3> origin = rt.hand_world;
    if (from_head) {
        const auto& cw = rt.camera_world;
        const auto world = [&](const std::array<float, 3>& xr) { // OpenXR view-space vector -> world direction
            std::array<float, 3> local{}, out{};
            for (std::size_t i = 0; i < 3; ++i) {
                local[i] = rt.basis[i][0] * xr[0] + rt.basis[i][1] * xr[1] + rt.basis[i][2] * xr[2];
            }
            for (std::size_t i = 0; i < 3; ++i) {
                out[i] = cw[i * 4 + 0] * local[0] + cw[i * 4 + 1] * local[1] + cw[i * 4 + 2] * local[2];
            }
            return out;
        };
        const auto f = world({0.0f, 0.0f, -1.0f});
        const auto u = world({0.0f, 1.0f, 0.0f});
        axes = {std::array<float, 3>{u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]}, u, f};
        origin = {cw[3], cw[7], cw[11]};
    }
    const auto& [left, up, forward] = axes;
    for (std::size_t c = 0; c < 3; ++c) {
        std::array<float, 3> v{};
        const std::string& name = aim.ray_columns[c];
        if (name == "right") {
            v = {-left[0], -left[1], -left[2]};
        } else if (name == "left") {
            v = left;
        } else if (name == "up") {
            v = up;
        } else if (name == "forward") {
            v = forward;
        }
        for (std::size_t r = 0; r < 3; ++r) {
            xf[r * 4 + c] = v[r];
        }
    }
    xf[3] = origin[0];
    xf[7] = origin[1];
    xf[11] = origin[2];
    ctx.memory.WriteBlock(address, xf.data(), sizeof(xf));
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
}

std::array<float, 12> HeadRelative(const Runtime& rt, const std::array<float, 12>& xf, int mode);

/// (scan brackets) / (scan lens): a transform the game placed relative to its own camera (body yaw +
/// game pitch), placed the same way relative to the head. The reticle already lies on the line toward the target; only
/// its plane and orientation follow the camera. Translation P' = H + (P - H) * d / ((P - H) . F_H), d = (P - C) . F_C;
/// rotation R' = R_H * R_C^T * R (column lengths of R, the scale, carried through).
/// rigid (the scan lens: no world target, it is part of the helmet view): P' = H + F_H * |P - H|, on the head's forward
/// ray at the game's distance. The game's lens position already follows our head cursor ray, but with the reticle's own
/// lag; R_H R_C^T (P - C) turned it twice.
std::array<float, 12> HeadRelative(const Runtime& rt, const std::array<float, 12>& xf, int mode) {
    const auto& c = rt.game_camera;
    const auto& h = rt.camera_world;
    std::array<float, 3> fc{c[2], c[6], c[10]}, fh{h[2], h[6], h[10]};
    const auto unit = [](std::array<float, 3>& v) {
        const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (float& x : v) {
            x /= n > 1e-6f ? n : 1.0f;
        }
    };
    unit(fc);
    unit(fh);
    const std::array<float, 3> p{xf[3], xf[7], xf[11]};
    const std::array<float, 3> ph{p[0] - h[3], p[1] - h[7], p[2] - h[11]};
    const float d = (p[0] - c[3]) * fc[0] + (p[1] - c[7]) * fc[1] + (p[2] - c[11]) * fc[2];
    const float along = ph[0] * fh[0] + ph[1] * fh[1] + ph[2] * fh[2];
    std::array<float, 12> out = xf;
    if (mode == 1) {
        const std::array<float, 3> pc{p[0] - c[3], p[1] - c[7], p[2] - c[11]};
        std::array<float, 3> local{}; // R_C^T * (P - C)
        for (std::size_t k = 0; k < 3; ++k) {
            local[k] = c[0 * 4 + k] * pc[0] + c[1 * 4 + k] * pc[1] + c[2 * 4 + k] * pc[2];
        }
        for (std::size_t r = 0; r < 3; ++r) {
            out[r * 4 + 3] = h[r * 4 + 3] + h[r * 4 + 0] * local[0] + h[r * 4 + 1] * local[1] + h[r * 4 + 2] * local[2];
        }
    } else if (mode == 2) {
        const float dist = std::sqrt(ph[0] * ph[0] + ph[1] * ph[1] + ph[2] * ph[2]);
        for (std::size_t r = 0; r < 3; ++r) {
            out[r * 4 + 3] = h[r * 4 + 3] + fh[r] * dist;
        }
    } else if (along > 1e-4f && d > 0.0f) {
        for (std::size_t r = 0; r < 3; ++r) {
            out[r * 4 + 3] = h[r * 4 + 3] + ph[r] * d / along;
        }
    }
    std::array<float, 9> rc{}; // R_C^T * R
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t k = 0; k < 3; ++k) {
            float v = 0.0f;
            for (std::size_t i2 = 0; i2 < 3; ++i2) {
                v += c[i2 * 4 + r] * xf[i2 * 4 + k];
            }
            rc[r * 3 + k] = v;
        }
    }
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t k = 0; k < 3; ++k) {
            float v = 0.0f;
            for (std::size_t i2 = 0; i2 < 3; ++i2) {
                v += h[r * 4 + i2] * rc[i2 * 3 + k];
            }
            out[r * 4 + k] = v;
        }
    }
    return out;
}

/// The Scan Visor lens / range-ring sprites, at the queue functions' entries.
void RunScanLens(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& l = rt.profile.scan_lens;
    const u64 lr = ctx.regs.x(ctx.regs.ctx, 30);
    const auto module = rt.modules.find(l.module);
    std::size_t bucket = rt.lens_hits.size() - 1;
    if (module != rt.modules.end()) {
        for (std::size_t i = 0; i < l.callers.size() && i + 1 < rt.lens_hits.size(); ++i) {
            if (lr == module->second.first + l.callers[i]) {
                bucket = i;
            }
        }
    }
    const u64 n = ++rt.lens_hits[bucket];
    const u64 total = rt.lens_hits[0] + rt.lens_hits[1] + rt.lens_hits[2] + rt.lens_hits[3] + rt.lens_hits[4];
    if (total == 1 || total % 600 == 0) {
        LOG_INFO(Core, "VIZR hooks: scan lens: hits by caller [{}, {}, {}, {}], other {}; rewrites {} ({})",
                 rt.lens_hits[0], rt.lens_hits[1], rt.lens_hits[2], rt.lens_hits[3], rt.lens_hits[4], rt.lens_writes,
                 rt.lens_write ? "on" : "off: VIZR_LENS_REWRITE=0");
    }
    (void)n;
    if (bucket == rt.lens_hits.size() - 1 || !rt.lens_write || !rt.camera_world_valid || !rt.game_camera_valid) {
        return;
    }
    const u64 address = ctx.regs.x(ctx.regs.ctx, l.xform_reg);
    if (address == 0 || !ctx.memory.IsValidVirtualAddressRange(address, 48)) {
        return;
    }
    std::array<float, 12> xf{};
    ctx.memory.ReadBlock(address, xf.data(), sizeof(xf));
    if (address == rt.lens_last_at && xf == rt.lens_last) {
        return; // the same Draw's next sprite: already rewritten (rewriting again would turn it twice)
    }
    std::array<float, 12> out = xf;
    if (rt.lens_mode == 3) {
        // Fresh from the head: rotation = the head's columns (the sprite's own column lengths kept),
        // position = H + distance * F_H (the head centre: the sprites are queued once and drawn for both eyes).
        const auto& h = rt.camera_world;
        std::array<float, 3> fh{h[2], h[6], h[10]};
        const float fn = std::sqrt(fh[0] * fh[0] + fh[1] * fh[1] + fh[2] * fh[2]);
        for (std::size_t k = 0; k < 3; ++k) {
            const float len = std::sqrt(xf[k] * xf[k] + xf[4 + k] * xf[4 + k] + xf[8 + k] * xf[8 + k]);
            const float hl = std::sqrt(h[k] * h[k] + h[4 + k] * h[4 + k] + h[8 + k] * h[8 + k]);
            for (std::size_t r = 0; r < 3; ++r) {
                out[r * 4 + k] = h[r * 4 + k] * len / std::max(hl, 1e-6f);
            }
        }
        for (std::size_t r = 0; r < 3; ++r) {
            out[r * 4 + 3] = h[r * 4 + 3] + fh[r] / std::max(fn, 1e-6f) * l.distance;
        }
    } else {
        out = HeadRelative(rt, xf, rt.lens_mode);
    }
    if (rt.rig_logs && (rt.lens_writes == 0 || rt.lens_writes % 120 == 0)) {
        const auto& c = rt.game_camera;
        const auto& h = rt.camera_world;
        LOG_INFO(Core,
                 "VIZR hooks: scan lens xf mode {}: P ({:.2f},{:.2f},{:.2f}) -> ({:.2f},{:.2f},{:.2f}); cols ({:.2f},{:.2f},{:.2f}) "
                 "({:.2f},{:.2f},{:.2f}) ({:.2f},{:.2f},{:.2f}); C ({:.2f},{:.2f},{:.2f}) col2 ({:.2f},{:.2f},{:.2f}); "
                 "H ({:.2f},{:.2f},{:.2f}) col2 ({:.2f},{:.2f},{:.2f})",
                 rt.lens_mode, xf[3], xf[7], xf[11], out[3], out[7], out[11], xf[0], xf[4], xf[8], xf[1], xf[5], xf[9],
                 xf[2], xf[6], xf[10], c[3], c[7], c[11], c[2], c[6], c[10], h[3], h[7], h[11], h[2], h[6], h[10]);
    }
    ctx.memory.WriteBlock(address, out.data(), sizeof(out));
    rt.lens_last = out;
    rt.lens_last_at = address;
    ++rt.lens_writes;
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
}

u64 SlotSpan(const Profile::GrappleAim& g);

/// [lock_ring]: the lock-on ring toward the locked target from the head, with the head's rotation (as the scan lens).
/// With orbit_point (the ring's own target point): at that point's distance less surface_margin, never
/// nearer than `distance`: P = H + u * D, the 3x3 scaled by D * dot(u, F) / distance, so its size on screen is the
/// game's (it sized the ring for a point `distance` / dot(u, F) away). Otherwise toward the entity's position at the
/// plane `distance` ahead: P = H + u * distance / dot(u, F). Without a target position the game's placement is made
/// head-relative.
void PlaceLockRing(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& aim = rt.profile.aim;
    const auto& ring = rt.profile.lock_ring;
    const auto& g = rt.profile.grapple_aim;
    auto& mem = ctx.memory;
    const u64 address = ctx.regs.x(ctx.regs.ctx, aim.reticle_xform_reg);
    if (address == 0 || !mem.IsValidVirtualAddressRange(address, 48)) {
        return;
    }
    std::array<float, 12> xf{};
    mem.ReadBlock(address, xf.data(), sizeof(xf));
    auto out = HeadRelative(rt, xf, 0);
    const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
    std::optional<std::array<float, 3>> target;
    bool orbit_point = false;
    if (ring.orbit_point_reg >= 0) {
        const u64 at = ctx.regs.x(ctx.regs.ctx, static_cast<u32>(ring.orbit_point_reg)) + ring.orbit_point_off;
        if (mem.IsValidVirtualAddressRange(at, 12)) {
            std::array<float, 3> t{};
            mem.ReadBlock(at, t.data(), sizeof(t));
            if (std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]) && (t[0] != 0.0f || t[1] != 0.0f || t[2] != 0.0f)) {
                target = t;
                orbit_point = true;
            }
        }
    }
    if (!target && rt.orbit_player != 0 && rt.orbit_table != 0 &&
        mem.IsValidVirtualAddressRange(rt.orbit_player + ring.target_off, 4)) {
        const u32 id = mem.Read32(rt.orbit_player + ring.target_off);
        const u64 e = read64(rt.orbit_table + (id & g.id_index_mask) * g.stride);
        if (e != 0 && mem.IsValidVirtualAddressRange(e, SlotSpan(g)) && mem.Read32(e + g.id_check_off) == id) {
            u64 at = read64(e + g.entity_off);
            for (const u64 step : g.point_chain) {
                at = at != 0 ? read64(at + step) : 0;
            }
            if (at != 0 && mem.IsValidVirtualAddressRange(at, 48)) {
                std::array<float, 12> pt{};
                mem.ReadBlock(at, pt.data(), sizeof(pt));
                target = std::array<float, 3>{pt[3], pt[7], pt[11]};
            }
        }
    }
    if (target) {
        const auto& h = rt.camera_world;
        std::array<float, 3> f{h[2], h[6], h[10]};
        const float fn = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        const std::array<float, 3> eye{h[3], h[7], h[11]};
        std::array<float, 3> u{(*target)[0] - eye[0], (*target)[1] - eye[1], (*target)[2] - eye[2]};
        const float un = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (fn > 1e-6f && un > 1e-3f) {
            for (std::size_t k = 0; k < 3; ++k) {
                f[k] /= fn;
                u[k] /= un;
            }
            const float along = u[0] * f[0] + u[1] * f[1] + u[2] * f[2];
            if (along > 0.05f && orbit_point) { // in front of the head: at the target's depth
                const float d = std::max(un - ring.surface_margin, ring.distance);
                const float k = d * along / ring.distance;
                for (std::size_t r = 0; r < 3; ++r) {
                    for (std::size_t c = 0; c < 3; ++c) {
                        out[r * 4 + c] *= k;
                    }
                    out[r * 4 + 3] = eye[r] + u[r] * d;
                }
                rt.ring_depth = d;
            } else if (along > 0.05f) { // in front of the head
                for (std::size_t r = 0; r < 3; ++r) {
                    out[r * 4 + 3] = eye[r] + u[r] * ring.distance / along;
                }
            }
        }
    }
    mem.WriteBlock(address, out.data(), sizeof(out));
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
    if (rt.rig_logs && SteadyNowNs() - rt.ring_logged_ns > 1'000'000'000) {
        rt.ring_logged_ns = SteadyNowNs();
        LOG_INFO(Core, "VIZR hooks: lock ring at ({:.2f},{:.2f},{:.2f}) {}", out[3], out[7], out[11],
                 !target         ? std::string{"head-relative (no target position)"}
                 : orbit_point   ? fmt::format("at the orbit point's depth ({:.2f} units)", rt.ring_depth)
                                 : std::string{"toward the target"});
    }
}

/// M6 [aim]: the aiming reticle sits on the hand ray at the cursor's plane point; move it to the
/// hit point, scaled by the distance ratio from the eye so it keeps its size on screen. Other reticles (their
/// translation is not at the plane point) and the skipped caller are left alone.
void RunReticle(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& aim = rt.profile.aim;
    const u64 lr = ctx.regs.x(ctx.regs.ctx, 30);
    if (!aim.rotate_only_lr_offsets.empty() && rt.camera_world_valid && rt.game_camera_valid) {
        // Already at the object's depth (and sized by the game): only the facing is re-made relative to the head.
        const auto module = rt.modules.find(aim.rotate_only_lr_module);
        bool rotate_only = false;
        if (module != rt.modules.end()) {
            for (const u64 offset : aim.rotate_only_lr_offsets) {
                rotate_only |= lr == module->second.first + offset;
            }
        }
        if (rotate_only) {
            const u64 address = ctx.regs.x(ctx.regs.ctx, aim.reticle_xform_reg);
            if (address != 0 && ctx.memory.IsValidVirtualAddressRange(address, 48)) {
                std::array<float, 12> xf{};
                ctx.memory.ReadBlock(address, xf.data(), sizeof(xf));
                auto out = HeadRelative(rt, xf, 0);
                out[3] = xf[3];
                out[7] = xf[7];
                out[11] = xf[11];
                ctx.memory.WriteBlock(address, out.data(), sizeof(out));
                ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
    }
    if (const auto& ring = rt.profile.lock_ring; ring.present && rt.camera_world_valid && rt.game_camera_valid) {
        const auto module = rt.modules.find(ring.lr_module);
        bool is_ring = false;
        if (module != rt.modules.end()) {
            for (const u64 offset : ring.lr_offsets) {
                is_ring |= lr == module->second.first + offset;
            }
        }
        if (is_ring) {
            PlaceLockRing(ctx);
            return;
        }
    }
    if (!aim.head_reticle_lr_offsets.empty() && rt.camera_world_valid && rt.game_camera_valid) {
        // Otherwise the scan reticle moves on a vertical plane instead of following the gaze:
        // the game puts it relative to its own camera (body yaw + game pitch); put it the same way relative to the
        // head: xf' = H * C^-1 * xf (C rigid: C^-1 = [R^T | -R^T t]).
        const auto module = rt.modules.find(aim.reticle_lr_module);
        bool head_relative = false;
        if (module != rt.modules.end()) {
            for (const u64 offset : aim.head_reticle_lr_offsets) {
                head_relative |= lr == module->second.first + offset;
            }
        }
        if (head_relative) {
            const u64 address = ctx.regs.x(ctx.regs.ctx, aim.reticle_xform_reg);
            if (address == 0 || !ctx.memory.IsValidVirtualAddressRange(address, 48)) {
                return;
            }
            std::array<float, 12> xf{};
            ctx.memory.ReadBlock(address, xf.data(), sizeof(xf));
            auto out = HeadRelative(rt, xf, 0);
            // In the Scan Visor the brackets point at the head's pick when there is one, else at the gaze, and never
            // further from the gaze than the pick cone (the yellow circle's radius): left alone they travel far out of
            // sight.
            const auto& ga = rt.profile.grapple_aim;
            const bool scan = rt.scan_state_mask != 0 && ga.scan_max_angle_deg > 0.0f &&
                              (g_input_states.load(std::memory_order_relaxed) & rt.scan_state_mask) != 0;
            if (scan) {
                const auto& h = rt.camera_world;
                std::array<float, 3> fh{h[2], h[6], h[10]};
                const float fn = std::sqrt(fh[0] * fh[0] + fh[1] * fh[1] + fh[2] * fh[2]);
                const std::array<float, 3> eye{h[3], h[7], h[11]};
                const std::array<float, 3> placed{out[3] - eye[0], out[7] - eye[1], out[11] - eye[2]};
                const float plane = (placed[0] * fh[0] + placed[1] * fh[1] + placed[2] * fh[2]) / std::max(fn, 1e-6f);
                if (fn > 1e-6f && plane > 1e-4f) {
                    for (float& v : fh) {
                        v /= fn;
                    }
                    std::array<float, 3> u = fh; // no pick: the gaze
                    if (rt.scan_pick_valid) {
                        u = {rt.scan_pick_pos[0] - eye[0], rt.scan_pick_pos[1] - eye[1], rt.scan_pick_pos[2] - eye[2]};
                    }
                    const float un = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                    for (float& v : u) {
                        v /= std::max(un, 1e-6f);
                    }
                    const float max_rad = ga.scan_max_angle_deg * std::numbers::pi_v<float> / 180.0f;
                    const float c = std::clamp(u[0] * fh[0] + u[1] * fh[1] + u[2] * fh[2], -1.0f, 1.0f);
                    if (std::acos(c) > max_rad) {
                        // Clamp: the direction at the cone's edge, in the plane of the gaze and u.
                        std::array<float, 3> side{u[0] - c * fh[0], u[1] - c * fh[1], u[2] - c * fh[2]};
                        const float sn = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
                        for (std::size_t k = 0; k < 3; ++k) {
                            u[k] = std::cos(max_rad) * fh[k] + std::sin(max_rad) * (sn > 1e-6f ? side[k] / sn : 0.0f);
                        }
                    }
                    // The same depth plane as the game's placement (its size on screen is unchanged).
                    const float along = u[0] * fh[0] + u[1] * fh[1] + u[2] * fh[2];
                    if (rt.rig_logs) {
                        // Test: the widest bracket angle from the gaze each second, with or without a pick.
                        const float deg = std::acos(std::clamp(along, -1.0f, 1.0f)) * 57.29578f;
                        rt.bracket_max_deg = std::max(rt.bracket_max_deg, deg);
                        if (const s64 now = SteadyNowNs(); now - rt.bracket_logged_ns > 1'000'000'000) {
                            rt.bracket_logged_ns = now;
                            LOG_INFO(Core, "VIZR hooks: scan brackets: widest {:.1f} deg from the gaze (cone {:.1f}), {}",
                                     rt.bracket_max_deg, ga.scan_max_angle_deg, rt.scan_pick_valid ? "on the pick" : "no pick: gaze");
                            rt.bracket_max_deg = 0.0f;
                        }
                    }
                    for (std::size_t r = 0; r < 3; ++r) {
                        out[r * 4 + 3] = eye[r] + u[r] * plane / std::max(along, 1e-3f);
                    }
                }
            }
            if (aim.head_reticle_target_reg >= 0 && rt.locked) {
                // Locked: out to the target's depth plane, scaled about the eye (layout and size on screen unchanged),
                // never nearer than the game's own plane.
                const auto& h = rt.camera_world;
                std::array<float, 3> fh{h[2], h[6], h[10]};
                const float fn = std::sqrt(fh[0] * fh[0] + fh[1] * fh[1] + fh[2] * fh[2]);
                const std::array<float, 3> eye{h[3], h[7], h[11]};
                const u64 at = ctx.regs.x(ctx.regs.ctx, static_cast<u32>(aim.head_reticle_target_reg)) +
                               aim.head_reticle_target_off;
                std::array<float, 3> t{};
                if (fn > 1e-6f && ctx.memory.IsValidVirtualAddressRange(at, 12)) {
                    ctx.memory.ReadBlock(at, t.data(), sizeof(t));
                    const std::array<float, 3> to_t{t[0] - eye[0], t[1] - eye[1], t[2] - eye[2]};
                    const std::array<float, 3> to_p{out[3] - eye[0], out[7] - eye[1], out[11] - eye[2]};
                    const float dt = std::sqrt(to_t[0] * to_t[0] + to_t[1] * to_t[1] + to_t[2] * to_t[2]);
                    const float t_plane = (to_t[0] * fh[0] + to_t[1] * fh[1] + to_t[2] * fh[2]) / fn;
                    const float p_plane = (to_p[0] * fh[0] + to_p[1] * fh[1] + to_p[2] * fh[2]) / fn;
                    if (std::isfinite(dt) && dt > 1e-3f && t_plane > 0.05f && p_plane > 1e-3f) {
                        const float want = t_plane * std::max(dt - aim.head_reticle_target_margin, 0.0f) / dt;
                        const float s = std::max(want, p_plane) / p_plane;
                        for (std::size_t r = 0; r < 3; ++r) {
                            for (std::size_t c = 0; c < 3; ++c) {
                                out[r * 4 + c] *= s;
                            }
                            out[r * 4 + 3] = eye[r] + (out[r * 4 + 3] - eye[r]) * s;
                        }
                        if (rt.rig_logs && SteadyNowNs() - rt.head_reticle_logged_ns > 1'000'000'000) {
                            rt.head_reticle_logged_ns = SteadyNowNs();
                            LOG_INFO(Core, "VIZR hooks: scan reticle at the target's depth: plane {:.2f} -> {:.2f} units "
                                     "(target {:.2f} away)", p_plane, p_plane * s, dt);
                        }
                    }
                }
            }
            ctx.memory.WriteBlock(address, out.data(), sizeof(out));
            ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (rt.cursor_ptr == 0 || !rt.hand_world_valid || !rt.camera_world_valid) {
        return;
    }
    if (rt.reticle_lrs_seen.size() < 32 && std::ranges::find(rt.reticle_lrs_seen, lr) == rt.reticle_lrs_seen.end()) {
        rt.reticle_lrs_seen.push_back(lr);
        const auto main = rt.modules.find("main");
        LOG_INFO(Core, "VIZR hooks: reticle caller main+{:#x}", main != rt.modules.end() ? lr - main->second.first : lr);
    }
    if (!aim.reticle_skip_lr_module.empty()) {
        const auto module = rt.modules.find(aim.reticle_skip_lr_module);
        if (module != rt.modules.end() && lr == module->second.first + aim.reticle_skip_lr_offset) {
            return;
        }
    }
    bool by_caller = false;
    if (!aim.reticle_lr_module.empty()) {
        const auto module = rt.modules.find(aim.reticle_lr_module);
        if (module == rt.modules.end()) {
            return;
        }
        for (const u64 offset : aim.reticle_lr_offsets) {
            by_caller |= lr == module->second.first + offset;
        }
        if (!by_caller) {
            return; // not the aiming reticle (chosen by caller)
        }
    }
    const u64 address = ctx.regs.x(ctx.regs.ctx, aim.reticle_xform_reg);
    if (address == 0 || !ctx.memory.IsValidVirtualAddressRange(address, 48)) {
        return;
    }
    std::array<float, 12> xf{};
    std::array<float, 3> plane{}, hit{};
    ctx.memory.ReadBlock(address, xf.data(), sizeof(xf));
    ctx.memory.ReadBlock(rt.cursor_ptr + aim.cursor_on_plane_off, plane.data(), sizeof(plane));
    ctx.memory.ReadBlock(rt.cursor_ptr + aim.cursor_hit_off, hit.data(), sizeof(hit));
    const std::array<float, 3> t{xf[3], xf[7], xf[11]};
    const auto dist = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    };
    if (!by_caller && dist(t, plane) > 0.3f) {
        return; // not the aiming reticle (no caller list: by position)
    }
    const std::array<float, 3> eye{rt.camera_world[3], rt.camera_world[7], rt.camera_world[11]};
    const float plane_dist = dist(plane, eye);
    if (by_caller && rt.reticle_logs < 4) {
        ++rt.reticle_logs;
        LOG_INFO(Core, "VIZR hooks: aiming reticle t ({:.2f},{:.2f},{:.2f}) plane ({:.2f},{:.2f},{:.2f}) hit ({:.2f},{:.2f},{:.2f}) "
                 "eye ({:.2f},{:.2f},{:.2f}) xf at {:#x}",
                 t[0], t[1], t[2], plane[0], plane[1], plane[2], hit[0], hit[1], hit[2], eye[0], eye[1], eye[2], address);
    }
    if (plane_dist < 1e-3f) {
        return;
    }
    const float k = dist(hit, eye) / plane_dist;
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            xf[r * 4 + c] *= k;
        }
        xf[r * 4 + 3] = hit[r] + k * (t[r] - plane[r]);
    }
    ctx.memory.WriteBlock(address, xf.data(), sizeof(xf));
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
}

/// Outside gameplay the hand pose, the cursor and a pending snap turn are dropped, so
/// nothing from before a cutscene or room change is used after it.
void DropAimState(Runtime& rt) {
    rt.hand_world_valid = false;
    rt.gun_xf_valid = false; // And the arm solve
    rt.game_camera_valid = false;  // No reticle re-placed from a stale camera
    rt.camera_world_valid = false;
    rt.cursor_ptr = 0;
    rt.ray_seen = false;
    g_snap_request_deg.store(0.0f, std::memory_order_relaxed);
}

/// M6 [snap]: an instant snap turn: rotate the body transform's 3x3 about world up and flag it
/// for the next physics step. Skipped while orbiting, grappling or in the morph ball.
bool RotateBodyYaw(const HitContext& ctx, float degrees);

void RunSnap(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& snap = rt.profile.snap;
    const float degrees = g_snap_request_deg.exchange(0.0f, std::memory_order_relaxed);
    if (degrees == 0.0f) {
        return;
    }
    for (const std::string& skip : snap.skip_fields) {
        if (const auto value = ReadScalar(ctx, skip); value && *value != 0) {
            LOG_INFO(Core, "VIZR hooks: snap turn skipped ({} = {})", skip, *value);
            return;
        }
    }
    if (!RotateBodyYaw(ctx, degrees)) {
        return;
    }
    rt.snap_psi_before = rt.psi;
    rt.snap_check_frames = 6; // the snap result after the next physics steps
    LOG_INFO(Core, "VIZR hooks: snap turn {:+.0f} deg (body yaw written, physics push flagged; snap {})", degrees,
             ++rt.snaps);
}

/// The game body's yaw turned by `degrees` (+ = right) about world up, as the next physics step takes it (M6 snap turn,
/// Torso): rotate the [snap] yaw_field's 3x3 and set its push_field.
bool RotateBodyYaw(const HitContext& ctx, float degrees) {
    Runtime& rt = ctx.rt;
    const auto& snap = rt.profile.snap;
    const Field* yaw = rt.profile.FindField(snap.yaw_field);
    const Field* push = rt.profile.FindField(snap.push_field);
    FieldState* yaw_state = yaw ? StateOf(rt, yaw) : nullptr;
    FieldState* push_state = push ? StateOf(rt, push) : nullptr;
    const auto yaw_address = yaw_state ? FieldAddress(ctx, *yaw_state) : std::nullopt;
    const auto push_address = push_state ? FieldAddress(ctx, *push_state) : std::nullopt;
    if (!yaw_address || !push_address || yaw->type != FieldType::Mat3x4) {
        return false;
    }
    std::array<float, 12> xf{};
    ctx.memory.ReadBlock(*yaw_address, xf.data(), sizeof(xf));
    // R' = R_up(a) * R, up = the [space] up axis (Prime: Y).
    const float a = snap.sign * degrees * std::numbers::pi_v<float> / 180.0f;
    const float c = std::cos(a), s = std::sin(a);
    const auto up = Column(rt.basis, 1);
    // Rodrigues' rotation about the unit up vector.
    Mat3 rot{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            rot[i][j] = (i == j ? c : 0.0f) + (1.0f - c) * up[i] * up[j];
        }
    }
    rot[0][1] -= s * up[2];
    rot[0][2] += s * up[1];
    rot[1][0] += s * up[2];
    rot[1][2] -= s * up[0];
    rot[2][0] -= s * up[1];
    rot[2][1] += s * up[0];
    std::array<float, 12> out = xf;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            out[i * 4 + j] = rot[i][0] * xf[0 * 4 + j] + rot[i][1] * xf[1 * 4 + j] + rot[i][2] * xf[2 * 4 + j];
        }
    }
    ctx.memory.WriteBlock(*yaw_address, out.data(), sizeof(out));
    ctx.memory.Write8(*push_address, 1);
    return true;
}

/// [tick_rate]: the game's simulation step follows the Compatibility "Game frame rate": Match headset = the XR session's
/// refresh (g_display_hz; a rate the table lacks -> 90), else 60 / 90 / 120. All sites are written in one go and then
/// retranslated, so no tick mixes rates; a change in the menu applies at the next camera hook. 60 writes the originals.
/// Eden then composes at that rate (g_present_hz).
void ApplyTickRate(const HitContext& ctx);

void ApplyTickRate(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& t = rt.profile.tick_rate;
    if (!t.present || rt.tick_failed) {
        return;
    }
    const float setting = g_user_settings.Load().compat_game_fps;
    u32 want = static_cast<u32>(std::lround(setting));
    g_tick_match_max_hz.store(t.match_max, std::memory_order_relaxed);
    if (want == 0) {
        want = g_display_hz.load(std::memory_order_relaxed);
        if (want == 0) {
            return; // the session's refresh is not known yet
        }
        if (t.match_max != 0 && want > t.match_max) {
            if (rt.tick_capped_logged != want) {
                rt.tick_capped_logged = want;
                LOG_INFO(Core, "VIZR hooks: headset offered {} Hz; game tick 1/{} (Match headset is capped at {} Hz: a "
                               "fixed tick runs slow when frames dip)", want, t.match_max, t.match_max);
            }
            want = t.match_max;
        }
    }
    if (const u32 cap = g_tick_session_cap_hz.load(std::memory_order_relaxed); cap != 0 && want > cap) {
        want = cap; // the presenter's watchdog: the game's frames did not keep up at the higher rate this session
    }
    if (want != 60 && !t.rates.contains(want)) {
        want = t.rates.contains(90) ? 90 : 60;
    }
    if (want == (rt.tick_hz == 0 ? 60u : rt.tick_hz)) {
        if (rt.tick_hz == 0 && want == 60) {
            rt.tick_hz = 60;
        }
        return;
    }
    const auto module = rt.modules.find(t.module);
    if (module == rt.modules.end()) {
        rt.tick_failed = true;
        return;
    }
    auto& mem = ctx.memory;
    const u64 base = module->second.first;
    for (std::size_t i = 0; i < t.sites.size(); ++i) { // each site holds the original or one of the table's rates
        std::vector<u8> now(t.original[i].size());
        mem.ReadBlock(base + t.sites[i], now.data(), now.size());
        bool known = now == t.original[i];
        for (const auto& [hz, bytes] : t.rates) {
            known = known || now == bytes[i];
        }
        if (!known) {
            rt.tick_failed = true;
            LOG_ERROR(Core, "VIZR hooks: [tick_rate] site {} (+{:#x}) holds unexpected bytes; the game stays at its own rate",
                      i, t.sites[i]);
            return;
        }
    }
    // The table lists a cave before the branch into it: writing a rate goes in list order (the cave exists before the
    // branch), restoring the originals goes in reverse (the branch is gone before its cave is cleared).
    for (std::size_t k = 0; k < t.sites.size(); ++k) {
        const std::size_t i = want == 60 ? t.sites.size() - 1 - k : k;
        const auto& bytes = want == 60 ? t.original[i] : t.rates.at(want)[i];
        mem.WriteBlock(base + t.sites[i], bytes.data(), bytes.size());
    }
    for (std::size_t i = 0; i < t.sites.size(); ++i) {
        Core::InvalidateInstructionCacheRange(static_cast<const Kernel::KProcess*>(rt.process), base + t.sites[i],
                                              t.original[i].size());
    }
    rt.tick_hz = want;
    g_present_hz = want;
    // The pipeline depth (in game frames) the presenter pairs images and poses with, at this tick rate.
    u32 lag = rt.pose_lag_default;
    if (rt.pose_lag_at != nullptr) {
        if (const auto it = rt.pose_lag_at->find(want); it != rt.pose_lag_at->end()) {
            lag = it->second;
        }
    }
    g_hook_state.pose_lag_frames = lag;
    LOG_INFO(Core, "VIZR hooks: game tick set to 1/{} ({}; headset {} Hz); composing at {} Hz; pose lag {} frame(s)",
             want, setting < 0.5f ? "match headset" : "fixed", g_display_hz.load(std::memory_order_relaxed), want, lag);
}

/// [orbit_head] lock_field: while a lock holds (lock_min..lock_max) the game turns the body toward the target; lock_view is that turn
/// (LOCAL yaw, left +, from the body's facing when the lock began) and the view subtracts it, so the view stays with
/// the head. At the release the turn is handed to the torso (torso_applied), so the view does not jump and Body mode
/// turns the body back to the player's torso.
void RunLockView(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& oh = rt.profile.orbit_head;
    if (!oh.present || oh.lock_field.empty() || rt.last_camera_right_eye) {
        return;
    }
    const auto value = ReadScalar(ctx, oh.lock_field);
    const bool lock = value && static_cast<s64>(*value) >= oh.lock_min && static_cast<s64>(*value) <= oh.lock_max;
    const auto up = Column(rt.basis, 1);
    std::array<float, 3> b = rt.body_axis;
    const float along = b[0] * up[0] + b[1] * up[1] + b[2] * up[2];
    for (std::size_t i = 0; i < 3; ++i) {
        b[i] -= along * up[i];
    }
    const float bn = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (bn < 1e-4f || SteadyNowNs() - rt.body_axis_ns >= HeadFreshNs) {
        rt.lock_axis_prev_valid = false; // no fresh body (ball, passthrough): a lock's start must not span the gap
        return;
    }
    for (float& v : b) {
        v /= bn;
    }
    if (lock && !rt.locked) {
        // From the last frame without the lock: the game starts turning the body in the frame the lock begins, before
        // this hook first sees it (rig: 7.7 deg of a 17 deg turn), and that part is the lock's too.
        rt.locked = true;
        const bool prev_fresh = rt.lock_axis_prev_valid && SteadyNowNs() - rt.lock_axis_prev_ns < HeadFreshNs;
        rt.lock_axis0 = prev_fresh ? rt.lock_axis_prev : b;
        rt.lock_view = 0.0f;
    }
    if (!rt.locked) {
        rt.lock_axis_prev = b;
        rt.lock_axis_prev_valid = true;
        rt.lock_axis_prev_ns = SteadyNowNs();
    }
    if (rt.locked) {
        const auto& a0 = rt.lock_axis0;
        const std::array<float, 3> c{a0[1] * b[2] - a0[2] * b[1], a0[2] * b[0] - a0[0] * b[2], a0[0] * b[1] - a0[1] * b[0]};
        const float world = std::atan2(c[0] * up[0] + c[1] * up[1] + c[2] * up[2], a0[0] * b[0] + a0[1] * b[1] + a0[2] * b[2]);
        // RotateBodyYaw turns the body by world = sign * (-local): the same rule backwards.
        rt.lock_view = -rt.profile.snap.sign * world;
    }
    if (!lock && rt.locked) {
        rt.locked = false;
        if (!rt.lock_return) { // a return still under way keeps its first target
            rt.lock_return_target = rt.torso_applied;
            rt.lock_return = true;
        }
        rt.torso_applied += rt.lock_view;
        rt.torso_view += rt.lock_view;
        if (rt.rig_logs) {
            LOG_INFO(Core, "VIZR hooks: lock released: the game turned the body {:.1f} deg during the lock",
                     rt.lock_view * 57.29578f);
        }
        rt.lock_view = 0.0f;
    }
}

/// With Move forward
/// follows = Body the game body turns with the player's real torso (the presenter's continuous estimate), and
/// the view counts the head from the torso (HeadRotation), so the view does not move while the body re-aligns. A step
/// written here first shows in the game camera at the next tick's camera hook, which runs before this function: the
/// view subtracts torso_applied as it stands then. Off in the ball, cinematics, grapple and while frozen ([snap]
/// torso_skip_fields): the turn already applied stays, so nothing jumps. A recenter starts over from the body's current
/// heading.
void RunTorso(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const HeadPose head = g_latest_head.Load();
    if (head.recenter_count != rt.torso_recenter_count) {
        rt.torso_recenter_count = head.recenter_count;
        rt.torso_applied = 0.0f;
        rt.torso_view = 0.0f;
        rt.lock_return = false;
    }
    if (rt.last_camera_right_eye) {
        return; // the second eye's pass of the same frame
    }
    const s64 now = SteadyNowNs();
    const float dt = rt.torso_ns != 0 ? std::clamp(static_cast<float>(now - rt.torso_ns) * 1e-9f, 0.0f, 0.1f) : 0.0f;
    rt.torso_ns = now;
    const float mode = g_user_settings.Load().move_forward;
    if (!rt.body_camera) {
        g_torso_hold.store(true, std::memory_order_relaxed); // the ball, a path camera: the torso waits
        return;
    }
    // Body, or the body follows the head: the torso drives the body. In the other modes only a lock's turn is undone
    // (handed into torso_applied at the release, RunLockView): the body turns back to where it was, the view still.
    const bool torso_mode = (mode > 1.5f && mode < 2.5f) || mode > 3.5f;
    if (torso_mode) {
        rt.lock_return = false; // the torso's target includes it
        if (!head.valid || !head.torso_valid || now - head.sample_ns > HeadFreshNs) {
            return;
        }
    } else if (!rt.lock_return) {
        return;
    }
    bool hold = false;
    for (const std::string& skip : rt.profile.snap.torso_skip_fields) {
        if (const auto value = ReadScalar(ctx, skip); value && *value != 0) {
            hold = true;
            break;
        }
    }
    g_torso_hold.store(hold, std::memory_order_relaxed);
    if (hold) {
        return;
    }
    constexpr float Pi = std::numbers::pi_v<float>;
    const auto wrap = [](float a) {
        while (a > Pi) {
            a -= 2.0f * Pi;
        }
        while (a < -Pi) {
            a += 2.0f * Pi;
        }
        return a;
    };
    const float step = 2.0f * Pi * dt; // 360 deg/s: a safety limit only; the presenter's smoothing shapes the turn
    const float target = torso_mode ? head.torso_offset : rt.lock_return_target;
    const float d = std::clamp(wrap(target - rt.torso_applied), -step, step);
    if (std::abs(d) < 1e-4f) {
        rt.lock_return = false;
        return;
    }
    if (!RotateBodyYaw(ctx, -d * 57.29578f)) { // LOCAL yaw is left-positive; degrees are right
        return;
    }
    rt.torso_applied = wrap(rt.torso_applied + d);
    if (rt.rig_logs && now - rt.torso_logged_ns > 500'000'000) {
        rt.torso_logged_ns = now;
        LOG_INFO(Core, "VIZR hooks: torso: offset {:.1f} deg, body turned {:.1f} deg (this step {:+.2f})",
                 head.torso_offset * 57.29578f, rt.torso_applied * 57.29578f, d * 57.29578f);
    }
}

/// The address file's [states] at the camera hook, for the layout's [when].
/// What every passthrough clears, so the reticle, hand and arm consumers never keep the last
/// camera (a camera rule's passthrough, no match, the explicit flat state).
void EnterPassthrough(Runtime& rt) {
    rt.body_camera = false;
    rt.orbit_head_ready = false;
    g_torso_hold.store(true, std::memory_order_relaxed); // no body camera: the torso waits
    g_vignette.store(0.0f, std::memory_order_relaxed);
    rt.vig_prev_valid = false;
    DropAimState(rt);
}

/// False when a field the condition names is read at another hook (it cannot be read here).
bool FieldsReadableHere(const HitContext& ctx, const When& when) {
    const auto here = [&](const std::string& name) {
        const Field* field = name.empty() ? nullptr : ctx.rt.profile.FindField(name);
        return field == nullptr || field->hook.empty() || field->hook == ctx.hook.name;
    };
    if (!here(when.field) || !here(when.other_field)) {
        return false;
    }
    return std::ranges::all_of(when.children, [&](const When& c) { return FieldsReadableHere(ctx, c); });
}

void PublishStates(const HitContext& ctx) {
    u32 mask = 0;
    const u32 published = g_input_states.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < ctx.rt.profile.states.size(); ++i) {
        const auto& entity = ctx.rt.profile.state_entities[i];
        // A state whose fields are read at another hook keeps its last value here: published as false, it flipped
        // every tick between the hooks, and whichever the presenter saw last decided (the morph ball read as left
        // while still rolling, so standing up never pressed to leave it).
        const auto readable = [&](std::string_view name) {
            const Field* field = ctx.rt.profile.FindField(name);
            return field == nullptr || field->hook.empty() || field->hook == ctx.hook.name;
        };
        if (entity ? !readable(entity->id_field) || !readable(entity->table_field)
                   : !FieldsReadableHere(ctx, ctx.rt.profile.states[i].second)) {
            mask |= published & (1u << static_cast<u32>(i));
            if (ctx.rt.rig_logs && (published & (1u << static_cast<u32>(i))) != 0) {
                ++ctx.rt.states_held_set; // the old rule published this bit as false here
            }
            continue;
        }
        bool holds = false;
        if (entity) {
            // Id -> entry (table + (id & id_index_mask) * stride), checked, -> entity, vtable compare.
            const auto id = ReadScalar(ctx, entity->id_field);
            const Field* table_field = ctx.rt.profile.FindField(entity->table_field);
            FieldState* table_state = table_field ? StateOf(ctx.rt, table_field) : nullptr;
            const auto table = table_state ? FieldAddress(ctx, *table_state) : std::nullopt;
            const auto module = ctx.rt.modules.find(entity->vtable_module);
            if (id && table && module != ctx.rt.modules.end()) {
                const u64 slot = *table + (*id & entity->id_index_mask) * entity->stride;
                const u64 e = ctx.memory.IsValidVirtualAddressRange(slot, 8) ? ctx.memory.Read64(slot) : 0;
                if (e != 0 && ctx.memory.IsValidVirtualAddressRange(e, entity->entity_off + 8) &&
                    ctx.memory.Read32(e + entity->id_check_off) == static_cast<u32>(*id) &&
                    (ctx.memory.Read8(e + entity->flags_off) & entity->flags_mask) == entity->flags_value) {
                    const u64 object = ctx.memory.Read64(e + entity->entity_off);
                    holds = object != 0 && ctx.memory.IsValidVirtualAddressRange(object, 8) &&
                            ctx.memory.Read64(object) == module->second.first + entity->vtable_offset;
                }
            }
        } else {
            holds = Eval(ctx, ctx.rt.profile.states[i].second);
        }
        if (holds) {
            mask |= 1u << static_cast<u32>(i);
        }
    }
    if (const int flat = ctx.rt.profile.flat_state_index; flat >= 0 && ctx.hook.name != ctx.rt.profile.flat_state_hook) {
        // Its fields are read at flat_state_hook only (e.g. the pause flag at the frame tick): elsewhere keep its last
        // value, or the state would flip every frame between the two hooks.
        const u32 bit = 1u << static_cast<u32>(flat);
        mask = (mask & ~bit) | (g_input_states.load(std::memory_order_relaxed) & bit);
    }
    g_input_states.store(mask, std::memory_order_relaxed);
    if (ctx.rt.rig_logs) {
        if (mask != published) {
            LOG_INFO(Core, "VIZR rig: states {:#x} -> {:#x} at '{}'", published, mask, ctx.hook.name);
        }
        if (const s64 now_ns = SteadyNowNs(); now_ns - ctx.rt.states_held_logged_ns > 1'000'000'000) {
            ctx.rt.states_held_logged_ns = now_ns;
            if (ctx.rt.states_held_set != 0) {
                LOG_INFO(Core, "VIZR rig: states held at other hooks: {} set bit(s) the old rule read as false",
                         ctx.rt.states_held_set);
                ctx.rt.states_held_set = 0;
            }
        }
    }
    if (const int flat = ctx.rt.profile.flat_state_index; flat >= 0) {
        const bool now = ((mask >> flat) & 1u) != 0;
        if (g_hook_state.flat_state.exchange(now) != now) {
            // Which of its fields made it (every field the condition names, as read now; "-" = not readable here).
            std::string values;
            const auto collect = [&](auto&& self, const When& w) -> void {
                if (!w.field.empty() && w.op != When::Op::State) {
                    const auto v = ReadScalar(ctx, w.field);
                    values += fmt::format(" {}={}", w.field, v ? fmt::format("{:#x}", *v) : std::string{"-"});
                }
                for (const When& c : w.children) {
                    self(self, c);
                }
            };
            collect(collect, ctx.rt.profile.states[static_cast<std::size_t>(flat)].second);
            LOG_INFO(Core, "VIZR hooks: state '{}' {} (at '{}':{})", ctx.rt.profile.flat_state,
                     now ? "on -> flat screen" : "off", ctx.hook.name, values);
        }
        if (now) {
            EnterPassthrough(ctx.rt); // The same cleanup as every passthrough
        }
        if (now && g_hook_state.camera_active.exchange(false)) {
            g_hook_state.passthrough_events.fetch_add(1, std::memory_order_relaxed);
            LOG_INFO(Core, "VIZR hooks: state '{}' -> passthrough (event {})", ctx.rt.profile.flat_state,
                     g_hook_state.passthrough_events.load());
        }
    }
    g_fade_active.store((mask & ctx.rt.fade_mask) != 0, std::memory_order_relaxed); // Comfort fade
    for (const auto& [name, value] : ctx.rt.rig_writes) { // Test runs only (see Install)
        const Field* field = ctx.rt.profile.FindField(name);
        FieldState* state = field ? StateOf(ctx.rt, field) : nullptr;
        if (const auto address = state ? FieldAddress(ctx, *state) : std::nullopt) {
            ctx.memory.Write32(*address, value);
            if (!ctx.rt.rig_writes_logged) {
                LOG_WARNING(Core, "VIZR hooks: rig write {} = {} (at {:#x})", name, value, *address);
            }
        }
    }
    ctx.rt.rig_writes_logged = ctx.rt.rig_writes_logged || !ctx.rt.rig_writes.empty();
    if (ctx.rt.snap_check_frames > 0 && --ctx.rt.snap_check_frames == 0) {
        float moved = ctx.rt.psi - ctx.rt.snap_psi_before;
        while (moved > std::numbers::pi_v<float>) {
            moved -= 2.0f * std::numbers::pi_v<float>;
        }
        while (moved < -std::numbers::pi_v<float>) {
            moved += 2.0f * std::numbers::pi_v<float>;
        }
        LOG_INFO(Core, "VIZR hooks: snap result: game yaw (psi) moved {:+.1f} deg", moved * 57.29578f);
    }
    g_game_yaw.store(ctx.rt.psi, std::memory_order_relaxed);
    g_game_yaw_ns.store(SteadyNowNs(), std::memory_order_relaxed);
}

namespace ArmMath {
using V3 = std::array<float, 3>;
using Q = std::array<float, 4>; // w, x, y, z
inline Q Mul(const Q& a, const Q& b) {
    return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3], a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1], a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}
inline Q Conj(const Q& q) {
    return {q[0], -q[1], -q[2], -q[3]};
}
inline V3 Rot(const Q& q, const V3& v) {
    const Q p = Mul(Mul(q, Q{0.0f, v[0], v[1], v[2]}), Conj(q));
    return {p[1], p[2], p[3]};
}
inline V3 Norm(V3 v) {
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (float& x : v) {
        x /= n > 1e-6f ? n : 1.0f;
    }
    return v;
}
/// The shortest rotation taking unit vector a onto unit vector b.
inline Q Between(const V3& a, const V3& b) {
    const V3 c{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    Q q{1.0f + d, c[0], c[1], c[2]};
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-6f) {
        return {0.0f, 0.0f, 1.0f, 0.0f}; // opposite: half turn about an axis normal to x (good enough here)
    }
    for (float& x : q) {
        x /= n;
    }
    return q;
}
/// The rotation whose columns are the unit, right-handed axes x, y, z (Shepperd's method).
inline Q FromBasis(const V3& x, const V3& y, const V3& z) {
    const float tr = x[0] + y[1] + z[2];
    Q q{};
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q = {0.25f * s, (y[2] - z[1]) / s, (z[0] - x[2]) / s, (x[1] - y[0]) / s};
    } else if (x[0] > y[1] && x[0] > z[2]) {
        const float s = std::sqrt(1.0f + x[0] - y[1] - z[2]) * 2.0f;
        q = {(y[2] - z[1]) / s, 0.25f * s, (y[0] + x[1]) / s, (z[0] + x[2]) / s};
    } else if (y[1] > z[2]) {
        const float s = std::sqrt(1.0f + y[1] - x[0] - z[2]) * 2.0f;
        q = {(z[0] - x[2]) / s, (y[0] + x[1]) / s, 0.25f * s, (z[1] + y[2]) / s};
    } else {
        const float s = std::sqrt(1.0f + z[2] - x[0] - y[1]) * 2.0f;
        q = {(x[1] - y[0]) / s, (z[0] + x[2]) / s, (z[1] + y[2]) / s, 0.25f * s};
    }
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& c : q) {
        c /= n;
    }
    return q;
}
} // namespace ArmMath

/// The right arm, at the pose build of the arm character, before the pose solve
/// propagates parent-local coords. The cannon write already put the rig (and the cannon) at the hand; the visible
/// upper arm is a separate model that hangs off the elbow node and turns with it. So: turn the elbow until its rest
/// upper-arm direction (elbow -> shoulder node, this frame's pose) points at the body's shoulder, and give the
/// cannon node the opposite turn so the cannon does not move. Nothing is stretched: only rotations change (plus the
/// cannon node's small offset, re-expressed in the turned elbow frame).
void RunArmIk(const HitContext& ctx, u64 chr, u64 pose) {
    using namespace ArmMath;
    Runtime& rt = ctx.rt;
    const auto& a = rt.profile.arms;
    if (!rt.gun_xf_valid || g_user_settings.Load().arm_follow < 0.5f) {
        return; // menu "Arm follows body" off: the game's own arm
    }
    auto& mem = ctx.memory;
    const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
    const u64 table = read64(pose + a.coords_table_at);
    u64 set = chr;
    for (const u64 step : a.node_set_chain) {
        set = read64(set + step);
    }
    if (table == 0 || set == 0 || !mem.IsValidVirtualAddressRange(set + a.count_at, 2)) {
        return;
    }
    const u32 count = mem.Read16(set + a.count_at);
    if (a.shoulder_node >= count || a.elbow_node >= count || a.gun_node >= count) {
        return;
    }
    const auto coords_of = [&](u32 node) -> u64 {
        const u64 id_at = set + a.ids_at + node * 4;
        if (!mem.IsValidVirtualAddressRange(id_at, 4)) {
            return 0;
        }
        const u32 id = mem.Read32(id_at);
        const u64 at = read64(table + (id & 0x1f) * 8) + ((id >> 5) & 0x7ff) * 8 + 8;
        return at > 8 && mem.IsValidVirtualAddressRange(at, 40) ? at : 0;
    };
    const u64 s_at = coords_of(a.shoulder_node);
    const u64 e_at = coords_of(a.elbow_node);
    const u64 g_at = coords_of(a.gun_node);
    if (s_at == 0 || e_at == 0 || g_at == 0) {
        return;
    }
    std::array<float, 10> sc{}, ec{}, gc{};
    mem.ReadBlock(s_at, sc.data(), sizeof(sc));
    mem.ReadBlock(e_at, ec.data(), sizeof(ec));
    mem.ReadBlock(g_at, gc.data(), sizeof(gc));
    // Solving from a pose that already holds our output turns the elbow again on every call (a spin), and the
    // animation rebuilds these coords only now and then. So the
    // solve always starts from the animation's own values: taken fresh whenever the pose does not hold our last
    // output, otherwise the remembered ones. Same input, same output: nothing compounds, and the hand is followed.
    if (!rt.ik_base_valid || std::memcmp(&ec[0], rt.ik_last_elbow.data(), sizeof(rt.ik_last_elbow)) != 0) {
        rt.ik_base_e = ec;
        rt.ik_base_g = gc;
        rt.ik_base_valid = true;
    } else {
        ec = rt.ik_base_e;
        gc = rt.ik_base_g;
        ++rt.ik_skips; // counts solves from the remembered base
    }
    const Q qs{sc[0], sc[1], sc[2], sc[3]};
    const Q qe{ec[0], ec[1], ec[2], ec[3]};
    const Q qg{gc[0], gc[1], gc[2], gc[3]};
    const V3 ts{sc[7], sc[8], sc[9]};
    const V3 te{ec[7], ec[8], ec[9]};
    const V3 tg{gc[7], gc[8], gc[9]};
    // Model space (the shoulder's parent is the root, identity at the model origin).
    const V3 rel = Rot(qs, te);
    const V3 elbow{ts[0] + rel[0], ts[1] + rel[1], ts[2] + rel[2]};
    const Q qe_abs = Mul(qs, qe);
    // The body's shoulder in model space: inverse of the model transform (columns may carry a scale).
    const auto& m = rt.gun_xf;
    const V3 d{rt.shoulder_world[0] - m[3], rt.shoulder_world[1] - m[7], rt.shoulder_world[2] - m[11]};
    const float a00 = m[0], a01 = m[1], a02 = m[2], a10 = m[4], a11 = m[5], a12 = m[6], a20 = m[8], a21 = m[9],
                a22 = m[10];
    const float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
    if (std::abs(det) < 1e-9f) {
        return;
    }
    V3 body{((a11 * a22 - a12 * a21) * d[0] - (a01 * a22 - a02 * a21) * d[1] + (a01 * a12 - a02 * a11) * d[2]) / det,
            (-(a10 * a22 - a12 * a20) * d[0] + (a00 * a22 - a02 * a20) * d[1] - (a00 * a12 - a02 * a10) * d[2]) / det,
            ((a10 * a21 - a11 * a20) * d[0] - (a00 * a21 - a01 * a20) * d[1] + (a00 * a11 - a01 * a10) * d[2]) / det};
    for (std::size_t k = 0; k < 3; ++k) {
        body[k] *= a.pose_signs[k]; // pose space vs the model transform (Prime: the barrel along -Z)
    }
    // The upper arm's rest direction (elbow -> shoulder) and where it should point (elbow -> the body's shoulder).
    const float sgn = a.upper_arm_sign;
    const V3 rest_dir = Norm({sgn * (ts[0] - elbow[0]), sgn * (ts[1] - elbow[1]), sgn * (ts[2] - elbow[2])});
    const V3 want_dir = Norm({body[0] - elbow[0], body[1] - elbow[1], body[2] - elbow[2]});
    const Q turn = Between(rest_dir, want_dir);
    const Q qe_abs2 = Mul(turn, qe_abs);
    const Q qe2 = Mul(Conj(qs), qe_abs2);
    // The cannon keeps its model-space rotation and position under the turned elbow.
    const Q qg2 = Mul(Mul(Conj(qe_abs2), qe_abs), qg);
    const V3 tg2 = Rot(Mul(Conj(qe_abs2), qe_abs), tg);
    const std::array<float, 4> e_rot{qe2[0], qe2[1], qe2[2], qe2[3]};
    const std::array<float, 4> g_rot{qg2[0], qg2[1], qg2[2], qg2[3]};
    mem.WriteBlock(e_at, e_rot.data(), sizeof(e_rot));
    rt.ik_last_elbow = e_rot;
    mem.WriteBlock(g_at, g_rot.data(), sizeof(g_rot));
    mem.WriteBlock(g_at + 0x1c, tg2.data(), sizeof(tg2));
    if (a.dirty_or != 0 && mem.IsValidVirtualAddressRange(pose + a.dirty_at, 1)) {
        mem.Write8(pose + a.dirty_at, static_cast<u8>(mem.Read8(pose + a.dirty_at) | a.dirty_or));
    }
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
    const u64 n = ++rt.ik_solves;
    if (a.ik_log == "frames" && n <= 15000) {
        // The stability series: the elbow's model-space rotation per applied solve and its change from the last one.
        const Q prev{rt.ik_last_abs[0], rt.ik_last_abs[1], rt.ik_last_abs[2], rt.ik_last_abs[3]};
        const Q delta = Mul(qe_abs2, Conj(prev));
        const float step = 2.0f * std::acos(std::clamp(std::abs(delta[0]), 0.0f, 1.0f)) * 57.29578f;
        const float turn_deg = 2.0f * std::acos(std::clamp(std::abs(turn[0]), 0.0f, 1.0f)) * 57.29578f;
        LOG_INFO(Core, "VIZR hooks: arm ik frame {}: turn {:.1f} deg, elbow abs ({:.3f},{:.3f},{:.3f},{:.3f}), "
                 "step {:.2f} deg, base reuses {}",
                 n, turn_deg, qe_abs2[0], qe_abs2[1], qe_abs2[2], qe_abs2[3], step, rt.ik_skips);
    }
    rt.ik_last_abs = {qe_abs2[0], qe_abs2[1], qe_abs2[2], qe_abs2[3]};
    if (a.ik_log == "once" && (n == 1 || n % 600 == 0)) {
        const float angle = 2.0f * std::acos(std::clamp(std::abs(turn[0]), 0.0f, 1.0f)) * 57.29578f;
        LOG_INFO(Core,
                 "VIZR hooks: arm ik {}: elbow ({:.3f},{:.3f},{:.3f}) rest upper arm ({:.2f},{:.2f},{:.2f}) -> body "
                 "shoulder dir ({:.2f},{:.2f},{:.2f}) at {:.3f}, turn {:.1f} deg (model units)",
                 n, elbow[0], elbow[1], elbow[2], rest_dir[0], rest_dir[1], rest_dir[2], want_dir[0], want_dir[1],
                 want_dir[2],
                 std::sqrt((body[0] - elbow[0]) * (body[0] - elbow[0]) + (body[1] - elbow[1]) * (body[1] - elbow[1]) +
                           (body[2] - elbow[2]) * (body[2] - elbow[2])),
                 angle);
    }
}

bool mem_ok_pose(const HitContext& ctx, u64 chr, const Profile::Arms& a) {
    return chr != 0 && ctx.memory.IsValidVirtualAddressRange(chr + a.pose_off_at, 2);
}

/// The arm solve inside the pose solve, after the animation rebuilt the parent-local coords.
/// Speed and direction of the feet, per camera tick, for the procedural legs.
void UpdateGait(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& a = rt.profile.arms;
    const Field* feet = rt.profile.FindField(a.body_feet_field);
    FieldState* state = feet ? StateOf(rt, feet) : nullptr;
    const auto address = state ? FieldAddress(ctx, *state) : std::nullopt;
    if (!address || feet->type != FieldType::Mat3x4 || !ctx.memory.IsValidVirtualAddressRange(*address, 48)) {
        rt.feet_prev_valid = false;
        return;
    }
    std::array<float, 12> xf{};
    ctx.memory.ReadBlock(*address, xf.data(), sizeof(xf));
    const std::array<float, 3> p{xf[3], xf[7], xf[11]};
    const s64 now = SteadyNowNs();
    rt.gait_air = false;
    if (!a.body_air_field.empty()) {
        rt.gait_air = ReadScalar(ctx, a.body_air_field).value_or(0) != 0;
    }
    if (rt.feet_prev_valid && now > rt.feet_prev_ns) {
        const float dt = static_cast<float>(now - rt.feet_prev_ns) * 1e-9f;
        const auto up = Column(rt.basis, 1);
        std::array<float, 3> d{p[0] - rt.feet_prev[0], p[1] - rt.feet_prev[1], p[2] - rt.feet_prev[2]};
        const float vertical = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        for (std::size_t i = 0; i < 3; ++i) {
            d[i] -= vertical * up[i];
        }
        const float step = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dt > 0.0f && dt < 0.25f && step < 5.0f) { // a teleport or a long gap is no step
            const float speed = step / dt;
            rt.gait_speed += (speed - rt.gait_speed) * std::min(1.0f, dt / 0.1f); // 0.1 s smoothing
            if (step > 1e-4f) {
                // body forward = the feet xf's [arms] body_feet_forward axis, side = up x forward (the body's left)
                const auto& v = a.body_feet_forward; // The parsed vector, never the key string
                std::array<float, 3> f{};
                for (std::size_t r = 0; r < 3; ++r) {
                    f[r] = xf[r * 4] * v[0] + xf[r * 4 + 1] * v[1] + xf[r * 4 + 2] * v[2];
                }
                const std::array<float, 3> l{up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2],
                                             up[0] * f[1] - up[1] * f[0]};
                rt.gait_forward = (d[0] * f[0] + d[1] * f[1] + d[2] * f[2]) / step;
                rt.gait_side = (d[0] * l[0] + d[1] * l[1] + d[2] * l[2]) / step;
            }
            const float cycle = a.body_stride * std::clamp(rt.gait_speed / a.body_walk_speed, 1.0f, 1.4f);
            rt.gait_phase = std::fmod(rt.gait_phase + 2.0f * std::numbers::pi_v<float> * rt.gait_speed / cycle * dt,
                                      2.0f * std::numbers::pi_v<float>);
        }
        if (a.body_leg_log && rt.rig_logs && now - rt.gait_log_ns > 1'000'000'000) {
            rt.gait_log_ns = now;
            LOG_INFO(Core, "VIZR hooks: gait speed {:.2f} u/s forward {:+.2f} side {:+.2f} phase {:.2f} air {}",
                     rt.gait_speed, rt.gait_forward, rt.gait_side, rt.gait_phase, rt.gait_air);
        }
    }
    rt.feet_prev = p;
    rt.feet_prev_ns = now;
    rt.feet_prev_valid = true;
}

/// Hips swing, knees bend, ankles keep the sole flat, composed onto the animation's own
/// parent-local rotations (taken fresh unless the node still holds our last output, as the arm).
void RunLegs(const HitContext& ctx, const std::function<u64(u32)>& coords_of) {
    using namespace ArmMath;
    Runtime& rt = ctx.rt;
    const auto& a = rt.profile.arms;
    auto& mem = ctx.memory;
    constexpr float deg = std::numbers::pi_v<float> / 180.0f;
    const float walk01 = std::clamp((rt.gait_speed - a.body_still_speed) / (a.body_walk_speed - a.body_still_speed), 0.0f, 1.0f);
    const float run01 = std::clamp((rt.gait_speed - a.body_walk_speed) / (a.body_run_speed - a.body_walk_speed), 0.0f, 1.0f);
    const float swing_amp = (25.0f + 15.0f * run01) * deg * walk01;
    const float knee_amp = (35.0f + 25.0f * run01) * deg * walk01;
    const auto axis_angle = [](const V3& axis, float angle) {
        const V3 n = Norm(axis);
        const float s = std::sin(angle * 0.5f);
        return Q{std::cos(angle * 0.5f), n[0] * s, n[1] * s, n[2] * s};
    };
    for (std::size_t leg = 0; leg < 2; ++leg) {
        const float phase = rt.gait_phase + (leg == 0 ? 0.0f : std::numbers::pi_v<float>);
        float swing = swing_amp * std::sin(phase) * rt.gait_forward;
        float side = 0.5f * swing_amp * std::sin(phase) * rt.gait_side;
        float knee = knee_amp * std::max(0.0f, std::sin(phase + 0.5f * std::numbers::pi_v<float>));
        if (rt.gait_air) {
            swing = 20.0f * deg;
            side = 0.0f;
            knee = 40.0f * deg;
        }
        // The crouch folds both legs (hips flex, knees bend twice that, ankles follow) so the feet stay down.
        swing += a.body_crouch_hip_sign * rt.crouch_alpha;
        knee += 2.0f * rt.crouch_alpha;
        const std::array<Q, 3> extra{Mul(axis_angle(a.body_swing_axis, swing), axis_angle(a.body_side_axis, side)),
                                     axis_angle(a.body_knee_axis, knee), axis_angle(a.body_knee_axis, -0.5f * knee)};
        for (std::size_t j = 0; j < 3; ++j) {
            const std::size_t k = leg * 3 + j;
            const u64 at = coords_of(a.body_legs[k]);
            if (at == 0) {
                continue;
            }
            Q q{};
            mem.ReadBlock(at, q.data(), sizeof(q));
            if (!rt.leg_base_valid[k] || std::memcmp(q.data(), rt.leg_last[k].data(), sizeof(q)) != 0) {
                rt.leg_base[k] = q; // the animation wrote this node since our last output
                rt.leg_base_valid[k] = true;
            }
            const Q out = Mul(rt.leg_base[k], extra[j]);
            mem.WriteBlock(at, out.data(), sizeof(out));
            rt.leg_last[k] = out;
        }
    }
}

/// Inside the pose solve, after the parent-local program and before the absolute one: the
/// body's hidden nodes get scale 0 (coords: rotation 4 floats, scale 3, translation 3).
void RunBodyPose(const HitContext& ctx, u64 chr, u64 pose) {
    Runtime& rt = ctx.rt;
    const auto& a = rt.profile.arms;
    if (!rt.body_camera) {
        return; // Any other view (cutscene, ball, passthrough) shows the body whole, as the game made it
    }
    auto& mem = ctx.memory;
    const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
    const u64 table = read64(pose + a.coords_table_at);
    u64 set = chr;
    for (const u64 step : a.node_set_chain) {
        set = read64(set + step);
    }
    if (table == 0 || set == 0 || !mem.IsValidVirtualAddressRange(set + a.count_at, 2)) {
        return;
    }
    const u32 count = mem.Read16(set + a.count_at);
    const std::function<u64(u32)> coords_of = [&](u32 node) -> u64 {
        const u64 id_at = set + a.ids_at + node * 4;
        if (node >= count || !mem.IsValidVirtualAddressRange(id_at, 4)) {
            return 0;
        }
        const u32 id = mem.Read32(id_at);
        const u64 at = read64(table + (id & 0x1f) * 8) + ((id >> 5) & 0x7ff) * 8 + 8;
        return at > 8 && mem.IsValidVirtualAddressRange(at, 40) ? at : 0;
    };
    // The standing stance, the crouch drop, the planted feet and the torso lean from the head.
    const UserSettings user = g_user_settings.Load();
    const float to_model = rt.space.units_per_meter * user.world_scale / std::max(a.body_model_scale, 1e-3f);
    float drop = 0.0f;              // model units, along body_model_up
    std::array<float, 3> lean{};    // model space
    std::array<float, 3> plant{};   // model space
    // Room (recentered: x right, y up, z back; m) -> model space through the profile's model axes.
    const auto room_to_model = [&](float right, float back) {
        std::array<float, 3> v{};
        for (std::size_t i = 0; i < 3; ++i) {
            v[i] = (-right * a.body_model_left[i] - back * a.body_model_forward[i]) * to_model;
        }
        return v;
    };
    const bool follow = !a.body_stand.empty() && user.body_follow >= 0.5f; // menu (test, off by default)
    rt.crouch_alpha = 0.0f;
    if (follow) {
        using namespace ArmMath;
        Q above{1.0f, 0.0f, 0.0f, 0.0f}; // the live rotations above the upright node (the game's anchors)
        for (const u32 node : a.body_upright_chain) {
            if (const u64 at = coords_of(node); at != 0) {
                Q q{};
                mem.ReadBlock(at, q.data(), sizeof(q));
                above = Mul(above, q);
            }
        }
        for (const auto& s : a.body_stand) {
            if (const u64 at = coords_of(s.node); at != 0) {
                Q q = s.rot;
                if (!a.body_upright_chain.empty() && s.node == a.body_upright_node) {
                    q = Mul(Conj(above), q); // the rest rotation in model space, whatever the anchors hold
                }
                mem.WriteBlock(at, q.data(), sizeof(q));
            }
        }
        if (rt.head_room_valid) {
            const auto& h = rt.head_room; // recentered room frame: x right, y up, z back (m)
            const std::array<float, 2> head_xz{h[0], h[2]};
            const float dx = head_xz[0] - rt.body_plant[0], dz = head_xz[1] - rt.body_plant[1];
            if (std::sqrt(dx * dx + dz * dz) > a.body_step_m) {
                rt.body_plant = head_xz; // a step: the feet re-plant under the head
            }
            // room (x right, z back) -> model (+x left, +z forward)
            plant = room_to_model(rt.body_plant[0], rt.body_plant[1]);
            lean = room_to_model(head_xz[0] - rt.body_plant[0], head_xz[1] - rt.body_plant[1]);
            drop = std::clamp(-h[1] * to_model, 0.0f, a.body_crouch_max);
        } else {
            rt.body_plant = {0.0f, 0.0f};
        }
        const float reach = std::clamp((a.body_leg_length - drop) / a.body_leg_length, 0.0f, 1.0f);
        rt.crouch_alpha = std::acos(reach);
    }
    if (!follow && !a.body_stand.empty() && rt.head_room_valid) {
        // The game's stance (the standing one is off): a crouch still lowers the body and folds its legs, so the
        // lowered view never ends up inside the chest.
        drop = std::clamp(-rt.head_room[1] * to_model, 0.0f, a.body_crouch_max);
        rt.crouch_alpha = std::acos(std::clamp((a.body_leg_length - drop) / a.body_leg_length, 0.0f, 1.0f));
    }
    if (a.body_legs.size() == 6) {
        RunLegs(ctx, coords_of);
    }
    if (!a.body_stand.empty()) {
        using namespace ArmMath;
        const float lean_len = follow ? std::sqrt(lean[0] * lean[0] + lean[1] * lean[1] + lean[2] * lean[2]) : 0.0f;
        if (lean_len > 1e-4f && !a.body_lean_nodes.empty()) {
            // Model-space turn taking up toward the lean (axis = up x lean), split evenly over the lean nodes top down;
            // each node's share is taken into its parent's frame (which already holds the shares above it), so the
            // Chest gets the whole angle with a smooth bend.
            const float angle = std::atan2(lean_len, a.body_torso_height) / static_cast<float>(a.body_lean_nodes.size());
            const auto& u = a.body_model_up;
            const V3 axis = Norm({u[1] * lean[2] - u[2] * lean[1], u[2] * lean[0] - u[0] * lean[2],
                                  u[0] * lean[1] - u[1] * lean[0]});
            const float s = std::sin(angle * 0.5f);
            const Q turn{std::cos(angle * 0.5f), axis[0] * s, axis[1] * s, axis[2] * s};
            const bool forward_order = a.body_lean_order >= 0.0f;
            Q parent{1.0f, 0.0f, 0.0f, 0.0f}; // the current node's parent, in model space
            for (const u32 node : a.body_lean_chain) {
                if (const u64 at = coords_of(node); at != 0) {
                    Q q{};
                    mem.ReadBlock(at, q.data(), sizeof(q));
                    parent = forward_order ? Mul(parent, q) : Mul(q, parent);
                }
            }
            for (const u32 node : a.body_lean_nodes) {
                const u64 at = coords_of(node);
                if (at == 0) {
                    break;
                }
                Q q{};
                mem.ReadBlock(at, q.data(), sizeof(q));
                const Q out = forward_order ? Mul(Mul(Mul(Conj(parent), turn), parent), q)
                                            : Mul(Mul(Mul(parent, turn), Conj(parent)), q);
                mem.WriteBlock(at, out.data(), sizeof(out));
                parent = forward_order ? Mul(parent, out) : Mul(out, parent);
                if (rt.rig_logs && SteadyNowNs() - rt.lean_log_ns > 3'000'000'000) {
                    LOG_INFO(Core, "VIZR hooks: body lean node {}: rel ({:.3f},{:.3f},{:.3f},{:.3f}) -> ({:.3f},{:.3f},{:.3f},{:.3f})",
                             node, q[0], q[1], q[2], q[3], out[0], out[1], out[2], out[3]);
                }
            }
            if (rt.rig_logs && SteadyNowNs() - rt.lean_log_ns > 3'000'000'000) {
                rt.lean_log_ns = SteadyNowNs();
            }
        }
        const float size = std::clamp(user.body_height, 0.5f, 1.5f);
        for (std::size_t k = 0; k < a.body_root_nodes.size() && k < rt.root_base.size(); ++k) {
            const u64 at = coords_of(a.body_root_nodes[k]);
            if (at == 0) {
                continue;
            }
            std::array<float, 3> t{};
            if (follow) {
                for (std::size_t i = 0; i < 3; ++i) {
                    t[i] = a.body_root_rest[i] * size + plant[i] - drop * a.body_model_up[i];
                }
            } else {
                // The game's own stance, sized: from the animation's translation (taken fresh unless it still holds
                // our last output, as the arm and the legs: nothing compounds).
                std::array<float, 3> now{};
                mem.ReadBlock(at + 28, now.data(), sizeof(now));
                if (!rt.root_base_valid[k] || std::memcmp(now.data(), rt.root_last[k].data(), sizeof(now)) != 0) {
                    rt.root_base[k] = now;
                    rt.root_base_valid[k] = true;
                }
                for (std::size_t i = 0; i < 3; ++i) {
                    t[i] = rt.root_base[k][i] * size - drop * a.body_model_up[i];
                }
            }
            const std::array<float, 3> scale{size, size, size};
            mem.WriteBlock(at + 16, scale.data(), sizeof(scale));
            mem.WriteBlock(at + 28, t.data(), sizeof(t));
            rt.root_last[k] = t;
        }
        if (rt.rig_logs && SteadyNowNs() - rt.body_log_ns > 1'000'000'000) {
            rt.body_log_ns = SteadyNowNs();
            LOG_INFO(Core, "VIZR hooks: body: head room ({:+.2f},{:+.2f},{:+.2f}) m plant ({:+.2f},{:+.2f}) m "
                     "lean {:.3f} drop {:.3f} model, hip flex {:.1f} deg",
                     rt.head_room[0], rt.head_room[1], rt.head_room[2], rt.body_plant[0], rt.body_plant[1], lean_len,
                     drop, rt.crouch_alpha * 57.29578f);
        }
    }
    static constexpr std::array<float, 3> zero{};
    std::vector<u32> hide = a.body_hide_nodes;
    if (g_user_settings.Load().show_body < 0.5f) { // menu "Show body" off
        hide.insert(hide.end(), a.body_off_nodes.begin(), a.body_off_nodes.end());
    }
    // Collapse onto the grandparent's joint. node joint = grandparent + R_gp (t_parent +
    // R_parent t_node), so t_node = -R_parent^-1 t_parent puts it on the grandparent's joint, whatever the pose.
    if (rt.body_camera) {
        using namespace ArmMath;
        for (const auto& c : a.body_collapse) {
            const u64 at = coords_of(c.node);
            const u64 parent_at = coords_of(c.parent);
            if (at == 0 || parent_at == 0) {
                continue;
            }
            std::array<float, 10> pc{};
            mem.ReadBlock(parent_at, pc.data(), sizeof(pc));
            const Q pq{pc[0], pc[1], pc[2], pc[3]};
            // The parent's matrix is R S, so t_node = -(R S)^-1 t_parent = -S^-1 R^T t_parent.
            const V3 back = Rot(Conj(pq), V3{pc[7], pc[8], pc[9]});
            std::array<float, 3> t{};
            for (std::size_t i = 0; i < 3; ++i) {
                t[i] = -back[i] / std::max(std::abs(pc[4 + i]), 1e-4f);
            }
            mem.WriteBlock(at + 16, zero.data(), sizeof(zero));
            mem.WriteBlock(at + 28, t.data(), sizeof(t));
        }
    }
    for (const u32 node : hide) {
        if (const u64 at = coords_of(node); at != 0) {
            mem.WriteBlock(at + 16, zero.data(), sizeof(zero));
            if (a.body_hide_to_parent) {
                mem.WriteBlock(at + 28, zero.data(), sizeof(zero)); // collapse onto the parent's joint
            }
        }
    }
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
}

/// The left arm as two bones, inside its pose solve (after the animation rebuilt
/// the parent-local coords). In the arm model's space (the inverse of the body-fixed transform we wrote at its render
/// entry): the shoulder stays where it is; the shoulder and the elbow turn so the wrist reaches the left controller
/// (clamped to the arm's reach), bending in the plane through the animation's own elbow. Like the right arm, the solve
/// always starts from the animation's values (never from our last output), so nothing compounds.
void RunLeftArmIk(const HitContext& ctx, u64 chr, u64 pose) {
    using namespace ArmMath;
    Runtime& rt = ctx.rt;
    const auto& a = rt.profile.arms;
    const HandPose hand = g_left_hand.Load();
    if (!rt.left_xf_valid || !hand.valid || SteadyNowNs() - hand.sample_ns > HeadFreshNs ||
        g_user_settings.Load().arm_follow < 0.5f) {
        return;
    }
    auto& mem = ctx.memory;
    const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
    const u64 table = read64(pose + a.coords_table_at);
    u64 set = chr;
    for (const u64 step : a.node_set_chain) {
        set = read64(set + step);
    }
    if (table == 0 || set == 0 || !mem.IsValidVirtualAddressRange(set + a.count_at, 2)) {
        return;
    }
    const u32 count = mem.Read16(set + a.count_at);
    if (a.left_shoulder_node >= count || a.left_elbow_node >= count || a.left_wrist_node >= count) {
        return;
    }
    const auto coords_of = [&](u32 node) -> u64 {
        const u64 id_at = set + a.ids_at + node * 4;
        if (!mem.IsValidVirtualAddressRange(id_at, 4)) {
            return 0;
        }
        const u32 id = mem.Read32(id_at);
        const u64 at = read64(table + (id & 0x1f) * 8) + ((id >> 5) & 0x7ff) * 8 + 8;
        return at > 8 && mem.IsValidVirtualAddressRange(at, 40) ? at : 0;
    };
    const u64 s_at = coords_of(a.left_shoulder_node);
    const u64 e_at = coords_of(a.left_elbow_node);
    const u64 w_at = coords_of(a.left_wrist_node);
    if (s_at == 0 || e_at == 0 || w_at == 0) {
        return;
    }
    std::array<float, 10> sc{}, ec{}, wc{};
    mem.ReadBlock(s_at, sc.data(), sizeof(sc));
    mem.ReadBlock(e_at, ec.data(), sizeof(ec));
    mem.ReadBlock(w_at, wc.data(), sizeof(wc));
    if (!rt.lik_base_valid || std::memcmp(&sc[0], rt.lik_last_s.data(), sizeof(rt.lik_last_s)) != 0 ||
        std::memcmp(&ec[0], rt.lik_last_e.data(), sizeof(rt.lik_last_e)) != 0) {
        rt.lik_base_s = sc;
        rt.lik_base_e = ec;
        rt.lik_base_valid = true;
    } else {
        sc = rt.lik_base_s;
        ec = rt.lik_base_e;
    }
    const Q qs{sc[0], sc[1], sc[2], sc[3]};
    const Q qe{ec[0], ec[1], ec[2], ec[3]};
    const V3 ts{sc[7], sc[8], sc[9]};
    const V3 te{ec[7], ec[8], ec[9]};
    // The wrist's translation is lengthened when the arm stretches; a node the game did not rebuild
    // still holds that write, so it is re-stretched from the kept original, never from itself.
    if (!rt.lik_tw_valid || std::memcmp(&wc[7], rt.lik_last_tw.data(), sizeof(rt.lik_last_tw)) != 0) {
        rt.lik_base_tw = {wc[7], wc[8], wc[9]};
        rt.lik_tw_valid = true;
    }
    const V3 tw = rt.lik_base_tw;
    const auto add = [](const V3& p, const V3& q) { return V3{p[0] + q[0], p[1] + q[1], p[2] + q[2]}; };
    const auto sub = [](const V3& p, const V3& q) { return V3{p[0] - q[0], p[1] - q[1], p[2] - q[2]}; };
    const auto mul = [](const V3& p, float k) { return V3{p[0] * k, p[1] * k, p[2] * k}; };
    const auto dot = [](const V3& p, const V3& q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; };
    const auto len = [&](const V3& p) { return std::sqrt(dot(p, p)); };
    // The shoulder's parent Skeleton_Root is NOT the identity: it is the
    // active anchor's abs transform P (animated: free-look, fidget, idle), copied by the abs program's op 0x0b from the
    // abs entry *(u32)(pose + 0x14). The rel coords are P-local; the controller is solved in P's frame.
    const u32 anchor_id = mem.IsValidVirtualAddressRange(pose + a.left_anchor_id_off, 4) ? mem.Read32(pose + a.left_anchor_id_off)
                                                                                         : 0;
    const u64 abs_banks = read64(pose + a.left_abs_banks_off);
    const u64 p_at = abs_banks != 0 ? read64(abs_banks + (anchor_id & 0x1f) * 8) + ((anchor_id >> 5) & 0x7ff) * 8 + 8 : 0;
    if (p_at <= 8 || !mem.IsValidVirtualAddressRange(p_at, 48)) {
        return;
    }
    std::array<float, 12> P{};
    mem.ReadBlock(p_at, P.data(), sizeof(P));
    // a 3x4 row-major point transform and its full inverse (P may scale)
    const auto apply = [](const std::array<float, 12>& x, const V3& v) {
        return V3{x[0] * v[0] + x[1] * v[1] + x[2] * v[2] + x[3], x[4] * v[0] + x[5] * v[1] + x[6] * v[2] + x[7],
                  x[8] * v[0] + x[9] * v[1] + x[10] * v[2] + x[11]};
    };
    const auto inverse = [](const std::array<float, 12>& x, std::array<float, 12>& out) {
        const float det = x[0] * (x[5] * x[10] - x[6] * x[9]) - x[1] * (x[4] * x[10] - x[6] * x[8]) +
                          x[2] * (x[4] * x[9] - x[5] * x[8]);
        if (std::abs(det) < 1e-9f) {
            return false;
        }
        const float i = 1.0f / det;
        out = {(x[5] * x[10] - x[6] * x[9]) * i, (x[2] * x[9] - x[1] * x[10]) * i, (x[1] * x[6] - x[2] * x[5]) * i, 0.0f,
               (x[6] * x[8] - x[4] * x[10]) * i, (x[0] * x[10] - x[2] * x[8]) * i, (x[2] * x[4] - x[0] * x[6]) * i, 0.0f,
               (x[4] * x[9] - x[5] * x[8]) * i, (x[1] * x[8] - x[0] * x[9]) * i, (x[0] * x[5] - x[1] * x[4]) * i, 0.0f};
        for (std::size_t r = 0; r < 3; ++r) {
            out[r * 4 + 3] = -(out[r * 4 + 0] * x[3] + out[r * 4 + 1] * x[7] + out[r * 4 + 2] * x[11]);
        }
        return true;
    };
    std::array<float, 12> Pi{};
    if (!inverse(P, Pi)) {
        return;
    }
    // P-local frame: the shoulder at ts, the elbow at ts + Rot(qs, te).
    const V3 S = ts;
    const Q qe_abs = Mul(qs, qe);
    // The controller in model space (the inverse of the arm transform we wrote; columns may carry a scale), then in P's.
    // The drawn transform: our +0x920 x Scale(S).
    std::array<float, 12> m = rt.left_xf;
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t r = 0; r < 3; ++r) {
            m[r * 4 + c] *= rt.left_S[c];
        }
    }
    std::array<float, 12> mi{};
    if (!inverse(m, mi)) {
        return;
    }
    // The wrist's target: the controller minus the grip offset (wrist -> palm) in the controller's own axes, so the
    // PALM point lands on the controller (the player's sliders move it along the hand).
    const auto user = g_user_settings.Load();
    const V3 grip{a.left_grip_m[0], a.left_grip_m[1] + user.left_hand_up_m, a.left_grip_m[2] - user.left_hand_forward_m};
    const HandWorld hw = HandToWorld(rt, hand, {-grip[0], -grip[1], -grip[2]});   // the wrist joint's target
    const HandWorld palm = HandToWorld(rt, hand, {0.0f, 0.0f, 0.0f});             // the controller = the palm's target
    V3 Tm = apply(mi, V3{hw.position[0], hw.position[1], hw.position[2]});
    for (std::size_t k = 0; k < 3; ++k) {
        Tm[k] *= a.left_pose_signs[k];
    }
    const V3 T = apply(Pi, Tm);
    // The shoulder in model space, for the next frame's arm placement (LeftArmBodyXf): P * ts, not ts.
    rt.lik_shoulder_pose = apply(P, ts);
    rt.lik_shoulder_valid = true;
    // No solve where the body is not the view.
    if (!rt.body_camera) {
        return;
    }
    // Two bones: upper arm L1 (shoulder -> elbow), forearm L2 (elbow -> wrist).
    const float l1 = len(te), l2 = len(tw);
    rt.lik_model_reach = l1 + l2; // the arm placement scales the drawn arm to the player's length from this
    const V3 to_t = sub(T, S);
    const float raw_d = len(to_t);
    if (l1 < 1e-4f || l2 < 1e-4f || raw_d < 1e-4f) {
        return;
    }
    // At least 10 deg of elbow flexion; beyond that reach both bones stretch in proportion, so the wrist is ON the
    // controller in every frame.
    constexpr float MinFlexRad = 10.0f * 3.14159265f / 180.0f;
    const float max_reach = std::sqrt(l1 * l1 + l2 * l2 + 2.0f * l1 * l2 * std::cos(MinFlexRad));
    if (raw_d > 2.25f * max_reach) {
        return; // The reach gate: a target this far is not the hand (spawn, a tracking loss): the animation's arm
    }
    const float stretch = raw_d > max_reach ? raw_d / max_reach : 1.0f;
    const float L1 = l1 * stretch, L2 = l2 * stretch;
    const V3 te_s = mul(te, stretch), tw_s = mul(tw, stretch);
    const V3 E = add(S, Rot(qs, te_s));
    const float d = std::clamp(raw_d, std::abs(L1 - L2) + 1e-4f, L1 + L2 - 1e-4f);
    const V3 dir = mul(to_t, 1.0f / raw_d);
    // The elbow's swivel: a down-and-out hint (the left elbow hangs down and to the left, a little back: the common
    // VR heuristic), in model space (columns left, up, forward) taken into P's frame.
    const V3 hint_m{0.5f * a.left_pose_signs[0], -1.0f * a.left_pose_signs[1], -0.3f * a.left_pose_signs[2]};
    const V3 hint{Pi[0] * hint_m[0] + Pi[1] * hint_m[1] + Pi[2] * hint_m[2], Pi[4] * hint_m[0] + Pi[5] * hint_m[1] + Pi[6] * hint_m[2],
                  Pi[8] * hint_m[0] + Pi[9] * hint_m[1] + Pi[10] * hint_m[2]};
    V3 pole = sub(hint, mul(dir, dot(hint, dir)));
    if (len(pole) < 1e-5f) {
        pole = sub(sub(E, S), mul(dir, dot(sub(E, S), dir))); // the hand along the hint: the animation's bend plane
    }
    if (len(pole) < 1e-5f) {
        pole = Norm(V3{dir[2], 0.0f, -dir[0]});
    } else {
        pole = Norm(pole);
    }
    const float along = (L1 * L1 - L2 * L2 + d * d) / (2.0f * d);
    const float out = std::sqrt(std::max(0.0f, L1 * L1 - along * along));
    const V3 E2 = add(add(S, mul(dir, along)), mul(pole, out));
    const V3 T2 = add(S, mul(dir, d)); // the target (reached exactly unless it sits inside |L1 - L2| of the shoulder)
    if (rt.rig_logs) {
        // Test measurement: what this solve intends, in model space (P x the P-frame points), for the compose hook.
        rt.lik_intended = {apply(P, S), apply(P, E2), apply(P, T2)};
        rt.lik_intended_valid = true;
        rt.lik_target_world = {hw.position[0], hw.position[1], hw.position[2]};
    }
    const Q turn1 = Between(Norm(sub(E, S)), Norm(sub(E2, S)));
    const Q qs2 = Mul(turn1, qs);
    const Q qe_abs1 = Mul(turn1, qe_abs);
    const V3 W1 = add(E2, Rot(qe_abs1, tw_s));
    const Q turn2 = Between(Norm(sub(W1, E2)), Norm(sub(T2, E2)));
    const Q qe_abs2 = Mul(turn2, qe_abs1);
    const Q qe2 = Mul(Conj(qs2), qe_abs2);
    // The hand takes the controller's
    // orientation whatever the forearm does. Wrist-local axes (the grappleArm bind): fingers -Z, back of the hand -X,
    // index side +Y. For the left hand in its grip the fingers point along the controller's forward and the back of the
    // hand faces the controller's left, so local X = the controller's right, Y = its up, Z = its back, turned by the
    // profile's grip rotation; taken into model space (the drawn transform, scale removed), then P's frame.
    const auto norm3 = [&](const V3& v) { return Norm(v); };
    const auto cross = [](const V3& p, const V3& q) {
        return V3{p[1] * q[2] - p[2] * q[1], p[2] * q[0] - p[0] * q[2], p[0] * q[1] - p[1] * q[0]};
    };
    const V3 c_up = norm3(V3{palm.up[0], palm.up[1], palm.up[2]});
    const V3 c_fwd = norm3(V3{palm.forward[0], palm.forward[1], palm.forward[2]});
    const V3 c_right = norm3(mul(V3{palm.left[0], palm.left[1], palm.left[2]}, -1.0f));
    const V3 c_back = mul(c_fwd, -1.0f);
    const auto& gd = a.left_grip_rot_deg;
    const float gp = gd[0] / 57.29578f, gy = gd[1] / 57.29578f, gr = gd[2] / 57.29578f;
    // R_grip = Ry(yaw) Rx(pitch) Rz(roll) in the controller's (right, up, back) frame; its columns in that frame.
    const float cp = std::cos(gp), sp = std::sin(gp), cy = std::cos(gy), sy = std::sin(gy), cr = std::cos(gr),
                sr = std::sin(gr);
    const std::array<V3, 3> g_cols{V3{cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr},
                                   V3{-cy * sr + sy * sp * cr, cp * cr, sy * sr + cy * sp * cr},
                                   V3{sy * cp, -sp, cy * cp}};
    std::array<V3, 3> want_w{};
    for (std::size_t c = 0; c < 3; ++c) {
        want_w[c] = add(add(mul(c_right, g_cols[c][0]), mul(c_up, g_cols[c][1])), mul(c_back, g_cols[c][2]));
    }
    const auto lin = [](const std::array<float, 12>& x, const V3& v) {
        return V3{x[0] * v[0] + x[1] * v[1] + x[2] * v[2], x[4] * v[0] + x[5] * v[1] + x[6] * v[2],
                  x[8] * v[0] + x[9] * v[1] + x[10] * v[2]};
    };
    std::array<V3, 3> want_p{};
    for (std::size_t c = 0; c < 3; ++c) {
        V3 v = norm3(lin(mi, want_w[c]));
        for (std::size_t k = 0; k < 3; ++k) {
            v[k] *= a.left_pose_signs[k];
        }
        want_p[c] = norm3(lin(Pi, v));
    }
    const bool proper = a.left_pose_signs[0] * a.left_pose_signs[1] * a.left_pose_signs[2] > 0.0f; // no mirror
    const V3 wz = want_p[2];
    const V3 wx = norm3(sub(want_p[0], mul(wz, dot(want_p[0], wz))));
    const V3 wy = cross(wz, wx);
    const Q q_want = FromBasis(wx, wy, wz);
    const Q qw2 = Mul(Conj(qe_abs2), q_want);
    if (rt.rig_logs) {
        rt.lik_want_fwd_world = norm3(mul(want_w[2], -1.0f)); // local -Z
        rt.lik_want_back_world = norm3(mul(want_w[0], -1.0f)); // local -X
        if (!rt.lik_index_logged && a.left_wrist_node + 1 < count) {
            // The live sign check, L_Index_1's rel translation ~ (-0.003, 0.039, -0.112).
            rt.lik_index_logged = true;
            if (const u64 i_at = coords_of(a.left_wrist_node + 1); i_at != 0) {
                std::array<float, 3> it{};
                mem.ReadBlock(i_at + 7 * sizeof(float), it.data(), sizeof(it));
                LOG_INFO(Core, "VIZR hooks: left hand: L_Index_1 rel translation ({:.3f},{:.3f},{:.3f}), expected about "
                         "(-0.003, 0.039, -0.112)", it[0], it[1], it[2]);
            }
        }
    }
    const std::array<float, 4> s_rot{qs2[0], qs2[1], qs2[2], qs2[3]};
    const std::array<float, 4> e_rot{qe2[0], qe2[1], qe2[2], qe2[3]};
    mem.WriteBlock(s_at, s_rot.data(), sizeof(s_rot));
    mem.WriteBlock(e_at, e_rot.data(), sizeof(e_rot));
    if (proper) {
        const std::array<float, 4> w_rot{qw2[0], qw2[1], qw2[2], qw2[3]}; // rotation only; the wrist's scale and t kept
        mem.WriteBlock(w_at, w_rot.data(), sizeof(w_rot));
    }
    if (stretch > 1.0f) {
        // CCoords: rotation (4), scale (3), translation (3): the bones' rel translations, lengthened in proportion
        // (rebuilt by the rel program every solve, so nothing accumulates).
        mem.WriteBlock(e_at + 7 * sizeof(float), te_s.data(), sizeof(te_s));
        mem.WriteBlock(w_at + 7 * sizeof(float), tw_s.data(), sizeof(tw_s));
    }
    // The grapple device's locators (children of L_elbow): the clip's rel coords, logged on the rig
    // (b52-rel-log: constant, the rest values).
    const auto& dw = rt.profile.left_window;
    std::array<std::array<float, 10>, 4> rel_seen{};
    std::size_t rel_n = 0;
    if (!dw.device_rel_ids.empty() && rt.rig_logs) {
        for (std::size_t k = 0; k < dw.device_rel_ids.size() && k < rel_seen.size(); ++k) {
            const u32 id = dw.device_rel_ids[k];
            const u64 at = read64(table + (id & 0x1f) * 8) + ((id >> 5) & 0x7ff) * 8 + 8;
            if (at <= 8 || !mem.IsValidVirtualAddressRange(at, 40)) {
                continue;
            }
            mem.ReadBlock(at, rel_seen[k].data(), sizeof(rel_seen[k]));
            rel_n = k + 1;
        }
    }
    // The compose hook (after the abs program) captures our elbow from this solve, not a tick-time compose.
    rt.dev_solve_pending = rt.left_window;
    rt.lik_last_s = s_rot;
    rt.lik_last_e = e_rot;
    rt.lik_last_tw = stretch > 1.0f ? tw_s : tw; // what the node holds now (the comparison above)
    if (a.dirty_or != 0 && mem.IsValidVirtualAddressRange(pose + a.dirty_at, 1)) {
        mem.Write8(pose + a.dirty_at, static_cast<u8>(mem.Read8(pose + a.dirty_at) | a.dirty_or));
    }
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
    const u64 n = ++rt.lik_solves;
    if (n == 1 || n % (rt.rig_logs ? 60 : 600) == 0) { // test: every second (the arm sweep holds each pose ~10 s)
        // The decisive check: the solved wrist back through P, the signs and the arm transform, against the controller.
        const V3 W2 = add(E2, Rot(qe_abs2, tw_s));
        V3 Wm = apply(P, W2);
        for (std::size_t k = 0; k < 3; ++k) {
            Wm[k] *= a.left_pose_signs[k];
        }
        const V3 Ww = apply(m, Wm);
        // The palm point = the solved wrist + the grip offset (controller axes) vs the controller, in metres.
        const V3 off{palm.position[0] - hw.position[0], palm.position[1] - hw.position[1], palm.position[2] - hw.position[2]};
        const float world_err_m = len(sub(add(Ww, off), V3{palm.position[0], palm.position[1], palm.position[2]})) /
                                  std::max(rt.space.units_per_meter * g_user_settings.Load().world_scale, 1e-3f);
        LOG_INFO(Core,
                 "VIZR hooks: left arm ik {}: anchor id {:#x} P [{:.2f} {:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f} {:.2f} | "
                 "{:.2f} {:.2f} {:.2f} {:.2f}]; shoulder (model) ({:.3f},{:.3f},{:.3f}); hand target (P) ({:.3f},{:.3f},{:.3f}) "
                 "at {:.3f} (reach {:.3f}, stretch {:.2f}, elbow {:.0f} deg, S {:.2f}); wrist error {:.4f} pose units, palm {:.3f} m from the controller; "
                 "torso offset {:.1f} applied {:.1f} view {:.1f} deg, neck vs body {:.1f} deg, d {:.3f} vs L1+L2 {:.3f}{}",
                 n, anchor_id, P[0], P[1], P[2], P[3], P[4], P[5], P[6], P[7], P[8], P[9], P[10], P[11],
                 rt.lik_shoulder_pose[0], rt.lik_shoulder_pose[1], rt.lik_shoulder_pose[2], T[0], T[1], T[2], raw_d,
                 max_reach, stretch,
                 std::acos(std::clamp(dot(Norm(sub(S, E2)), Norm(sub(T2, E2))), -1.0f, 1.0f)) * 57.29578f, rt.left_S[0],
                 len(sub(W2, T2)), world_err_m, g_latest_head.Load().torso_offset * 57.29578f,
                 rt.torso_applied * 57.29578f, rt.torso_view * 57.29578f, NeckBodyAngleDeg(rt), raw_d, L1 + L2,
                 raw_d > L1 + L2 - 1e-4f ? " (CLAMPED)" : "");
        if (rt.rig_logs && rel_n != 0) {
            std::string rels;
            for (std::size_t k = 0; k < rel_n; ++k) {
                const auto& c = rel_seen[k];
                rels += fmt::format(" {:#06x} q ({:.4f},{:.4f},{:.4f},{:.4f}) s ({:.3f},{:.3f},{:.3f}) t ({:.4f},{:.4f},{:.4f});",
                                    dw.device_rel_ids[k], c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9]);
            }
            // Cross-check (Reviewer 6d495a1): the rel-id formula for L_elbow (0x01e1) must be the slot our elbow write uses.
            const u64 e_by_id = read64(table + (0x01e1u & 0x1f) * 8) + ((0x01e1u >> 5) & 0x7ff) * 8 + 8;
            LOG_INFO(Core, "VIZR hooks: left device rels (the clip's; elbow id 0x01e1 {} our elbow slot):{}",
                     e_by_id == e_at ? "IS" : "is NOT", rels);
        }
    }
}

/// The left arm's draw window and the rig's order probe.
void LeftOrderEvent(Runtime& rt, char kind, u32 caller = 0, u32 caller2 = 0, u32 caller3 = 0) {
    if (rt.order_rec && rt.order_n < rt.order_ev.size()) {
        rt.order_ev[rt.order_n++] = {kind, caller, caller2, caller3, rt.camera_writes};
    }
}

void LeftWindowOpen(const HitContext& ctx, char kind) {
    Runtime& rt = ctx.rt;
    const auto& w = rt.profile.left_window;
    if (!w.present) {
        return;
    }
    if (kind == 'R' && w.probe && rt.rig_logs) {
        // A frame's events run from one Render entry to the next: the previous frame's render-path solves, then the
        // tick-time solves before this draw.
        if (rt.order_rec && rt.order_n > 0) {
            std::string events;
            const u64 base = rt.order_ev[0].cam_writes;
            for (u32 i = 0; i < rt.order_n; ++i) {
                const auto& e = rt.order_ev[i];
                events += fmt::format(" {}{}", e.kind, e.cam_writes - base);
                if (e.kind == 'S' || e.kind == 's') {
                    events += fmt::format("@{:#x}/{:#x}/{:#x}", e.caller, e.caller2, e.caller3);
                }
            }
            LOG_INFO(Core, "VIZR rig: left order (solves in the window {}, outside {} since the last line;{} ik only in "
                     "the window):{}", rt.left_window_solves, rt.left_other_solves, w.ik_window_only ? "" : " not",
                     events);
            rt.left_window_solves = rt.left_other_solves = 0;
        }
        const s64 now = SteadyNowNs();
        rt.order_rec = now - rt.order_last_ns >= 1'000'000'000;
        if (rt.order_rec) {
            rt.order_last_ns = now;
        }
        rt.order_n = 0;
    }
    rt.left_window = true;
    ++rt.lw_opens[kind == 'P' ? 0 : kind == 'R' ? 1 : 2];
    LeftOrderEvent(rt, kind);
}

/// A left-arm solve: inside the window or not; the probe records the callers from the frame records (x29 chain).
void NoteLeftSolve(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    ++(rt.left_window ? rt.left_window_solves : rt.left_other_solves);
    ++(rt.left_window ? rt.lw_solves_in : rt.lw_solves_out);
    if (!rt.order_rec) {
        return;
    }
    auto& mem = ctx.memory;
    const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
    const u64 fp = ctx.regs.x(ctx.regs.ctx, 29);
    const u64 lr1 = fp != 0 ? read64(fp + 8) : 0;        // Solve's return into BuildPose
    const u64 fp2 = fp != 0 ? read64(fp) : 0;
    const u64 lr2 = fp2 != 0 ? read64(fp2 + 8) : 0;      // BuildPose's return into its caller
    const u64 fp3 = fp2 != 0 ? read64(fp2) : 0;
    const u64 lr3 = fp3 != 0 ? read64(fp3 + 8) : 0;      // the path: PreRender or Render
    const auto main_mod = rt.modules.find("main");
    const u64 base = main_mod != rt.modules.end() ? main_mod->second.first : 0;
    const auto off = [&](u64 lr) { return lr > base && lr - base < 0x8000000 ? static_cast<u32>(lr - base) : 0u; };
    LeftOrderEvent(rt, rt.left_window ? 'S' : 's', off(lr1), off(lr2), off(lr3));
}

void RunArmIkHook(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& a = rt.profile.arms;
    const u64 chr = ctx.regs.x(ctx.regs.ctx, a.ik_char_reg);
    if (chr != 0 && a.body_char_index > 0 && chr == rt.arm_chars[static_cast<std::size_t>(a.body_char_index)]) {
        RunBodyPose(ctx, chr, ctx.regs.x(ctx.regs.ctx, a.ik_pose_reg));
        return;
    }
    if (a.left_ik && chr != 0 && chr == rt.left_char) {
        NoteLeftSolve(ctx);
        // The drawn pose is the one solved inside the arm's draw window, from that frame's camera and arm
        // transform; with ik_window_only the tick-time solves keep the game's pose.
        if (!rt.profile.left_window.ik_window_only || rt.left_window) {
            RunLeftArmIk(ctx, chr, ctx.regs.x(ctx.regs.ctx, a.ik_pose_reg));
        }
        return;
    }
    if (chr == 0 || chr != rt.arm_chars[0]) {
        return;
    }
    RunArmIk(ctx, chr, ctx.regs.x(ctx.regs.ctx, a.ik_pose_reg));
}

/// Part B diagnostic: each arm character's pose nodes, logged at its 300th and 1500th
/// pose build: id (bank/offset), parent-local rotation (4 floats as stored), scale, translation (model units).
void RunArmsLog(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& a = rt.profile.arms;
    const u64 chr = ctx.regs.x(ctx.regs.ctx, a.char_reg);
    if (rt.rig_logs && rt.profile.arm_measure.present && chr != 0 && chr == rt.left_char &&
        ++rt.left_buildpose_hits % 60 == 0 && ctx.memory.IsValidVirtualAddressRange(chr + a.pose_off_at, 2)) {
        // Test measurement M0: the left arm's Solve returns early when the flags byte's bits 6/7 are
        // clear; is it solved, and did our write point run? The solver list should be 0. Offsets: [arm_measure].
        auto& mem = ctx.memory;
        const auto& am = rt.profile.arm_measure;
        const u64 pose = chr + mem.Read16(chr + a.pose_off_at);
        const u8 flags = mem.IsValidVirtualAddressRange(pose + am.flags_off, 1) ? mem.Read8(pose + am.flags_off) : 0;
        const s16 rel = mem.IsValidVirtualAddressRange(pose + am.list_rel_off, 2)
                            ? static_cast<s16>(mem.Read16(pose + am.list_rel_off))
                            : 0;
        const u64 list_at = pose + static_cast<u64>(static_cast<s64>(rel)) + am.list_add;
        const u64 solvers = mem.IsValidVirtualAddressRange(list_at, 8) ? mem.Read64(list_at) : ~0ULL;
        LOG_INFO(Core, "VIZR hooks: arm measure M0 (left BuildPose): flags {:#04x} (bit6 {}, bit7 {}), our solves since "
                 "the last line {}, solver list {:#x}",
                 flags, (flags >> 6) & 1, (flags >> 7) & 1, rt.lik_solves - rt.lik_solves_logged, solvers);
        rt.lik_solves_logged = rt.lik_solves;
    }
    if (a.left_ik && chr != 0 && chr == rt.left_char && a.dirty_or != 0 && rt.body_camera && mem_ok_pose(ctx, chr, a)) {
        // The left arm's absolute pass every frame too, as for the right arm and the body: the render-time Solve
        // recomputes it only with these bits, and without them the skin kept the arm placed for the previous body
        // transform on about half the frames (a frame behind while the body turns).
        const u64 pose = chr + ctx.memory.Read16(chr + a.pose_off_at);
        if (ctx.memory.IsValidVirtualAddressRange(pose + a.dirty_at, 1)) {
            ctx.memory.Write8(pose + a.dirty_at, static_cast<u8>(ctx.memory.Read8(pose + a.dirty_at) | a.dirty_or));
        }
    }
    std::size_t k = 0;
    while (k < rt.arm_chars.size() && (rt.arm_chars[k] == 0 || rt.arm_chars[k] != chr)) {
        ++k;
    }
    if (k == rt.arm_chars.size()) {
        return;
    }
    if (a.body_char_index > 0 && k == static_cast<std::size_t>(a.body_char_index) && a.dirty_or != 0 &&
        rt.body_camera && mem_ok_pose(ctx, chr, a)) {
        // The body's absolute pass every frame too, so the hidden nodes stay hidden.
        const u64 pose = chr + ctx.memory.Read16(chr + a.pose_off_at);
        if (ctx.memory.IsValidVirtualAddressRange(pose + a.dirty_at, 1)) {
            ctx.memory.Write8(pose + a.dirty_at, static_cast<u8>(ctx.memory.Read8(pose + a.dirty_at) | a.dirty_or));
        }
    }
    if (a.ik && k == 0) {
        if (a.ik_hook.empty()) {
            if (mem_ok_pose(ctx, chr, a)) {
                RunArmIk(ctx, chr, ctx.memory.Read16(chr + a.pose_off_at) + chr); // The right arm
            }
        } else if (a.dirty_or != 0 && mem_ok_pose(ctx, chr, a)) {
            // Here only ask for the absolute pass; the solve runs at ik_hook.
            const u64 pose = chr + ctx.memory.Read16(chr + a.pose_off_at);
            if (ctx.memory.IsValidVirtualAddressRange(pose + a.dirty_at, 1)) {
                ctx.memory.Write8(pose + a.dirty_at, static_cast<u8>(ctx.memory.Read8(pose + a.dirty_at) | a.dirty_or));
            }
        }
    }
    if (!a.node_log) {
        return;
    }
    const u64 hit = ++rt.arm_hits[k];
    if (hit != 300 && hit != 1500) {
        return;
    }
    auto& mem = ctx.memory;
    const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
    if (!mem.IsValidVirtualAddressRange(chr + a.pose_off_at, 2)) {
        return;
    }
    const u64 pose = chr + mem.Read16(chr + a.pose_off_at);
    const u64 table = read64(pose + a.coords_table_at);
    u64 set = chr;
    for (const u64 step : a.node_set_chain) {
        set = read64(set + step);
    }
    if (table == 0 || set == 0 || !mem.IsValidVirtualAddressRange(set + a.count_at, 2)) {
        LOG_WARNING(Core, "VIZR hooks: arms: char {} ({:#x}) pose {:#x} table {:#x} set {:#x}: unreadable", k, chr,
                    pose, table, set);
        return;
    }
    const u32 count = std::min<u32>(mem.Read16(set + a.count_at), 128);
    LOG_INFO(Core, "VIZR hooks: arms: char {} ({}) build {}: {} nodes, pose {:#x}, flags {:#x}", k,
             k < a.char_fields.size() ? a.char_fields[k] : "?", hit, count, pose,
             mem.IsValidVirtualAddressRange(pose + a.dirty_at, 1) ? mem.Read8(pose + a.dirty_at) : 0);
    for (u32 n = 0; n < count; ++n) {
        const u64 id_at = set + a.ids_at + n * 4;
        if (!mem.IsValidVirtualAddressRange(id_at, 4)) {
            break;
        }
        const u32 id = mem.Read32(id_at);
        const u64 bank = id & 0x1f;
        const u64 offset = (id >> 5) & 0x7ff;
        const u64 coords = read64(table + bank * 8) + offset * 8 + 8;
        std::array<float, 10> c{};
        if (coords > 8 && mem.IsValidVirtualAddressRange(coords, sizeof(c))) {
            mem.ReadBlock(coords, c.data(), sizeof(c));
        }
        LOG_INFO(Core,
                 "VIZR hooks: arms: char {} node {} id {:#x} (type {} bank {} off {}) rot ({:.3f},{:.3f},{:.3f},{:.3f}) "
                 "scale ({:.3f},{:.3f},{:.3f}) trans ({:.3f},{:.3f},{:.3f})",
                 k, n, id, (id >> 21) & 7, bank, offset, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9]);
    }
}

/// The left arm (the glove that rests on the cannon) follows the left controller: at its render
/// entry its transform gets the controller's axes, placed so the arm's wrist is at the controller.
/// The left arm's transform at the left controller, from its current one (kept: the column lengths).
/// False = the game's own arm (no fresh hand, menu "Arm follows body" off).
bool LeftArmXf(const Runtime& rt, std::array<float, 12>& xf) {
    const auto& a = rt.profile.arms;
    const HandPose hand = g_left_hand.Load();
    if (!rt.camera_world_valid || !hand.valid || SteadyNowNs() - hand.sample_ns > HeadFreshNs ||
        !rt.space.camera_to_world || !rt.space.row_major || g_user_settings.Load().arm_follow < 0.5f) {
        return false;
    }
    const HandWorld hw = HandToWorld(rt, hand, {0.0f, 0.0f, 0.0f});
    const auto axis = [&](const std::string& name) { // [arms] left_columns (no fixed convention in core)
        const auto neg = [](const std::array<float, 3>& v) { return std::array<float, 3>{-v[0], -v[1], -v[2]}; };
        return name == "left" ? hw.left : name == "right" ? neg(hw.left) : name == "up" ? hw.up
             : name == "down" ? neg(hw.up) : name == "forward" ? hw.forward : neg(hw.forward);
    };
    const std::array<std::array<float, 3>, 3> cols{axis(a.left_columns[0]), axis(a.left_columns[1]),
                                                   axis(a.left_columns[2])};
    std::array<float, 3> length{};
    for (std::size_t c = 0; c < 3; ++c) {
        const float l = std::sqrt(xf[c] * xf[c] + xf[4 + c] * xf[4 + c] + xf[8 + c] * xf[8 + c]);
        length[c] = l > 1e-3f ? l : 1.0f;
        for (std::size_t r = 0; r < 3; ++r) {
            xf[r * 4 + c] = cols[c][r] * length[c];
        }
    }
    const auto& w = a.left_wrist;
    for (std::size_t r = 0; r < 3; ++r) {
        xf[r * 4 + 3] = hw.position[r] - (w[0] * xf[r * 4 + 0] + w[1] * xf[r * 4 + 1] + w[2] * xf[r * 4 + 2]);
    }
    return true;
}

/// The left arm's transform for the two-bone solve: body-fixed (the body's yaw frame: columns right, up,
/// back, each keeping its current length), placed so the arm model's shoulder point is on the body's left shoulder.
bool LeftArmBodyXf(Runtime& rt, std::array<float, 12>& xf) {
    const auto& a = rt.profile.arms;
    if (!rt.gun_xf_valid || g_user_settings.Load().arm_follow < 0.5f) {
        rt.left_xf_valid = false;
        return false;
    }
    // The game's own frame for this arm: columns left, up, forward (the body's yaw frame).
    const std::array<std::array<float, 3>, 3> cols{
        std::array<float, 3>{-rt.neck_right[0], -rt.neck_right[1], -rt.neck_right[2]}, rt.body_up, rt.neck_fwd};
    // The arm is drawn with this transform x Scale(S) (S = 2: the drawn wrist overshot the
    // controller by 0.35 m). The columns get k so the drawn reach (model reach x S x k) is the player's arm length.
    std::array<float, 3> length{};
    const float units_per_m = rt.space.units_per_meter * g_user_settings.Load().world_scale;
    const float s_avg = (rt.left_S[0] + rt.left_S[1] + rt.left_S[2]) / 3.0f;
    const float k = rt.lik_model_reach > 1e-3f && s_avg > 1e-3f
                        ? g_user_settings.Load().arm_length_m * units_per_m / (rt.lik_model_reach * s_avg)
                        : 1.0f / std::max(s_avg, 1e-3f);
    for (std::size_t c = 0; c < 3; ++c) {
        length[c] = k;
        for (std::size_t r = 0; r < 3; ++r) {
            xf[r * 4 + c] = cols[c][r] * length[c];
        }
    }
    // The shoulder in the transform's frame: the live pose's (last solve) or the profile's, with the pose-space signs
    // (the same convention the solve uses for the controller).
    const auto& sp = rt.lik_shoulder_valid ? rt.lik_shoulder_pose : a.left_shoulder_model;
    const std::array<float, 3> s{sp[0] * a.left_pose_signs[0], sp[1] * a.left_pose_signs[1], sp[2] * a.left_pose_signs[2]};
    const auto& S = rt.left_S; // the shoulder lands on its anchor through the drawn transform (xf x Scale(S))
    for (std::size_t r = 0; r < 3; ++r) {
        xf[r * 4 + 3] = rt.shoulder_left_world[r] -
                        (S[0] * s[0] * xf[r * 4 + 0] + S[1] * s[1] * xf[r * 4 + 1] + S[2] * s[2] * xf[r * 4 + 2]);
    }
    rt.left_xf = xf;
    rt.left_xf_valid = true;
    return true;
}

/// Rig check, once a second at the arm's render: the frame the grapple beam starts from this tick (base x local, as
/// the game composes it from the base written at UpdateState; the game rewrites the base from the gun later in the same
/// update, so it is kept from the write, not read back here) against the arm transform just written for drawing.
/// 0 cm / 0 deg = the beam and the drawn arm come from the same body and hand.
void LogBeamFrame(const HitContext& ctx, const std::array<float, 12>& drawn) {
    const s64 now = SteadyNowNs();
    if (now - ctx.rt.beam_check_ns < 1'000'000'000 || !ctx.rt.beam_frame_valid) {
        return;
    }
    ctx.rt.beam_check_ns = now;
    const auto& frame = ctx.rt.beam_frame;
    float d2 = 0.0f;
    for (std::size_t r = 0; r < 3; ++r) {
        const float d = frame[r * 4 + 3] - drawn[r * 4 + 3];
        d2 += d * d;
    }
    // The largest angle between matching unit columns.
    float worst_deg = 0.0f;
    for (std::size_t c = 0; c < 3; ++c) {
        float dot = 0.0f, la = 0.0f, lb = 0.0f;
        for (std::size_t r = 0; r < 3; ++r) {
            dot += frame[r * 4 + c] * drawn[r * 4 + c];
            la += frame[r * 4 + c] * frame[r * 4 + c];
            lb += drawn[r * 4 + c] * drawn[r * 4 + c];
        }
        const float cosine = std::clamp(dot / std::max(std::sqrt(la * lb), 1e-9f), -1.0f, 1.0f);
        worst_deg = std::max(worst_deg, std::acos(cosine) * 57.29578f);
    }
    const float units_per_m = ctx.rt.space.units_per_meter * g_user_settings.Load().world_scale;
    LOG_INFO(Core, "VIZR rig: beam frame vs drawn arm: {:.2f} cm, {:.2f} deg (origin {:.3f},{:.3f},{:.3f})",
             std::sqrt(d2) / std::max(units_per_m, 1e-3f) * 100.0f, worst_deg, frame[3], frame[7], frame[11]);
}

/// The grapple device's part locals at the Render entry, while the player is not grappling:
/// abs(locator) with its translation scaled by the arm scale, from the solved pose's abs slots (after this frame's PreRender solve), or the parent
/// bone's abs x a fixed local. In a grapple the game's own locals stay. False = nothing written.
/// A CCoords (quaternion w,x,y,z Hamilton, scale, translation) as a row-major 3x4: R(q) x diag(scale) | t.
std::array<float, 12> RelToAffine(const std::array<float, 10>& c) {
    const float w = c[0], x = c[1], y = c[2], z = c[3];
    const std::array<float, 9> r{1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y),
                                 2 * (x * y + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                                 2 * (x * z - w * y),     2 * (y * z + w * x),     1 - 2 * (x * x + y * y)};
    std::array<float, 12> m{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            m[row * 4 + col] = r[row * 3 + col] * c[4 + col];
        }
        m[row * 4 + 3] = c[7 + row];
    }
    return m;
}

/// The compose hook (after the abs program): right after OUR window solve of the left arm, keep L_elbow's abs and the
/// device locators' rels, so the Render entry composes the parts from our elbow (the banks may hold a later tick-time
/// compose of the clip pose by then; b52-rel-log).
void CaptureDeviceElbow(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& w = rt.profile.left_window;
    const auto& a = rt.profile.arms;
    auto& mem = ctx.memory;
    if (!rt.dev_solve_pending || rt.left_char == 0 || !mem.IsValidVirtualAddressRange(rt.left_char + a.pose_off_at, 2)) {
        return;
    }
    const u64 pose = ctx.regs.x(ctx.regs.ctx, w.device_compose_pose_reg);
    if (pose != rt.left_char + mem.Read16(rt.left_char + a.pose_off_at)) {
        return; // another character's compose
    }
    rt.dev_solve_pending = false;
    const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
    const auto slot = [&](u64 banks, u32 id) -> u64 {
        const u64 at = read64(banks + (id & 0x1f) * 8) + ((id >> 5) & 0x7ff) * 8 + 8;
        return at > 8 ? at : 0;
    };
    const u64 e_at = slot(read64(pose + a.left_abs_banks_off), w.device_elbow_id);
    if (e_at == 0 || !mem.IsValidVirtualAddressRange(e_at, 48)) {
        return;
    }
    const u64 rel_table = read64(pose + a.coords_table_at);
    std::array<std::array<float, 10>, 4> rels{};
    for (std::size_t k = 0; k < w.device_part_rel_ids.size() && k < rels.size(); ++k) {
        const u64 r_at = slot(rel_table, w.device_part_rel_ids[k]);
        if (r_at == 0 || !mem.IsValidVirtualAddressRange(r_at, 40)) {
            return;
        }
        mem.ReadBlock(r_at, rels[k].data(), sizeof(rels[k]));
    }
    // Each part's legacy m, from the left CChar's render context (ids matched as u16).
    const u64 rctx = read64(read64(rt.left_char + 8) + 0x20);
    const s32 count = rctx != 0 && mem.IsValidVirtualAddressRange(rctx + 0x40, 4) ? static_cast<s32>(mem.Read32(rctx + 0x40)) : -1;
    const u64 mats = rctx != 0 ? read64(rctx + 0x30) : 0;
    const u64 ids = rctx != 0 ? read64(rctx + 0x38) : 0;
    std::string table_ids;
    for (std::size_t k = 0; k < w.device_part_bind_ids.size() && k < rt.dev_bind_m.size(); ++k) {
        rt.dev_bind_ok[k] = false;
        for (s32 n = 0; n < count && n < 512 && ids != 0 && mats != 0; ++n) {
            if (!mem.IsValidVirtualAddressRange(ids + n * 2, 2) || mem.Read16(ids + n * 2) != (w.device_part_bind_ids[k] & 0xffff)) {
                continue;
            }
            std::array<float, 12> m{};
            if (mem.IsValidVirtualAddressRange(mats + n * 0x30, 48)) {
                mem.ReadBlock(mats + n * 0x30, m.data(), sizeof(m));
                rt.dev_bind_m[k] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
                rt.dev_bind_ok[k] = true;
            }
            break;
        }
    }
    if (!rt.dev_bind_logged) { // once a session: the table and each part's m
        rt.dev_bind_logged = true;
        for (s32 n = 0; n < count && n < 64 && ids != 0; ++n) {
            table_ids += fmt::format(" {:#06x}", mem.IsValidVirtualAddressRange(ids + n * 2, 2) ? mem.Read16(ids + n * 2) : 0);
        }
        std::string ms;
        for (std::size_t k = 0; k < w.device_part_bind_ids.size() && k < rt.dev_bind_m.size(); ++k) {
            const auto& m = rt.dev_bind_m[k];
            ms += rt.dev_bind_ok[k]
                      ? fmt::format(" part {} id {:#06x}: rows [{:.3f} {:.3f} {:.3f}; {:.3f} {:.3f} {:.3f}; {:.3f} {:.3f} {:.3f}];", k,
                                    w.device_part_bind_ids[k], m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8])
                      : fmt::format(" part {} id {:#06x}: NOT IN THE TABLE;", k, w.device_part_bind_ids[k]);
        }
        LOG_INFO(Core, "VIZR hooks: device legacy table: count {}, ids{};{}", count, table_ids, ms);
    }
    mem.ReadBlock(e_at, rt.dev_elbow_abs.data(), sizeof(rt.dev_elbow_abs));
    rt.dev_part_rel = rels;
    rt.dev_elbow_valid = true;
    rt.dev_renders_since_capture = 0;
    rt.dev_elbow_char = rt.left_char;
    ++rt.dev_captures;
    if (rt.rig_logs) {
        rt.dev_cap_runs = mem.IsValidVirtualAddressRange(pose + 0x22, 2) ? mem.Read16(pose + 0x22) : 0xffffu;
        rt.dev_cap_pose4 = mem.IsValidVirtualAddressRange(pose + 4, 4) ? mem.Read32(pose + 4) : 0;
        const u64 abs = read64(pose + a.left_abs_banks_off);
        const u64 hdr = abs >= 8 ? read64(abs - 8) : 0;
        rt.dev_cap_parent_base = hdr != 0 ? read64(hdr + 0x20) : 0;
        rt.dev_cap_dst_base = hdr != 0 ? read64(hdr + 0x18) : 0;
        rt.dev_cap_bank0 = read64(abs) + 8;
    }
}

/// Rig: the device's parts as drawn this frame (the arm's locals, ours or the game's), in the frame of the live L_elbow
/// abs slot (bank 0 +0xc8, whichever solve composed it): t = inv(E) x (local.t / S), and each part's x and z axes as
/// unit directions in the elbow's axes. An A/B with the game's own placement compares these.
/// Rig: the game's GetNodeId results for the grapple arm's three parts, decoded.
std::string NodeIdText(Runtime& rt) {
    if (rt.profile.left_window.device_nodeid_hook.empty()) {
        return {};
    }
    std::string out = "; game GetNodeId:";
    const char* names[3] = {"gear", "noz1", "noz2"};
    for (std::size_t k = 0; k < 3; ++k) {
        const u32 id = rt.dev_nodeid[k];
        out += id == 0xffffffffu ? fmt::format(" {} - (x{});", names[k], rt.dev_nodeid_hits[k])
                                 : fmt::format(" {} id {:#x} (type {:#x}, bank {}, slot {:#x}; x{});", names[k], id,
                                               id & 0xe00000u, id & 0x1fu, ((id >> 5) & 0x7ffu) * 8 + 8, rt.dev_nodeid_hits[k]);
        rt.dev_nodeid_hits[k] = 0;
    }
    return out;
}

std::string DeviceInElbowSpace(const HitContext& ctx, u64 arm) {
    Runtime& rt = ctx.rt;
    const auto& w = rt.profile.left_window;
    const auto& a = rt.profile.arms;
    auto& mem = ctx.memory;
    const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
    const u64 chr = rt.left_char;
    if (chr == 0 || !mem.IsValidVirtualAddressRange(chr + a.pose_off_at, 2)) {
        return "; elbow space: -";
    }
    const u64 pose = chr + mem.Read16(chr + a.pose_off_at);
    const u64 bank = read64(read64(pose + a.left_abs_banks_off));
    if (bank == 0 || !mem.IsValidVirtualAddressRange(bank + 0xc8, 48)) {
        return "; elbow space: -";
    }
    std::array<float, 12> e{};
    mem.ReadBlock(bank + 0xc8, e.data(), sizeof(e));
    const float a00 = e[0], a01 = e[1], a02 = e[2], a10 = e[4], a11 = e[5], a12 = e[6], a20 = e[8], a21 = e[9], a22 = e[10];
    const float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
    if (std::abs(det) < 1e-12f) {
        return "; elbow space: singular";
    }
    const std::array<float, 9> inv{(a11 * a22 - a12 * a21) / det, (a02 * a21 - a01 * a22) / det, (a01 * a12 - a02 * a11) / det,
                                   (a12 * a20 - a10 * a22) / det, (a00 * a22 - a02 * a20) / det, (a02 * a10 - a00 * a12) / det,
                                   (a10 * a21 - a11 * a20) / det, (a01 * a20 - a00 * a21) / det, (a00 * a11 - a01 * a10) / det};
    const auto apply = [&](const std::array<float, 3>& v) {
        return std::array<float, 3>{inv[0] * v[0] + inv[1] * v[1] + inv[2] * v[2], inv[3] * v[0] + inv[4] * v[1] + inv[5] * v[2],
                                    inv[6] * v[0] + inv[7] * v[1] + inv[8] * v[2]};
    };
    const auto unit = [](std::array<float, 3> v) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-9f) {
            v = {v[0] / l, v[1] / l, v[2] / l};
        }
        return v;
    };
    std::string out = "; elbow space (live +0xc8):";
    for (std::size_t k = 0; k < w.device_locals.size() && k < 3; ++k) {
        std::array<float, 12> m{};
        if (!mem.IsValidVirtualAddressRange(arm + w.device_locals[k], 48)) {
            continue;
        }
        mem.ReadBlock(arm + w.device_locals[k], m.data(), sizeof(m));
        const std::array<float, 3> p{m[3] / std::max(rt.left_S[0], 1e-3f) - e[3], m[7] / std::max(rt.left_S[1], 1e-3f) - e[7],
                                     m[11] / std::max(rt.left_S[2], 1e-3f) - e[11]};
        const auto t = apply(p);
        const auto x = unit(apply({m[0], m[4], m[8]}));
        const auto z = unit(apply({m[2], m[6], m[10]}));
        out += fmt::format(" {:#x} t ({:.3f},{:.3f},{:.3f}) x ({:.2f},{:.2f},{:.2f}) z ({:.2f},{:.2f},{:.2f});",
                           w.device_locals[k], t[0], t[1], t[2], x[0], x[1], x[2], z[0], z[1], z[2]);
    }
    return out;
}

bool WriteDeviceLocals(const HitContext& ctx, u64 arm) {
    Runtime& rt = ctx.rt;
    const auto& w = rt.profile.left_window;
    const auto& a = rt.profile.arms;
    auto& mem = ctx.memory;
    const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
    const u64 grapple_at = rt.orbit_player + w.device_skip_player_off;
    if (rt.orbit_player == 0 || !mem.IsValidVirtualAddressRange(grapple_at, 4) || mem.Read32(grapple_at) != 0) {
        return false; // grappling (or the player unknown): the game places the device
    }
    const u64 chr = rt.left_char;
    if (chr == 0 || !mem.IsValidVirtualAddressRange(chr + a.pose_off_at, 2)) {
        return false;
    }
    const u64 pose = chr + mem.Read16(chr + a.pose_off_at);
    const u64 bank = read64(read64(pose + w.device_abs_banks_off));
    if (bank == 0 || !mem.IsValidVirtualAddressRange(bank + w.device_parent_slot, 48)) {
        return false;
    }
    std::array<float, 12> parent{};
    mem.ReadBlock(bank + w.device_parent_slot, parent.data(), sizeof(parent));
    const bool composed = !w.device_compose_hook.empty();
    if (composed) {
        // Only an elbow from this drawn frame's solve or the previous one's, of this left char: when our solve does not
        // run (tracking lost, the window shut, a new char after a room load) the arm shows the clip, and an older
        // elbow would put the device beside it. The game's locals stay.
        if (!rt.dev_elbow_valid || rt.dev_renders_since_capture > 1 || rt.dev_elbow_char != chr) {
            return false;
        }
        // Every part has its legacy m, or none is written (never a half-turned device).
        for (std::size_t k = 0; k < w.device_abs_slots.size() && k < w.device_part_rel_ids.size(); ++k) {
            if (!rt.dev_bind_ok[k]) {
                return false;
            }
        }
        if (rt.rig_logs) { // the decisive read at this Render entry (the live bank against the capture)
            const auto cols = [](const std::array<float, 12>& m) {
                return fmt::format("{:.3f}/{:.3f}/{:.3f}", std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]),
                                   std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]),
                                   std::sqrt(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]));
            };
            std::array<float, 12> live_g{};
            const bool g_ok = mem.IsValidVirtualAddressRange(bank + 0x4e8, 48);
            if (g_ok) {
                mem.ReadBlock(bank + 0x4e8, live_g.data(), sizeof(live_g));
            }
            // inv(live elbow) x live grapLocator: a general 3x3 inverse (the elbow carries a scale).
            const auto& e = parent;
            const float a00 = e[0], a01 = e[1], a02 = e[2], a10 = e[4], a11 = e[5], a12 = e[6], a20 = e[8], a21 = e[9],
                        a22 = e[10];
            const float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
            std::string rel_live = "-";
            if (g_ok && std::abs(det) > 1e-12f) {
                const std::array<float, 9> inv{(a11 * a22 - a12 * a21) / det, (a02 * a21 - a01 * a22) / det,
                                               (a01 * a12 - a02 * a11) / det, (a12 * a20 - a10 * a22) / det,
                                               (a00 * a22 - a02 * a20) / det, (a02 * a10 - a00 * a12) / det,
                                               (a10 * a21 - a11 * a20) / det, (a01 * a20 - a00 * a21) / det,
                                               (a00 * a11 - a01 * a10) / det};
                const std::array<float, 3> d{live_g[3] - e[3], live_g[7] - e[7], live_g[11] - e[11]};
                std::array<float, 3> t{};
                for (std::size_t r = 0; r < 3; ++r) {
                    t[r] = inv[r * 3] * d[0] + inv[r * 3 + 1] * d[1] + inv[r * 3 + 2] * d[2];
                }
                rel_live = fmt::format("({:.4f},{:.4f},{:.4f})", t[0], t[1], t[2]);
            }
            rt.dev_render_read = fmt::format(
                "; decisive: runs at capture {} now {}, pose+4 {:#x} now {:#x}, op bases parent {:#x} dst {:#x} vs bank0+8 "
                "{:#x}; live +0xc8 cols {} t ({:.3f},{:.3f},{:.3f}), captured elbow cols {} t ({:.3f},{:.3f},{:.3f}); "
                "inv(live +0xc8) x live +0x4e8 t {} vs grapLocator rel t ({:.4f},{:.4f},{:.4f})",
                rt.dev_cap_runs, mem.IsValidVirtualAddressRange(pose + 0x22, 2) ? mem.Read16(pose + 0x22) : 0xffffu,
                rt.dev_cap_pose4, mem.IsValidVirtualAddressRange(pose + 4, 4) ? mem.Read32(pose + 4) : 0u,
                rt.dev_cap_parent_base, rt.dev_cap_dst_base, rt.dev_cap_bank0, cols(parent), parent[3], parent[7],
                parent[11], cols(rt.dev_elbow_abs), rt.dev_elbow_abs[3], rt.dev_elbow_abs[7], rt.dev_elbow_abs[11],
                rel_live, rt.dev_part_rel[0][7], rt.dev_part_rel[0][8], rt.dev_part_rel[0][9]);
        }
        parent = rt.dev_elbow_abs;
    }
    const auto mul = [](const std::array<float, 12>& x, const std::array<float, 12>& y) { // 3x4 x 3x4 (affine)
        std::array<float, 12> o{};
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t c = 0; c < 4; ++c) {
                o[r * 4 + c] = x[r * 4 + 0] * y[0 * 4 + c] + x[r * 4 + 1] * y[1 * 4 + c] + x[r * 4 + 2] * y[2 * 4 + c] +
                               (c == 3 ? x[r * 4 + 3] : 0.0f);
            }
        }
        return o;
    };
    for (std::size_t k = 0; k < w.device_abs_slots.size(); ++k) {
        std::array<float, 12> abs{};
        const u64 slot = w.device_abs_slots[k];
        if (composed && k < w.device_part_rel_ids.size()) {
            abs = mul(parent, RelToAffine(rt.dev_part_rel[k]));
            // The game's GetLegacyTransform: R = abs.R x m.R, t = abs.t (m's translation unused).
            const auto& m = rt.dev_bind_m[k];
            std::array<float, 9> r{};
            for (std::size_t i = 0; i < 3; ++i) {
                for (std::size_t j = 0; j < 3; ++j) {
                    r[i * 3 + j] = abs[i * 4 + 0] * m[0 * 3 + j] + abs[i * 4 + 1] * m[1 * 3 + j] + abs[i * 4 + 2] * m[2 * 3 + j];
                }
            }
            for (std::size_t i = 0; i < 3; ++i) {
                for (std::size_t j = 0; j < 3; ++j) {
                    abs[i * 4 + j] = r[i * 3 + j];
                }
            }
        } else if (slot != 0 && mem.IsValidVirtualAddressRange(bank + slot, 48)) {
            mem.ReadBlock(bank + slot, abs.data(), sizeof(abs));
        } else {
            abs = mul(parent, w.device_fallback_local[k]);
        }
        for (std::size_t r = 0; r < 3; ++r) { // the arm's model scale on the translation only, as
            abs[r * 4 + 3] *= rt.left_S[r];  // GetScaledLocatorTransform (0xfa8ad0): the rotation stays unit
        }
        if (!mem.IsValidVirtualAddressRange(arm + w.device_locals[k], 48)) {
            return false;
        }
        mem.WriteBlock(arm + w.device_locals[k], abs.data(), sizeof(abs));
    }
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
    return true;
}

/// The arm's Render entry, and (prerender) its PreRender entry: the pass before the draws builds the pose the skin
/// keeps, so the arm transform is written there as well, from the same frame's last camera hook.
void RunLeftArmRender(const HitContext& ctx, bool prerender = false) {
    const auto& a = ctx.rt.profile.arms;
    const u64 arm = ctx.regs.x(ctx.regs.ctx, a.left_arm_reg);
    if (prerender) {
        const auto& w = ctx.rt.profile.left_window;
        const u64 at = arm + w.prerender_flags_off;
        if (arm == 0 || !ctx.memory.IsValidVirtualAddressRange(at, 4) ||
            (ctx.memory.Read32(at) & w.prerender_flags_mask) != w.prerender_flags_equals) {
            return; // not drawn this frame
        }
        LeftWindowOpen(ctx, 'P');
    } else {
        ctx.rt.left_arm_ptr = arm; // test measurement M3
        LeftWindowOpen(ctx, 'R');
        ++ctx.rt.dev_renders_since_capture; // every drawn frame, whether or not our solve or the device write runs
        const auto& locals = ctx.rt.profile.left_window.device_locals;
        ctx.rt.la_device_n = 0;
        if (const auto& f = ctx.rt.profile.left_window.owned_field; !f.empty()) {
            const auto v = ReadScalar(ctx, f);
            ctx.rt.la_owned = v ? static_cast<s64>(*v) : -1;
        }
        for (std::size_t k = 0; k < locals.size() && k < ctx.rt.la_device.size(); ++k) {
            if (arm != 0 && ctx.memory.IsValidVirtualAddressRange(arm + locals[k], 48)) {
                ctx.memory.ReadBlock(arm + locals[k], ctx.rt.la_device[k].data(), 48);
                ctx.rt.la_device_n = static_cast<u32>(k + 1);
            }
        }
        if (ctx.rt.rig_logs && arm != 0) { // the game's placement (replaced below when our write runs)
            ctx.rt.la_elbow_space = DeviceInElbowSpace(ctx, arm);
            ctx.rt.la_device_ours = false;
        }
    }
    if (!prerender) {
        // Game ticks per drawn frame (camera writes since the last Render entry), for the cannon-base line.
        const u64 ticks = ctx.rt.camera_writes - ctx.rt.render_cam_writes_seen;
        ctx.rt.render_cam_writes_seen = ctx.rt.camera_writes;
        ++ctx.rt.render_ticks[std::min<u64>(ticks, 2)];
    }
    if (arm != 0 && ctx.memory.IsValidVirtualAddressRange(arm + a.left_model_scale_off, 12)) {
        std::array<float, 3> S{};
        ctx.memory.ReadBlock(arm + a.left_model_scale_off, S.data(), sizeof(S));
        if (S[0] > 1e-3f && S[1] > 1e-3f && S[2] > 1e-3f) {
            ctx.rt.left_S = S;
        }
    }
    if (arm == 0 || !ctx.memory.IsValidVirtualAddressRange(arm + a.left_xf_off, 48)) {
        return;
    }
    std::array<float, 12> xf{};
    ctx.memory.ReadBlock(arm + a.left_xf_off, xf.data(), sizeof(xf));
    if (a.left_ik) {
        // The arm's character for the solve, **(arm + left_char_off); the transform body-fixed.
        const auto read64 = [&](u64 at) -> u64 {
            return ctx.memory.IsValidVirtualAddressRange(at, 8) ? ctx.memory.Read64(at) : 0;
        };
        const u64 holder = read64(arm + a.left_char_off);
        ctx.rt.left_char = holder != 0 ? read64(holder) : 0;
        if (!LeftArmBodyXf(ctx.rt, xf)) {
            return; // the game's own arm
        }
    } else if (!LeftArmXf(ctx.rt, xf)) {
        return; // the game's own arm
    }
    ctx.memory.WriteBlock(arm + a.left_xf_off, xf.data(), sizeof(xf));
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
    if (!prerender && ctx.rt.profile.left_window.device_write) {
        const bool wrote = WriteDeviceLocals(ctx, arm);
        ++(wrote ? ctx.rt.la_device_writes : ctx.rt.la_device_skips);
        ctx.rt.la_device_ours = wrote;
        if (ctx.rt.rig_logs) {
            ctx.rt.la_elbow_space = DeviceInElbowSpace(ctx, arm);
        }
        if (wrote) { // the 1 s line shows what this frame draws (the snapshot above is the game's tick-time locals)
            const auto& locals = ctx.rt.profile.left_window.device_locals;
            for (std::size_t k = 0; k < locals.size() && k < ctx.rt.la_device.size(); ++k) {
                if (ctx.memory.IsValidVirtualAddressRange(arm + locals[k], 48)) {
                    ctx.memory.ReadBlock(arm + locals[k], ctx.rt.la_device[k].data(), 48);
                }
            }
        }
    }
    if (ctx.rt.rig_logs && !a.left_beam_hook.empty()) {
        LogBeamFrame(ctx, xf);
    }
}

/// The grapple beam starts at base x local; base = the drawn left arm's transform x local^-1 (local rigid, read live),
/// so the beam leaves the arm the player sees: the arm's transform is computed here the way its render computes it
/// (body-anchored for the two-bone solve, else the rigid placement), from this tick's body and hand. The game places
/// the beam's origin at base x local x the arm model's beam locator, so the origin then follows the solved pose.
void RunLeftBeam(const HitContext& ctx) {
    const auto& a = ctx.rt.profile.arms;
    auto& mem = ctx.memory;
    const u64 arm = ctx.regs.x(ctx.regs.ctx, a.left_arm_reg);
    if (arm == 0 || !mem.IsValidVirtualAddressRange(arm + a.left_xf_off, 48) ||
        !mem.IsValidVirtualAddressRange(arm + a.left_beam_off, 48) ||
        !mem.IsValidVirtualAddressRange(arm + a.left_beam_local_off, 48) ||
        !mem.IsValidVirtualAddressRange(arm + a.left_beam_state_off, 1)) {
        return;
    }
    // Only in the update that fires the beam: the base written in any other state reached the game's other arm
    // states (it made the morph ball miss presses).
    if ((mem.Read8(arm + a.left_beam_state_off) & a.left_beam_state_mask) != a.left_beam_state_equals) {
        return;
    }
    std::array<float, 12> xf{}, local{};
    mem.ReadBlock(arm + a.left_xf_off, xf.data(), sizeof(xf));
    mem.ReadBlock(arm + a.left_beam_local_off, local.data(), sizeof(local));
    if (!(a.left_ik ? LeftArmBodyXf(ctx.rt, xf) : LeftArmXf(ctx.rt, xf))) {
        return;
    }
    // local^-1 = [Rt | -Rt t] (rigid), then base = xf x local^-1
    std::array<float, 12> inv{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            inv[r * 4 + c] = local[c * 4 + r];
        }
        inv[r * 4 + 3] = -(local[0 * 4 + r] * local[3] + local[1 * 4 + r] * local[7] + local[2 * 4 + r] * local[11]);
    }
    std::array<float, 12> base{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 4; ++c) {
            float v = c == 3 ? xf[r * 4 + 3] : 0.0f;
            for (std::size_t k = 0; k < 3; ++k) {
                v += xf[r * 4 + k] * inv[k * 4 + c];
            }
            base[r * 4 + c] = v;
        }
    }
    mem.WriteBlock(arm + a.left_beam_off, base.data(), sizeof(base));
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
    if (ctx.rt.rig_logs) {
        // The beam's frame as the game composes it this tick (base x local), for the render's check.
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t c = 0; c < 4; ++c) {
                float v = c == 3 ? base[r * 4 + 3] : 0.0f;
                for (std::size_t k = 0; k < 3; ++k) {
                    v += base[r * 4 + k] * local[k * 4 + c];
                }
                ctx.rt.beam_frame[r * 4 + c] = v;
            }
        }
        ctx.rt.beam_frame_valid = true;
    }
}

/// The grapple point nearest the left hand's ray replaces the game's camera candidate.
/// Every byte of an object-list entry the pick code reads: the entity pointer, the id check and the flags byte.
u64 SlotSpan(const Profile::GrappleAim& g) {
    return std::max({g.entity_off + 8, g.id_check_off + 4, g.flags_off + 1});
}

/// In the Scan Visor the player scans what they look at (the game would snap to the nearest target): the orbit
/// candidate (the object ZL locks and the scan scans) = the scannable object nearest the HEAD ray
/// within scan_max_angle_deg, or the game's invalid id: nothing locks unless the player looks at it.
void RunScanPick(const HitContext& ctx, u64 player, u64 table, u32 count, u64 data, u32 none, float range) {
    // (count, data: the in-range list the caller read; the pick walks scan_lists itself)
    Runtime& rt = ctx.rt;
    const auto& g = rt.profile.grapple_aim;
    auto& mem = ctx.memory;
    const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
    // The head ray, as the scan cursor's (RunCursorRay from_head): OpenXR -Z through the head camera.
    const auto& cw = rt.camera_world;
    std::array<float, 3> local{}, fwd{};
    for (std::size_t i = 0; i < 3; ++i) {
        local[i] = -rt.basis[i][2];
    }
    for (std::size_t i = 0; i < 3; ++i) {
        fwd[i] = cw[i * 4 + 0] * local[0] + cw[i * 4 + 1] * local[1] + cw[i * 4 + 2] * local[2];
    }
    const float fn = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (fn < 1e-6f) {
        return;
    }
    for (float& v : fwd) {
        v /= fn;
    }
    const std::array<float, 3> eye{cw[3], cw[7], cw[11]};
    const float cos_max = std::cos(g.scan_max_angle_deg * std::numbers::pi_v<float> / 180.0f);
    u32 best = none;
    float best_cos = cos_max;
    u32 scannable = 0;
    (void)data;
    // The pick walks every orbit list (in range, and the two screen-zone lists), each id once.
    std::vector<u32> ids;
    for (const u64 list : g.scan_lists) {
        if (!mem.IsValidVirtualAddressRange(player + list, 4)) {
            continue;
        }
        const u32 n_list = std::min<u32>(mem.Read32(player + list), 64);
        const u64 at = read64(player + list + g.list_data_off);
        if (at == 0 || !mem.IsValidVirtualAddressRange(at, n_list * 4)) {
            continue;
        }
        for (u32 n = 0; n < n_list; ++n) {
            const u32 id = mem.Read32(at + n * 4);
            if (id != none && std::ranges::find(ids, id) == ids.end()) {
                ids.push_back(id);
            }
        }
    }
    count = static_cast<u32>(ids.size());
    // Test: each listed id once (up to 40 per launch) with the stage it reached (why each one was or was not picked).
    const auto note = [&](u32 id, std::string_view stage, u64 object) {
        if (!rt.rig_logs || rt.scan_list_logs >= 40 || std::ranges::find(rt.scan_ids_logged, id) != rt.scan_ids_logged.end()) {
            return;
        }
        rt.scan_ids_logged.push_back(id);
        ++rt.scan_list_logs;
        const u64 vt = object != 0 ? read64(object) : 0;
        const auto main_module = rt.modules.find("main");
        const u64 base = main_module != rt.modules.end() ? main_module->second.first : 0;
        LOG_INFO(Core, "VIZR hooks: scan pick list: id {:#x}: {} (object {:#x}, vtable main+{:#x})", id, stage, object,
                 vt >= base ? vt - base : 0);
    };
    if (table == 0 && rt.rig_logs && rt.scan_list_logs == 0) {
        ++rt.scan_list_logs;
        LOG_INFO(Core, "VIZR hooks: scan pick list: no entity table ({} ids listed)", ids.size());
    }
    if (table != 0) {
        for (const u32 id : ids) {
            const u64 e = read64(table + (id & g.id_index_mask) * g.stride);
            if (e == 0 || !mem.IsValidVirtualAddressRange(e, SlotSpan(g))) {
                note(id, "no table slot", 0);
                continue;
            }
            if (mem.Read32(e + g.id_check_off) != id) {
                note(id, fmt::format("slot id {:#x} differs", mem.Read32(e + g.id_check_off)), 0);
                continue;
            }
            if ((mem.Read8(e + g.flags_off) & g.flags_mask) != g.flags_value) {
                note(id, fmt::format("slot flags {:#x}", mem.Read8(e + g.flags_off)), 0);
                continue;
            }
            const u64 object = read64(e + g.entity_off);
            if (object == 0 || !mem.IsValidVirtualAddressRange(object + g.scan_holder_off, 8)) {
                note(id, "no object", object);
                continue;
            }
            if (g.deleted_mask != 0 && (!mem.IsValidVirtualAddressRange(object + g.deleted_off, 1) ||
                                        (mem.Read8(object + g.deleted_off) & g.deleted_mask) != 0)) {
                note(id, "being deleted", object);
                continue;
            }
            // the game's own scannable test (GetScannableObjectInfo)
            const u64 holder = read64(object + g.scan_holder_off);
            const bool has_scan = holder != 0 && mem.IsValidVirtualAddressRange(holder, g.scan_info_off + 8) &&
                                  (mem.Read64(holder + g.scan_info_off) != 0 || mem.Read8(holder + g.scan_info_flag_off) != 0);
            if (!has_scan) {
                note(id, "no scan data", object);
                continue;
            }
            u64 at = object;
            for (const u64 step : g.point_chain) {
                at = read64(at + step);
            }
            if (at == 0 || !mem.IsValidVirtualAddressRange(at, 48)) {
                note(id, "scan data, no position (point_chain)", object);
                continue;
            }
            ++scannable;
            std::array<float, 12> p{};
            mem.ReadBlock(at, p.data(), sizeof(p));
            const std::array<float, 3> d{p[3] - eye[0], p[7] - eye[1], p[11] - eye[2]};
            const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const float c = dist > 1e-3f ? (d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]) / dist : 1.0f;
            note(id, fmt::format("scannable at {:.1f} units (range {:.1f}), {:.1f} deg off the head ray", dist, range,
                                 std::acos(std::clamp(c, -1.0f, 1.0f)) * 57.29578f),
                 object);
            if (dist < 1e-3f || dist > range) {
                continue;
            }
            if (c >= best_cos) {
                best_cos = c;
                best = id;
                rt.scan_pick_pos = {p[3], p[7], p[11]};
            }
        }
    }
    rt.scan_list_logged_count = count;
    rt.scan_pick_valid = best != none;
    const u32 game = mem.Read32(player + g.candidate_off);
    if (rt.rig_logs && best != rt.scan_pick_logged) {
        rt.scan_pick_logged = best;
        LOG_INFO(Core, "VIZR hooks: scan pick: {} in range, {} scannable, camera candidate {:#x}, head pick {:#x} ({:.1f} deg)",
                 count, scannable, game, best, std::acos(std::clamp(best_cos, -1.0f, 1.0f)) * 57.29578f);
    }
    if (g.scan_state_offs.size() == 4 && mem.IsValidVirtualAddressRange(player + g.scan_state_offs[0], 4)) {
        const u32 orbit = mem.Read32(player + g.scan_state_offs[0]);
        if (rt.scan_orbit != 0 && orbit == 0) {
            rt.release_until_ns = SteadyNowNs() + 2'000'000'000; // A lock just released
        }
        rt.scan_orbit = orbit;
    }
    if (const s64 now = SteadyNowNs(); !g.release_log_offs.empty() && now < rt.release_until_ns &&
                                       now - rt.release_logged_ns >= 100'000'000) {
        // The game camera has been seen to swing 45-100 deg/s just after a scan lock releases, then the body follows:
        // the free-look flags, the sticks we sent and the yaws, 2 s after every release.
        rt.release_logged_ns = now;
        std::string flags;
        for (const u64 off : g.release_log_offs) {
            flags += fmt::format(" PL+{:#x}={:#04x}", off,
                                 mem.IsValidVirtualAddressRange(player + off, 1) ? mem.Read8(player + off) : 0);
        }
        const auto sent = VIZR::g_sent_pad.Load();
        const auto& gc = rt.game_camera;
        LOG_INFO(Core, "VIZR hooks: scan release:{}; sent left ({:.2f},{:.2f}) right ({:.2f},{:.2f}); game camera yaw {:.2f} "
                 "body {}",
                 flags, sent.sticks[0], sent.sticks[1], sent.sticks[2], sent.sticks[3],
                 std::atan2(gc[2], -gc[10]) * 57.29578f,
                 rt.body_yaw_valid ? fmt::format("{:.2f}", rt.body_yaw_deg) : std::string{"-"});
    }
    if (const s64 now = SteadyNowNs(); rt.rig_logs && g.scan_state_offs.size() == 4 &&
                                         now - rt.scan_state_logged_ns > 1'000'000'000) {
        // Test (the full-scan sweep): the lock and the scan, once a second in the Scan Visor (the profile's
        // scan_state_offs: orbit state, locked target, scan state, scan time).
        rt.scan_state_logged_ns = now;
        const auto& o = g.scan_state_offs;
        const bool readable = std::ranges::all_of(o, [&](u64 off) { return mem.IsValidVirtualAddressRange(player + off, 4); });
        if (readable) {
            u32 t_bits = mem.Read32(player + o[3]);
            float t{};
            std::memcpy(&t, &t_bits, sizeof(t));
            LOG_INFO(Core,
                     "VIZR hooks: scan state: orbit {} target {:#x} scan {} time {:.2f} s; candidate {:#x} (head pick {:#x})",
                     mem.Read32(player + o[0]), mem.Read32(player + o[1]), mem.Read32(player + o[2]), t, game, best);
        }
    }
    mem.Write32(player + g.candidate_off, best);
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
}

/// [grapple_aim.zl_trace] entry i (every session; see profile.h).
/// [grapple_aim.press_log] at a ZL press edge (every session): why a press finds a target or not.
void LogPressTargets(const HitContext& ctx, u64 player) {
    Runtime& rt = ctx.rt;
    const auto& g = rt.profile.grapple_aim;
    const auto& pl = g.press_log;
    auto& mem = ctx.memory;
    if (!pl.present || player == 0) {
        return;
    }
    const auto read32 = [&](u64 at) -> std::optional<u32> {
        return mem.IsValidVirtualAddressRange(at, 4) ? std::optional<u32>{mem.Read32(at)} : std::nullopt;
    };
    const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
    std::string counts;
    for (const u64 off : pl.lists) {
        const auto n = read32(player + off);
        counts += fmt::format(" +{:#x}={}", off, n ? fmt::format("{}", *n) : std::string{"-"});
    }
    // The nearest target in the first list (the in-range box list), from the head.
    std::string nearest = "none";
    const auto module = rt.modules.find(g.vtable_module);
    if (!pl.lists.empty() && rt.orbit_table != 0 && rt.camera_world_valid && module != rt.modules.end()) {
        const u32 count = std::min<u32>(read32(player + pl.lists[0]).value_or(0), 64);
        const u64 data = read64(player + pl.lists[0] + g.list_data_off);
        const auto& h = rt.camera_world;
        const std::array<float, 3> eye{h[3], h[7], h[11]};
        std::array<float, 3> fh{h[2], h[6], h[10]};
        const float fn = std::sqrt(fh[0] * fh[0] + fh[1] * fh[1] + fh[2] * fh[2]);
        const HandPose hand = g_left_hand.Load();
        const bool hand_ok = hand.valid && SteadyNowNs() - hand.sample_ns <= HeadFreshNs && rt.space.camera_to_world &&
                             rt.space.row_major;
        const HandWorld hw =
            hand_ok ? TurnAim(rt, HandToWorld(rt, hand, {0.0f, 0.0f, 0.0f}), g.left_aim_yaw_offset_deg) : HandWorld{};
        // The right hand's own ray too (no offset): whether the same bias is in both hands' aim poses.
        const HandPose right_hand = g_right_hand.Load();
        const bool right_ok = right_hand.valid && SteadyNowNs() - right_hand.sample_ns <= HeadFreshNs &&
                              rt.space.camera_to_world && rt.space.row_major;
        const HandWorld rw = right_ok ? HandToWorld(rt, right_hand, {0.0f, 0.0f, 0.0f}) : HandWorld{};
        float best_d = 1e30f;
        for (u32 n = 0; n < count && data != 0 && fn > 1e-6f; ++n) {
            const auto id = read32(data + n * 4);
            if (!id) {
                break;
            }
            const u64 e = read64(rt.orbit_table + (*id & g.id_index_mask) * g.stride);
            if (e == 0 || !mem.IsValidVirtualAddressRange(e, SlotSpan(g)) || mem.Read32(e + g.id_check_off) != *id) {
                continue;
            }
            const u64 object = read64(e + g.entity_off);
            u64 at = object;
            for (const u64 step : g.point_chain) {
                at = at != 0 ? read64(at + step) : 0;
            }
            if (at == 0 || !mem.IsValidVirtualAddressRange(at, 48)) {
                continue;
            }
            std::array<float, 12> p{};
            mem.ReadBlock(at, p.data(), sizeof(p));
            const std::array<float, 3> d{p[3] - eye[0], p[7] - eye[1], p[11] - eye[2]};
            const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dl < 1e-3f || dl >= best_d) {
                continue;
            }
            best_d = dl;
            const float head_deg =
                std::acos(std::clamp((d[0] * fh[0] + d[1] * fh[1] + d[2] * fh[2]) / (dl * fn), -1.0f, 1.0f)) * 57.29578f;
            // Signed: the point's direction right / up of a hand's ray, in the hand's own axes (a systematic
            // offset of the aim pose shows as a constant sign over many presses).
            const auto off_hand = [&](const HandWorld& w) -> std::string {
                const std::array<float, 3> dh{p[3] - w.position[0], p[7] - w.position[1], p[11] - w.position[2]};
                const float hl = std::sqrt(dh[0] * dh[0] + dh[1] * dh[1] + dh[2] * dh[2]);
                if (hl <= 1e-3f) {
                    return "-";
                }
                const auto dot = [&](const std::array<float, 3>& v) { return (dh[0] * v[0] + dh[1] * v[1] + dh[2] * v[2]) / hl; };
                const float fwd = dot(w.forward);
                const float right = -dot(w.left), up = dot(w.up);
                return fmt::format("{:.1f} (right {:+.1f}, up {:+.1f})", std::acos(std::clamp(fwd, -1.0f, 1.0f)) * 57.29578f,
                                   std::atan2(right, fwd) * 57.29578f, std::atan2(up, fwd) * 57.29578f);
            };
            const std::string hand_deg = hand_ok ? off_hand(hw) : std::string{"-"};
            const std::string right_deg = right_ok ? off_hand(rw) : std::string{"-"};
            const bool point = object != 0 && read64(object) == module->second.first + g.vtable_offset;
            nearest = fmt::format("{:#x} ({}) {:.1f} units, {:.1f} deg off the head, {} deg off the hand (aim yaw "
                                  "{:+.1f}), {} deg off the right hand",
                                  *id, point ? "grapple point" : "object", dl, head_deg, hand_deg,
                                  g.left_aim_yaw_offset_deg, right_deg);
        }
    }
    // The visibility read list: id and flags & 3 (3 = visible), the first 8.
    std::string reads;
    u32 read_n = 0;
    const u64 list = read64(player + pl.read_list_off);
    if (list != 0) {
        read_n = read32(list + pl.read_count_off).value_or(0);
        const u64 data = read64(list + pl.read_data_off);
        for (u32 n = 0; n < std::min<u32>(read_n, 8) && data != 0; ++n) {
            const u64 entry = data + n * pl.read_stride;
            const auto id = read32(entry + pl.read_id_off);
            const auto flags = read32(entry + pl.read_flags_off);
            if (!id || !flags) {
                break;
            }
            reads += fmt::format(" {:#x}:{}", *id, *flags & 3);
        }
    }
    LOG_INFO(Core, "VIZR hooks: press targets: lists{}; nearest in the box list: {}; orbit disable {}; read list {}:{}",
             counts, nearest, read32(player + pl.disable_off).value_or(0), read_n, reads.empty() ? " -" : reads);
}

void LogZlTrace(const HitContext& ctx, std::size_t i) {
    Runtime& rt = ctx.rt;
    const auto& z = rt.profile.grapple_aim.zl_trace[i];
    if (rt.zl_trace_last.size() != rt.profile.grapple_aim.zl_trace.size()) {
        rt.zl_trace_last.assign(rt.profile.grapple_aim.zl_trace.size(), ~0ULL);
    }
    const u64 value = ctx.regs.x(ctx.regs.ctx, z.reg) & 0xffffffffULL;
    const s64 now = SteadyNowNs();
    const bool changed = value != rt.zl_trace_last[i];
    if (value == 1 && rt.zl_trace_last[i] == 0 && now - rt.grapple_press_logged_ns > 150'000'000) {
        // A ZL press edge (the first trace register to go 0 -> 1): the candidate the press locks and who put it there.
        rt.grapple_press_logged_ns = now;
        const u64 player = ctx.regs.x(ctx.regs.ctx, z.player_reg);
        const u64 off = rt.profile.grapple_aim.candidate_off;
        const u32 field = player != 0 && ctx.memory.IsValidVirtualAddressRange(player + off, 4)
                              ? ctx.memory.Read32(player + off)
                              : 0xffffffffu;
        const bool fresh = rt.grapple_source != nullptr && now - rt.grapple_source_ns < 100'000'000;
        LogPressTargets(ctx, player);
        LOG_INFO(Core, "VIZR hooks: grapple zl: press: PL+{:#x}={:#x} from {}{}", off, field,
                 fresh ? rt.grapple_source : "the game's own candidate (no grapple aim this tick)",
                 fresh ? fmt::format(" (at {}, the game's own {:#x})", rt.grapple_source_hook, rt.grapple_source_game)
                       : std::string{});
    }
    if (value != 0 && now >= rt.zl_trace_until) {
        rt.zl_trace_until = now + 500'000'000;
        rt.zl_trace_lines = 0;
    }
    if ((!changed && now >= rt.zl_trace_until) || rt.zl_trace_lines >= 30) {
        rt.zl_trace_last[i] = value;
        return;
    }
    rt.zl_trace_last[i] = value;
    ++rt.zl_trace_lines;
    const u64 player = ctx.regs.x(ctx.regs.ctx, z.player_reg);
    std::string extra;
    for (const u64 off : z.player_offs) {
        extra += fmt::format(" PL+{:#x}={:#x}", off,
                             player != 0 && ctx.memory.IsValidVirtualAddressRange(player + off, 4)
                                 ? ctx.memory.Read32(player + off)
                                 : 0u);
    }
    LOG_INFO(Core, "VIZR hooks: zl trace {}: w{}={:#x}{}", z.label, z.reg, value, extra);
}

/// Every session: one line at every ZL press (the held-ZL hook after a gap) with the game's candidate (a grapple point by
/// its vtable, another object, or none), whether this frame's candidate box came from the head camera ([orbit_head]),
/// and the player's [grapple_aim.zl_log] fields; six ticks later the same fields again (did it lock / grapple?).
void LogZlPress(const HitContext& ctx, const Profile::GrappleAim::Site& site) {
    Runtime& rt = ctx.rt;
    const auto& g = rt.profile.grapple_aim;
    const auto& z = g.zl_log;
    if (!z.present || ctx.hook.name != z.hook) {
        return;
    }
    auto& mem = ctx.memory;
    const u64 player = ctx.regs.x(ctx.regs.ctx, site.player_reg);
    const auto fields = [&] {
        std::string out;
        for (const u64 off : z.player_offs) {
            out += fmt::format(" PL+{:#x}={:#x}", off,
                               player != 0 && mem.IsValidVirtualAddressRange(player + off, 4) ? mem.Read32(player + off) : 0u);
        }
        return out;
    };
    const s64 now = SteadyNowNs();
    const bool press = now - rt.zl_last_ns > 150'000'000;
    rt.zl_last_ns = now;
    if (!press) {
        if (rt.zl_followup > 0 && --rt.zl_followup == 0) {
            LOG_INFO(Core, "VIZR hooks: zl press +6 ticks:{}", fields());
        }
        return;
    }
    rt.zl_followup = 6;
    const u32 id = static_cast<u32>(ctx.regs.x(ctx.regs.ctx, z.candidate_reg));
    const auto invalid = ReadScalar(ctx, g.invalid_field);
    std::string kind = "none";
    if (!invalid || id != static_cast<u32>(*invalid)) {
        kind = "object";
        u64 table = ctx.regs.x(ctx.regs.ctx, site.gl_reg);
        const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
        for (const u64 step : g.table_chain) {
            table = read64(table + step);
        }
        const auto module = rt.modules.find(g.vtable_module);
        if (table != 0 && module != rt.modules.end()) {
            table += g.table_add;
            const u64 e = read64(table + (id & g.id_index_mask) * g.stride);
            if (e != 0 && mem.IsValidVirtualAddressRange(e, SlotSpan(g)) && mem.Read32(e + g.id_check_off) == id) {
                const u64 object = read64(e + g.entity_off);
                const u64 vt = object != 0 ? read64(object) : 0;
                kind = vt == module->second.first + g.vtable_offset
                           ? "grapple point"
                           : fmt::format("object, vtable main+{:#x}", vt >= module->second.first ? vt - module->second.first : 0);
            }
        }
    }
    LOG_INFO(Core, "VIZR hooks: zl press: game candidate {:#x} ({}); candidate box from the {} camera;{}", id, kind,
             rt.orbit_head_ready ? "head" : "game", fields());
}

void RunGrappleAim(const HitContext& ctx, const Profile::GrappleAim::Site& site) {
    Runtime& rt = ctx.rt;
    const auto& g = rt.profile.grapple_aim;
    auto& mem = ctx.memory;
    const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
    LogZlPress(ctx, site);
    // For the press line (LogZlTrace): who supplies the candidate field after this hook.
    const auto source = [&](const char* who) {
        rt.grapple_source = who;
        rt.grapple_source_hook = ctx.hook.name;
        rt.grapple_source_ns = SteadyNowNs();
        const u64 pl = ctx.regs.x(ctx.regs.ctx, site.player_reg);
        rt.grapple_source_game = pl != 0 && mem.IsValidVirtualAddressRange(pl + g.candidate_off, 4)
                                     ? mem.Read32(pl + g.candidate_off)
                                     : 0xffffffffu;
    };
    if (!g.candidate_writes) {
        source("the game's own candidate (no hand writes)");
        return; // the game's own lock-on and grapple candidates
    }
    source("the game's own candidate (no hand pick this tick)"); // until a pick below says otherwise
    const HandPose hand = g_left_hand.Load();
    // In the Scan Visor the head picks the scan target (any scannable object), not the hand a grapple.
    const bool scan = g.scan_max_angle_deg > 0.0f && rt.scan_state_mask != 0 && rt.camera_world_valid &&
                      (g_input_states.load(std::memory_order_relaxed) & rt.scan_state_mask) != 0;
    if (!scan && (!rt.camera_world_valid || !hand.valid || SteadyNowNs() - hand.sample_ns > HeadFreshNs ||
                  !rt.space.camera_to_world || !rt.space.row_major)) {
        return; // no hand: the game's own (camera) choice
    }
    const u64 player = ctx.regs.x(ctx.regs.ctx, site.player_reg);
    const auto invalid = ReadScalar(ctx, g.invalid_field);
    const auto range_bits = ReadScalar(ctx, g.range_field);
    const auto module = rt.modules.find(g.vtable_module);
    if (player == 0 || !invalid || !range_bits || module == rt.modules.end() ||
        !mem.IsValidVirtualAddressRange(player + g.candidate_off, 4) ||
        !mem.IsValidVirtualAddressRange(player + g.list_off, 4)) {
        return;
    }
    float range{};
    const u32 bits = static_cast<u32>(*range_bits);
    std::memcpy(&range, &bits, sizeof(range));
    u64 table = ctx.regs.x(ctx.regs.ctx, site.gl_reg);
    for (const u64 step : g.table_chain) {
        table = read64(table + step);
    }
    const u64 table_parent = table; // the grapple-point list ([grapple_aim.points]) hangs off it too
    if (table != 0) {
        table += g.table_add; // the list is inline in its parent (GetObjectById 0xb0ed6c: ldr x0, [list, slot*16])
    }
    rt.orbit_table = table; // the lock ring looks its target up here
    rt.orbit_player = player;
    const u32 count = std::min<u32>(mem.Read32(player + g.list_off), 64);
    const u64 data = read64(player + g.list_off + g.list_data_off);
    if (scan) {
        source("the head's scan pick or the game's own candidate");
        RunScanPick(ctx, player, table, count, data, static_cast<u32>(*invalid), range);
        return;
    }
    const HandWorld hw = TurnAim(rt, HandToWorld(rt, hand, {0.0f, 0.0f, 0.0f}), g.left_aim_yaw_offset_deg);
    const float cos_max = std::cos(g.max_angle_deg * std::numbers::pi_v<float> / 180.0f);
    const u32 none = static_cast<u32>(*invalid);
    const u64 point_vtable = module->second.first + g.vtable_offset;
    // A grapple point's world position (point_chain from the entity), or nothing.
    const auto point_of = [&](u64 object) -> std::optional<std::array<float, 3>> {
        u64 at = object;
        for (const u64 step : g.point_chain) {
            at = read64(at + step);
        }
        if (at == 0 || !mem.IsValidVirtualAddressRange(at, 48)) {
            return std::nullopt;
        }
        std::array<float, 12> p{};
        mem.ReadBlock(at, p.data(), sizeof(p));
        return std::array<float, 3>{p[3], p[7], p[11]};
    };
    // The entity table's object for an id (the slot's id must match), or 0.
    const auto object_of = [&](u32 id) -> u64 {
        if (id == none || table == 0) {
            return 0;
        }
        const u64 e = read64(table + (id & g.id_index_mask) * g.stride);
        if (e == 0 || !mem.IsValidVirtualAddressRange(e, SlotSpan(g)) || mem.Read32(e + g.id_check_off) != id) {
            return 0;
        }
        return read64(e + g.entity_off);
    };
    // The grapple point an id names, or nothing (any other orbit target: enemies, scan points).
    const auto grapple_point = [&](u32 id) -> std::optional<std::array<float, 3>> {
        if (id == none || table == 0) {
            return std::nullopt;
        }
        const u64 e = read64(table + (id & g.id_index_mask) * g.stride);
        if (e == 0 || !mem.IsValidVirtualAddressRange(e, SlotSpan(g)) ||
            mem.Read32(e + g.id_check_off) != id || (mem.Read8(e + g.flags_off) & g.flags_mask) != g.flags_value) {
            return std::nullopt;
        }
        const u64 object = read64(e + g.entity_off);
        if (object == 0 || read64(object) != point_vtable) {
            return std::nullopt;
        }
        return point_of(object);
    };
    u32 best = none;
    float best_cos = cos_max;
    float best_dist = 0.0f;
    const bool anywhere = g.points.present && g_user_settings.Load().grapple_pick >= 0.5f;
    if (anywhere) {
        // Anywhere the hand points: every grapple point the game has loaded (its own list, not the camera's box), in
        // the player's area, active, from min_range to the lock range, within max_angle_deg of the hand's ray.
        const auto& pt = g.points;
        const u64 list = table_parent != 0 ? read64(table_parent + pt.list_ptr_off) : 0;
        const u64 l = list != 0 ? list + pt.list_off : 0;
        const s32 n_buckets = l != 0 && mem.IsValidVirtualAddressRange(l, 4) ? static_cast<s32>(mem.Read32(l)) : 0;
        const u32 buckets = static_cast<u32>(std::clamp<s32>(n_buckets, 0, 1024));
        const u64 array = l != 0 ? read64(l + pt.list_data_off) : 0;
        const u32 area = mem.IsValidVirtualAddressRange(player + pt.player_area_off, 4)
                             ? mem.Read32(player + pt.player_area_off)
                             : 0xffffffffu;
        u64 best_object = 0;
        u32 walked = 0, in_area = 0;
        constexpr u32 MaxWalk = 512; // a corrupt chain never loops for long
        for (u32 k = 0; k < buckets && array != 0 && walked < MaxWalk; ++k) {
            const u64 head = read64(array + k * pt.bucket_stride);
            for (u64 goc = head != 0 ? read64(head + pt.head_goc_off) : 0; goc != 0 && walked < MaxWalk;
                 goc = read64(goc + pt.goc_next_off)) {
                ++walked;
                const u64 object = read64(goc + pt.goc_entity_off);
                const u64 span = std::max({pt.id_off + 4, pt.area_off + 4, pt.active_off + 1});
                if (object == 0 || !mem.IsValidVirtualAddressRange(object, span) || read64(object) != point_vtable ||
                    (mem.Read8(object + pt.active_off) & pt.active_mask) == 0 || mem.Read32(object + pt.area_off) != area) {
                    continue;
                }
                ++in_area;
                const auto q = point_of(object);
                if (!q) {
                    continue;
                }
                const std::array<float, 3> d{(*q)[0] - hw.position[0], (*q)[1] - hw.position[1], (*q)[2] - hw.position[2]};
                const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (dist < pt.min_range || dist < 1e-3f || dist > range) {
                    continue;
                }
                const float c = (d[0] * hw.forward[0] + d[1] * hw.forward[1] + d[2] * hw.forward[2]) / dist;
                if (c >= best_cos) {
                    best_cos = c;
                    best = mem.Read32(object + pt.id_off);
                    best_dist = dist;
                    best_object = object;
                }
            }
        }
        if (area != rt.points_area_logged) {
            rt.points_area_logged = area;
            LOG_INFO(Core, "VIZR hooks: grapple points: area {:#x}, list L+0 = {} ({} walked, {} in this area)", area,
                     n_buckets, walked, in_area);
        }
        // The id goes into the game's candidate field, which the game resolves through its entity table: it must lead
        // back to this same entity, or the game's own candidate stays.
        if (best != none && object_of(best) != best_object) {
            const s64 now = SteadyNowNs();
            if (now - rt.points_resolve_logged_ns >= 1'000'000'000) {
                rt.points_resolve_logged_ns = now;
                LOG_WARNING(Core, "VIZR hooks: grapple points: id {:#x} (entity {:#x}) resolves to {:#x} in the entity "
                                  "table; the game's candidate stays",
                            best, best_object, object_of(best));
            }
            best = none;
        }
        if (best != rt.points_pick_logged) {
            rt.points_pick_logged = best;
            if (best != none) {
                LOG_INFO(Core, "VIZR hooks: grapple points: pick {:#x} at {:.1f} units, {:.1f} deg off the hand ray", best,
                         best_dist, std::acos(std::clamp(best_cos, -1.0f, 1.0f)) * 57.29578f);
            }
        }
    } else if (data != 0 && mem.IsValidVirtualAddressRange(data, count * 4)) {
        for (u32 n = 0; n < count; ++n) {
            const u32 id = mem.Read32(data + n * 4);
            const auto point = grapple_point(id);
            if (!point) {
                continue;
            }
            const auto& q = *point;
            const std::array<float, 3> d{q[0] - hw.position[0], q[1] - hw.position[1], q[2] - hw.position[2]};
            const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dist < 1e-3f || dist > range) {
                continue;
            }
            const float c = (d[0] * hw.forward[0] + d[1] * hw.forward[1] + d[2] * hw.forward[2]) / dist;
            if (c >= best_cos) {
                best_cos = c;
                best = id;
            }
        }
    }
    const u32 game = mem.Read32(player + g.candidate_off);
    // While the game's own candidate is a grapple point, what it chose, what the candidate field
    // holds after this hook, the hand's angle to the point and the game's gates, twice a second (every session: the
    // grapple fix is proven in the headset).
    const auto zl_log = [&](u32 written) {
        const auto& z = g.zl_log;
        const s64 now = SteadyNowNs();
        if (!z.present || ctx.hook.name != z.hook || now - rt.grapple_zl_logged_ns < 500'000'000) {
            return;
        }
        const u32 own = static_cast<u32>(ctx.regs.x(ctx.regs.ctx, z.candidate_reg));
        const auto point = grapple_point(own);
        if (!point) {
            return;
        }
        rt.grapple_zl_logged_ns = now;
        const std::array<float, 3> d{(*point)[0] - hw.position[0], (*point)[1] - hw.position[1],
                                     (*point)[2] - hw.position[2]};
        const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float c = dl > 1e-4f ? (d[0] * hw.forward[0] + d[1] * hw.forward[1] + d[2] * hw.forward[2]) / dl : 1.0f;
        std::string extra;
        for (const u64 off : z.player_offs) {
            extra += fmt::format(" PL+{:#x}={:#x}", off,
                                 mem.IsValidVirtualAddressRange(player + off, 4) ? mem.Read32(player + off) : 0u);
        }
        for (const auto& f : z.fields) {
            const auto v = ReadScalar(ctx, f);
            extra += fmt::format(" {}={}", f, v ? fmt::format("{:#x}", *v) : std::string{"-"});
        }
        LOG_INFO(Core, "VIZR hooks: grapple zl: game candidate {:#x} (grapple point), field {:#x}{}, hand {:.1f} deg off;{}",
                 own, mem.Read32(player + g.candidate_off), written == own ? "" : fmt::format(" (we wrote {:#x})", written),
                 std::acos(std::clamp(c, -1.0f, 1.0f)) * 57.29578f, extra);
    };
    if (best == none) {
        // The hand points at no grapple point:
        // the game's own candidate stays, whatever it is. Writing "none" over a game-picked grapple point
        // made grappling impossible whenever the hand was more than max_angle_deg off the point.
        source(anywhere ? "the game's own candidate (the hand points at no grapple point, anywhere)"
                        : "the game's own candidate (the hand points at no grapple point in the game's list)");
        zl_log(game);
        return;
    }
    if (rt.rig_logs && game != rt.grapple_logged_game) {
        rt.grapple_logged_game = game;
        LOG_INFO(Core, "VIZR hooks: grapple aim: {} in range, camera candidate {:#x}, hand pick {:#x} ({:.1f} deg)",
                 count, game, best, std::acos(std::clamp(best_cos, -1.0f, 1.0f)) * 57.29578f);
    }
    source(anywhere ? "the hand pick (anywhere)" : "the hand pick (the game's list)"); // the game's own, before the write
    mem.Write32(player + g.candidate_off, best);
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
    zl_log(best);
}

/// Keep the left arm drawn and place it at the left controller, each camera hook.
void RunLeftArmForce(const HitContext& ctx) {
    Runtime& rt = ctx.rt;
    const auto& a = rt.profile.arms;
    if (!a.left_flags_field.empty()) {
        const Field* field = rt.profile.FindField(a.left_flags_field);
        FieldState* state = field ? StateOf(rt, field) : nullptr;
        if (const auto address = state ? FieldAddress(ctx, *state) : std::nullopt) {
            const u32 flags = ctx.memory.Read32(*address);
            ctx.memory.Write32(*address, (flags | a.flags_set) & ~a.flags_clear);
        }
    }
    const HandPose hand = g_left_hand.Load();
    if (a.left_xf_field.empty() || !rt.camera_world_valid || !hand.valid ||
        SteadyNowNs() - hand.sample_ns > HeadFreshNs || !rt.space.camera_to_world || !rt.space.row_major) {
        return;
    }
    const Field* field = rt.profile.FindField(a.left_xf_field);
    FieldState* state = field ? StateOf(rt, field) : nullptr;
    const auto address = state ? FieldAddress(ctx, *state) : std::nullopt;
    if (!address || field->type != FieldType::Mat3x4) {
        return;
    }
    const HandWorld hw = HandToWorld(rt, hand, a.left_offset_m);
    std::array<float, 12> xf{};
    ctx.memory.ReadBlock(*address, xf.data(), sizeof(xf));
    const auto axis = [&](const std::string& name) {
        const auto neg = [](const std::array<float, 3>& v) { return std::array<float, 3>{-v[0], -v[1], -v[2]}; };
        return name == "left" ? hw.left : name == "right" ? neg(hw.left) : name == "up" ? hw.up
             : name == "down" ? neg(hw.up) : name == "forward" ? hw.forward : neg(hw.forward);
    };
    const std::array<std::array<float, 3>, 3> cols{axis(a.left_columns[0]), axis(a.left_columns[1]),
                                                   axis(a.left_columns[2])};
    for (std::size_t c = 0; c < 3; ++c) {
        const float length = std::sqrt(xf[c] * xf[c] + xf[4 + c] * xf[4 + c] + xf[8 + c] * xf[8 + c]);
        const float keep = length > 1e-3f ? length : 1.0f;
        for (std::size_t r = 0; r < 3; ++r) {
            xf[r * 4 + c] = cols[c][r] * keep;
        }
    }
    xf[3] = hw.position[0];
    xf[7] = hw.position[1];
    xf[11] = hw.position[2];
    ctx.memory.WriteBlock(*address, xf.data(), sizeof(xf));
    if (++rt.left_arm_writes == 1) {
        LOG_INFO(Core, "VIZR hooks: arms: left arm forced on and placed at the left controller");
    }
}

/// While the left hand is at the head, the game's own visor dial previews the choice (it
/// shows only the owned visors); nothing switches until the hand leaves (the XR thread then sends the D-pad).
void RunVisorDial(const HitContext& ctx) {
    const auto& d = ctx.rt.profile.visor_dial;
    const int slot = g_visor_preview.load(std::memory_order_relaxed);
    if (slot < 0) {
        ctx.rt.dial_slot = -1; // the next preview (even of the same visor) starts a new override
        return;
    }
    if (!d.choice_regs.empty()) {
        // The game's own selection call shows the highlight AND the title of the slot we pass.
        if (ctx.regs.set_x == nullptr || ctx.regs.set_s == nullptr) {
            return;
        }
        // The title only shows while the dial settles, so override just override_frames calls after a
        // new choice (stores it, then types its title) and let the game's own (current, 1.0) call settle it.
        float value = 0.0f;
        if (d.override_frames > 0) {
            if (slot != ctx.rt.dial_slot) {
                ctx.rt.dial_slot = slot;
                ctx.rt.dial_frames_left = d.override_frames;
            }
            if (ctx.rt.dial_frames_left == 0) {
                return;
            }
            const u32 k = d.override_frames - ctx.rt.dial_frames_left--;
            value = d.float_value - static_cast<float>(k) * d.float_alt;
        } else {
            // The value must differ from the last call's or the game skips the call.
            value = (++ctx.rt.dial_calls & 1) != 0 ? d.float_value : d.float_value - d.float_alt;
        }
        for (const u32 r : d.choice_regs) {
            ctx.regs.set_x(ctx.regs.ctx, r, static_cast<u64>(slot));
        }
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        ctx.regs.set_s(ctx.regs.ctx, d.float_reg, bits);
        ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 base = ctx.regs.x(ctx.regs.ctx, d.menu_reg) + d.menu_off;
    if (!ctx.memory.IsValidVirtualAddressRange(base, 8)) {
        return;
    }
    const u64 menu = ctx.memory.Read64(base);
    const u64 end = std::max({d.choice_off, d.turn_off, d.state_off, d.flags_off}) + 4;
    if (menu == 0 || !ctx.memory.IsValidVirtualAddressRange(menu, end)) {
        return;
    }
    ctx.memory.Write32(menu + d.choice_off, static_cast<u32>(slot));
    if (d.turn_off != 0) {
        u32 bits = 0;
        std::memcpy(&bits, &d.turn, sizeof(bits));
        ctx.memory.Write32(menu + d.turn_off, bits);
    }
    if (d.state_off != 0) {
        ctx.memory.Write32(menu + d.state_off, d.state);
    }
    if (d.flags_off != 0) {
        ctx.memory.Write32(menu + d.flags_off, ctx.memory.Read32(menu + d.flags_off) | d.flags_or);
    }
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
}

/// While the preview holds, keep the dial's highlight on the choice and its title at full
/// alpha (the fade timer held at its start value).
void RunVisorDialUpdate(const HitContext& ctx) {
    const auto& d = ctx.rt.profile.visor_dial;
    const int slot = g_visor_preview.load(std::memory_order_relaxed);
    if (slot < 0) {
        ctx.rt.dial_slot = -1; // the next preview starts a new override
        return;
    }
    const u64 base = ctx.regs.x(ctx.regs.ctx, d.menu_reg) + d.menu_off;
    if (!ctx.memory.IsValidVirtualAddressRange(base, 8)) {
        return;
    }
    const u64 menu = ctx.memory.Read64(base);
    const u64 end = std::max({d.choice_off, d.fade_off, d.fade_src_off, d.flags_off}) + 4;
    if (menu == 0 || !ctx.memory.IsValidVirtualAddressRange(menu, end)) {
        return;
    }
    ctx.memory.Write32(menu + d.choice_off, static_cast<u32>(slot));
    ctx.memory.Write32(menu + d.fade_off, ctx.memory.Read32(menu + d.fade_src_off));
    if (d.flags_off != 0) {
        ctx.memory.Write32(menu + d.flags_off, ctx.memory.Read32(menu + d.flags_off) | d.flags_or);
    }
    ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
}

/// The [hooks] entries RunHook hands to an enabled feature (each of those branches returns), plus the keep-alive hooks
/// and the hooks a camera rule writes at. Keep in step with RunHook: an installed hook outside this set would fall
/// through to the camera rules (the passthrough rule is attached to every hook), as arm_compose did in b52g-90x2.
std::unordered_set<std::string> ClaimedHooks(const Profile::GameProfile& p) {
    std::unordered_set<std::string> out;
    const auto add = [&](const std::string& name) {
        if (!name.empty()) {
            out.insert(name);
        }
    };
    if (p.grapple_aim.present) {
        for (const auto& site : p.grapple_aim.sites) {
            add(site.hook);
        }
    }
    for (const auto& trace : p.grapple_aim.zl_trace) {
        add(trace.hook);
    }
    if (p.arms.present) {
        add(p.arms.left_beam_hook);
        add(p.arms.left_render_hook);
        add(p.arms.hook);
        if (p.arms.ik) {
            add(p.arms.ik_hook);
        }
    }
    if (p.hud_widgets.present) {
        add(p.hud_widgets.enter_hook);
        add(p.hud_widgets.exit_hook);
        for (const auto& el : p.hud_widgets.elements) {
            if (el.kind == "corners") {
                add(el.corners_enter_hook);
                add(el.corners_exit_hook);
            }
        }
    }
    if (p.visor_dial.present) {
        add(p.visor_dial.hook);
        add(p.visor_dial.update_hook);
    }
    if (p.left_window.present) {
        add(p.left_window.prerender_hook);
        for (const auto& h : p.left_window.close_hooks) {
            add(h);
        }
        for (const auto& h : p.left_window.open_hooks) {
            add(h);
        }
        add(p.left_window.device_nodeid_hook);
        add(p.left_window.device_compose_hook);
    }
    if (p.orbit_head.present) {
        add(p.orbit_head.hook);
    }
    if (p.aim.present) {
        add(p.aim.ray_hook);
        add(p.aim.reticle_hook);
    }
    if (p.hands.present) {
        add(p.hands.hook);
        add(p.hands.fire_hook);
    }
    if (p.arm_measure.present) {
        add(p.arm_measure.compose_hook);
        add(p.arm_measure.render_hook);
        add(p.arm_measure.draw_hook);
    }
    if (p.hint_camera.present) {
        add(p.hint_camera.hook);
    }
    if (p.scan_lens.present) {
        for (const auto& h : p.scan_lens.hooks) {
            add(h);
        }
    }
    if (p.gui_eye.present) {
        add(p.gui_eye.proj_hook);
        add(p.gui_eye.view_hook);
        add(p.gui_eye.helmet_hook);
    }
    if (p.mode_c.present) {
        add(p.mode_c.hook);
        if (p.mode_c.backend == "look_at_copy") {
            add(p.mode_c.hook_right);
        }
    }
    for (const auto& h : p.alive_hooks) {
        add(h);
    }
    for (const auto& camera : p.cameras) {
        if (camera.mode != "passthrough") {
            if (const Field* target = p.FindField(camera.target); target != nullptr) {
                add(target->hook);
            }
        }
    }
    return out;
}

void RunHook(const HitContext& ctx) {
    if (ctx.rt.tick_hz == 0) {
        // The tick rate goes in at the first hook of any kind once the headset's refresh is known (the title screen),
        // before a save loads: the physics world takes its step when an area is created, so a later write would miss
        // the area already loaded.
        ApplyTickRate(ctx);
    }
    if (const auto& g = ctx.rt.profile.grapple_aim; g.present) {
        for (const auto& site : g.sites) {
            if (ctx.hook.name == site.hook) {
                RunGrappleAim(ctx, site);
                return;
            }
        }
    }
    if (const auto& a = ctx.rt.profile.arms; a.present && !a.left_beam_hook.empty() && ctx.hook.name == a.left_beam_hook) {
        RunLeftBeam(ctx);
        return;
    }
    if (const auto& hw = ctx.rt.profile.hud_widgets; hw.present) {
        for (std::size_t i = 0; i < hw.elements.size(); ++i) {
            const auto& el = hw.elements[i];
            if (el.kind == "corners" && (ctx.hook.name == el.corners_enter_hook || ctx.hook.name == el.corners_exit_hook)) {
                RunHudCorners(ctx, el, i, ctx.hook.name == el.corners_enter_hook);
                return;
            }
        }
    }
    if (const auto& dial = ctx.rt.profile.visor_dial; dial.present && !dial.update_hook.empty() &&
                                                       ctx.hook.name == dial.update_hook) {
        RunVisorDialUpdate(ctx);
        return;
    }
    if (const auto& w = ctx.rt.profile.left_window; w.present) {
        if (!w.prerender_hook.empty() && ctx.hook.name == w.prerender_hook) {
            RunLeftArmRender(ctx, true);
            return;
        }
        if (std::find(w.close_hooks.begin(), w.close_hooks.end(), ctx.hook.name) != w.close_hooks.end()) {
            ctx.rt.left_window = false;
            ++ctx.rt.lw_closes;
            LeftOrderEvent(ctx.rt, 'C');
            return;
        }
        if (std::find(w.open_hooks.begin(), w.open_hooks.end(), ctx.hook.name) != w.open_hooks.end()) {
            LeftWindowOpen(ctx, 'r');
            return;
        }
    }
    const auto& arms = ctx.rt.profile.arms;
    if (arms.present && !arms.left_render_hook.empty() && ctx.hook.name == arms.left_render_hook) {
        RunLeftArmRender(ctx);
        return;
    }
    if (arms.present && arms.ik && !arms.ik_hook.empty() && ctx.hook.name == arms.ik_hook) {
        RunArmIkHook(ctx);
        return;
    }
    if (arms.present && ctx.hook.name == arms.hook) {
        RunArmsLog(ctx);
        return;
    }
    const auto& dial = ctx.rt.profile.visor_dial;
    if (dial.present && ctx.hook.name == dial.hook) {
        RunVisorDial(ctx);
        return;
    }
    if (const auto& traces = ctx.rt.profile.grapple_aim.zl_trace; !traces.empty()) {
        for (std::size_t i = 0; i < traces.size(); ++i) {
            if (ctx.hook.name == traces[i].hook) {
                LogZlTrace(ctx, i);
                return;
            }
        }
    }
    if (const auto& oh = ctx.rt.profile.orbit_head; oh.present && ctx.hook.name == oh.hook) {
        // The candidate box is built from the head camera, not the game's (the buffer is this frame's head).
        if (!ctx.rt.orbit_head_ready && ctx.rt.orbit_facing_valid) {
            WriteOrbitFacing(ctx); // no body camera now (the camera hook went passthrough): the game's own facing
        }
        if (ctx.rt.orbit_head_ready && ctx.regs.set_x != nullptr) {
            const auto module = ctx.rt.modules.find(oh.buffer_module);
            if (module != ctx.rt.modules.end()) {
                ctx.regs.set_x(ctx.regs.ctx, oh.reg, module->second.first + oh.buffer_offset);
                ctx.hook.window_writes.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }
    const auto& aim = ctx.rt.profile.aim;
    if (aim.present && ctx.hook.name == aim.ray_hook) {
        RunCursorRay(ctx);
        return;
    }
    if (aim.present && !aim.reticle_hook.empty() && ctx.hook.name == aim.reticle_hook) {
        RunReticle(ctx);
        return;
    }
    const auto& hands = ctx.rt.profile.hands;
    if (hands.present && ctx.hook.name == hands.hook) {
        RunHand(ctx);
        return;
    }
    if (hands.present && !hands.fire_hook.empty() && ctx.hook.name == hands.fire_hook) {
        RunFire(ctx);
        return;
    }
    const auto& widgets = ctx.rt.profile.hud_widgets;
    if (widgets.present && ctx.hook.name == widgets.enter_hook) {
        RunHudEnter(ctx);
        return;
    }
    if (widgets.present && ctx.hook.name == widgets.exit_hook) {
        if (ctx.rt.hud_pending) {
            HudRestore(ctx);
        }
        return;
    }
    if (const auto& lw = ctx.rt.profile.left_window;
        lw.present && !lw.device_nodeid_hook.empty() && ctx.hook.name == lw.device_nodeid_hook) {
        // Rig: which node the game's locator name resolves to, for the grapple arm's three part calls.
        Runtime& rt = ctx.rt;
        auto& mem = ctx.memory;
        const auto read64 = [&](u64 at) -> u64 { return mem.IsValidVirtualAddressRange(at, 8) ? mem.Read64(at) : 0; };
        const u64 fp = ctx.regs.x(ctx.regs.ctx, 29);
        const u64 lr = fp != 0 ? read64(read64(fp) + 8) : 0; // GetScaledLocatorTransform's return into its caller
        const auto main_mod = rt.modules.find("main");
        const u64 base = main_mod != rt.modules.end() ? main_mod->second.first : 0;
        const u64 off = lr > base ? lr - base : 0;
        const int k = off == 0xc804e0 ? 0 : off == 0xc8054c ? 1 : off == 0xc805b8 ? 2 : -1;
        if (k >= 0) {
            rt.dev_nodeid[static_cast<std::size_t>(k)] = static_cast<u32>(ctx.regs.x(ctx.regs.ctx, 0));
            ++rt.dev_nodeid_hits[static_cast<std::size_t>(k)];
        }
        return;
    }
    if (const auto& lw = ctx.rt.profile.left_window;
        lw.present && !lw.device_compose_hook.empty() && ctx.hook.name == lw.device_compose_hook) {
        CaptureDeviceElbow(ctx);
        // Not a camera write: falling through to the camera rules (passthrough is attached to every hook) would add a
        // pose-ledger entry with no render fov per arm compose, and mode C would show every pair as mono (b52g-90x2).
        if (const auto& m = ctx.rt.profile.arm_measure; !(m.present && ctx.hook.name == m.compose_hook)) {
            return;
        }
    }
    if (const auto& m = ctx.rt.profile.arm_measure; m.present && ctx.hook.name == m.compose_hook) {
        // Test measurement M1: the composed abs of shoulder / elbow / wrist vs the solve's intent (every 60th hit).
        Runtime& rt = ctx.rt;
        if (rt.rig_logs && rt.lik_intended_valid && ++rt.measure_hits % 60 == 0) {
            auto& mem = ctx.memory;
            const u64 pose = ctx.regs.x(ctx.regs.ctx, m.pose_reg);
            const u64 banks = pose != 0 && mem.IsValidVirtualAddressRange(pose + rt.profile.arms.left_abs_banks_off, 8)
                                  ? mem.Read64(pose + rt.profile.arms.left_abs_banks_off)
                                  : 0;
            std::string line;
            for (std::size_t k = 0; k < 3 && banks != 0; ++k) {
                const u32 id = m.abs_ids[k];
                const u64 bank_at = banks + (id & 0x1f) * 8;
                const u64 at = mem.IsValidVirtualAddressRange(bank_at, 8) ? mem.Read64(bank_at) + ((id >> 5) & 0x7ff) * 8 + 8 : 0;
                std::array<float, 12> x{};
                if (at > 8 && mem.IsValidVirtualAddressRange(at, 48)) {
                    mem.ReadBlock(at, x.data(), sizeof(x));
                }
                const auto& want = rt.lik_intended[k];
                const float dx = x[3] - want[0], dy = x[7] - want[1], dz = x[11] - want[2];
                line += fmt::format(" {} abs ({:.3f},{:.3f},{:.3f}) intended ({:.3f},{:.3f},{:.3f}) off {:.3f};",
                                    k == 0 ? "shoulder" : k == 1 ? "elbow" : "wrist", x[3], x[7], x[11], want[0], want[1],
                                    want[2], std::sqrt(dx * dx + dy * dy + dz * dz));
            }
            LOG_INFO(Core, "VIZR hooks: arm measure M1 (composed):{}", line);
        }
        return;
    }
    if (const auto& m = ctx.rt.profile.arm_measure; m.present && ctx.hook.name == m.render_hook) {
        // Test measurement M2: the skinning matrices the GPU gets, as joint positions (skin_k x rest_k), vs the intent.
        Runtime& rt = ctx.rt;
        if (rt.rig_logs && rt.lik_intended_valid && ctx.regs.x(ctx.regs.ctx, m.char_reg) == rt.left_char &&
            ++rt.render_measure_hits % 60 == 0) {
            auto& mem = ctx.memory;
            const u64 data = ctx.regs.x(ctx.regs.ctx, m.render_data_reg);
            const u64 arr = data != 0 && mem.IsValidVirtualAddressRange(data + m.skin_ptr_off, 8)
                                ? mem.Read64(data + m.skin_ptr_off)
                                : 0;
            std::string line;
            for (std::size_t k = 0; k < 3 && arr != 0; ++k) {
                std::array<float, 12> x{};
                if (mem.IsValidVirtualAddressRange(arr + k * m.skin_stride, 48)) {
                    mem.ReadBlock(arr + k * m.skin_stride, x.data(), sizeof(x));
                }
                const auto& r = m.rest[k];
                const std::array<float, 3> p{x[0] * r[0] + x[1] * r[1] + x[2] * r[2] + x[3],
                                             x[4] * r[0] + x[5] * r[1] + x[6] * r[2] + x[7],
                                             x[8] * r[0] + x[9] * r[1] + x[10] * r[2] + x[11]};
                if (k == 2) {
                    rt.m2_wrist = p;
                    rt.m2_wrist_rot = {x[0], x[1], x[2], x[4], x[5], x[6], x[8], x[9], x[10]};
                    rt.m2_wrist_valid = true;
                } else if (k == 1) {
                    rt.m2_elbow = p;
                } else {
                    rt.m2_shoulder = p;
                }
                const auto& want = rt.lik_intended[k];
                const float dx = p[0] - want[0], dy = p[1] - want[1], dz = p[2] - want[2];
                line += fmt::format(" {} rendered ({:.3f},{:.3f},{:.3f}) intended ({:.3f},{:.3f},{:.3f}) off {:.3f};",
                                    k == 0 ? "shoulder" : k == 1 ? "elbow" : "wrist", p[0], p[1], p[2], want[0], want[1],
                                    want[2], std::sqrt(dx * dx + dy * dy + dz * dz));
            }
            LOG_INFO(Core, "VIZR hooks: arm measure M2 (skinning):{}", line);
        }
        return;
    }
    if (const auto& m = ctx.rt.profile.arm_measure; m.present && ctx.hook.name == m.draw_hook) {
        // Test measurement M3: the model-to-world the arm is drawn with, and where the skinned wrist lands.
        Runtime& rt = ctx.rt;
        auto& mem = ctx.memory;
        const u64 model = ctx.regs.x(ctx.regs.ctx, m.draw_model_reg);
        if (rt.rig_logs && model != rt.left_arm_ptr + m.model_data_off && rt.hand_world_valid && rt.camera_world_valid) {
            // M4: the cannon as drawn (a model whose origin is within 0.5 m of the cannon written this tick) against
            // the cannon composed from the camera current at this draw: what the player sees, cm in the camera's axes.
            const u64 sp = ctx.regs.sp(ctx.regs.ctx);
            if (mem.IsValidVirtualAddressRange(sp + m.draw_xf_sp_off, 48)) {
                std::array<float, 12> M{};
                mem.ReadBlock(sp + m.draw_xf_sp_off, M.data(), sizeof(M));
                const float u = std::max(rt.space.units_per_meter * g_user_settings.Load().world_scale, 1e-3f);
                const auto& w = rt.hand_world;
                const float to_written = std::sqrt((M[3] - w[0]) * (M[3] - w[0]) + (M[7] - w[1]) * (M[7] - w[1]) +
                                                   (M[11] - w[2]) * (M[11] - w[2])) / u;
                const HandPose rh = g_right_hand.Load();
                if (to_written < 0.5f && rh.valid && ++rt.cannon_measure_hits % 10 == 0) {
                    const auto& hs = rt.profile.hands;
                    const UserSettings us = g_user_settings.Load();
                    const HandWorld now = HandToWorld(rt, rh, {hs.offset_m[0], hs.offset_m[1] + us.cannon_up_m,
                                                               hs.offset_m[2] - us.cannon_forward_m});
                    const std::array<float, 3> e{(M[3] - now.position[0]) / u, (M[7] - now.position[1]) / u,
                                                 (M[11] - now.position[2]) / u};
                    const auto& c = rt.camera_world;
                    LOG_INFO(Core, "VIZR hooks: arm measure M4 cannon: drawn origin - this frame's cannon {:.1f} cm "
                             "(camera left {:.1f}, up {:.1f}, forward {:.1f}); drawn - written {:.1f} cm",
                             100.0f * std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]),
                             100.0f * (e[0] * c[0] + e[1] * c[4] + e[2] * c[8]), 100.0f * (e[0] * c[1] + e[1] * c[5] + e[2] * c[9]),
                             100.0f * (e[0] * c[2] + e[1] * c[6] + e[2] * c[10]), 100.0f * to_written);
                }
            }
        }
        if (rt.rig_logs && rt.left_arm_ptr != 0 && model == rt.left_arm_ptr + m.model_data_off && rt.m2_wrist_valid &&
            ++rt.draw_measure_hits % 10 == 0) {
            const u64 sp = ctx.regs.sp(ctx.regs.ctx);
            std::array<float, 12> M{};
            std::array<float, 3> S{};
            if (mem.IsValidVirtualAddressRange(sp + m.draw_xf_sp_off, 48) && mem.IsValidVirtualAddressRange(model, 12)) {
                mem.ReadBlock(sp + m.draw_xf_sp_off, M.data(), sizeof(M));
                mem.ReadBlock(model, S.data(), sizeof(S));
                const auto& w = rt.m2_wrist;
                const std::array<float, 3> ww{M[0] * w[0] + M[1] * w[1] + M[2] * w[2] + M[3],
                                              M[4] * w[0] + M[5] * w[1] + M[6] * w[2] + M[7],
                                              M[8] * w[0] + M[9] * w[1] + M[10] * w[2] + M[11]};
                const auto& t = rt.lik_target_world;
                const float d = std::sqrt((ww[0] - t[0]) * (ww[0] - t[0]) + (ww[1] - t[1]) * (ww[1] - t[1]) +
                                          (ww[2] - t[2]) * (ww[2] - t[2])) /
                                std::max(rt.space.units_per_meter * g_user_settings.Load().world_scale, 1e-3f);
                {
                    // The drawn wrist against the left hand composed from the camera current at this draw (the view's
                    // camera): what the player sees between the arm and the controller, in the camera's axes (cm).
                    const HandPose lh = g_left_hand.Load();
                    if (lh.valid && rt.camera_world_valid) {
                        const auto& lg = rt.profile.arms.left_grip_m;
                        const UserSettings us = g_user_settings.Load();
                        const std::array<float, 3> grip{lg[0], lg[1] + us.left_hand_up_m, lg[2] - us.left_hand_forward_m};
                        const HandWorld now_hand = HandToWorld(rt, lh, {-grip[0], -grip[1], -grip[2]});
                        const float u = std::max(rt.space.units_per_meter * g_user_settings.Load().world_scale, 1e-3f);
                        const std::array<float, 3> e{(ww[0] - now_hand.position[0]) / u, (ww[1] - now_hand.position[1]) / u,
                                                     (ww[2] - now_hand.position[2]) / u};
                        const auto& c = rt.camera_world;
                        LOG_INFO(Core, "VIZR hooks: arm measure M3 view: drawn wrist - this frame's hand {:.1f} cm "
                                 "(camera left {:.1f}, up {:.1f}, forward {:.1f}); camera written {:.2f} ms before the draw",
                                 100.0f * std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]),
                                 100.0f * (e[0] * c[0] + e[1] * c[4] + e[2] * c[8]),
                                 100.0f * (e[0] * c[1] + e[1] * c[5] + e[2] * c[9]),
                                 100.0f * (e[0] * c[2] + e[1] * c[6] + e[2] * c[10]),
                                 static_cast<float>(SteadyNowNs() - rt.camera_world_ns) * 1e-6f);
                    }
                }
                // The drawn hand: forward (fingers) and back of the hand = M x skin_wrist x their rest directions,
                // against the controller's wanted axes; the forearm (elbow -> wrist) direction drawn.
                const auto rot = [](const float* r, std::size_t stride, const std::array<float, 3>& v) {
                    std::array<float, 3> o{};
                    for (std::size_t i = 0; i < 3; ++i) {
                        o[i] = r[i * stride] * v[0] + r[i * stride + 1] * v[1] + r[i * stride + 2] * v[2];
                    }
                    const float n = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
                    for (float& c : o) {
                        c /= n > 1e-6f ? n : 1.0f;
                    }
                    return o;
                };
                const auto angle = [](const std::array<float, 3>& p, const std::array<float, 3>& q) {
                    return std::acos(std::clamp(p[0] * q[0] + p[1] * q[1] + p[2] * q[2], -1.0f, 1.0f)) * 57.29578f;
                };
                const auto hf = rot(M.data(), 4, rot(rt.m2_wrist_rot.data(), 3, m.hand_forward_rest));
                const auto hb = rot(M.data(), 4, rot(rt.m2_wrist_rot.data(), 3, m.hand_back_rest));
                const std::array<float, 3> fa_m{rt.m2_wrist[0] - rt.m2_elbow[0], rt.m2_wrist[1] - rt.m2_elbow[1],
                                                rt.m2_wrist[2] - rt.m2_elbow[2]};
                const auto fa = rot(M.data(), 4, fa_m);
                const auto& q = rt.m2_shoulder;
                const auto& an = rt.shoulder_left_world;
                const std::array<float, 3> ds{M[0] * q[0] + M[1] * q[1] + M[2] * q[2] + M[3] - an[0],
                                              M[4] * q[0] + M[5] * q[1] + M[6] * q[2] + M[7] - an[1],
                                              M[8] * q[0] + M[9] * q[1] + M[10] * q[2] + M[11] - an[2]};
                LOG_INFO(Core, "VIZR hooks: arm measure M3 shoulder: drawn {:.3f} m from the anchor",
                         std::sqrt(ds[0] * ds[0] + ds[1] * ds[1] + ds[2] * ds[2]) /
                             std::max(rt.space.units_per_meter * g_user_settings.Load().world_scale, 1e-3f));
                LOG_INFO(Core,
                         "VIZR hooks: arm measure M3 hand: forward error {:.1f} deg, back-of-hand error {:.1f} deg; drawn "
                         "forward ({:.3f},{:.3f},{:.3f}) wanted ({:.3f},{:.3f},{:.3f}); forearm ({:.4f},{:.4f},{:.4f})",
                         angle(hf, rt.lik_want_fwd_world), angle(hb, rt.lik_want_back_world), hf[0], hf[1], hf[2],
                         rt.lik_want_fwd_world[0], rt.lik_want_fwd_world[1], rt.lik_want_fwd_world[2], fa[0], fa[1], fa[2]);
                const auto& x = rt.left_xf;
                LOG_INFO(Core,
                         "VIZR hooks: arm measure M3 (draw): S ({:.3f},{:.3f},{:.3f}); M col lengths ({:.3f},{:.3f},{:.3f}) vs "
                         "our +0x920 ({:.3f},{:.3f},{:.3f}); M origin ({:.3f},{:.3f},{:.3f}) vs ours ({:.3f},{:.3f},{:.3f}); "
                         "drawn wrist ({:.3f},{:.3f},{:.3f}) vs target ({:.3f},{:.3f},{:.3f}): {:.3f} m",
                         S[0], S[1], S[2], std::hypot(M[0], M[4], M[8]), std::hypot(M[1], M[5], M[9]),
                         std::hypot(M[2], M[6], M[10]), std::hypot(x[0], x[4], x[8]), std::hypot(x[1], x[5], x[9]),
                         std::hypot(x[2], x[6], x[10]), M[3], M[7], M[11], x[3], x[7], x[11], ww[0], ww[1], ww[2], t[0], t[1],
                         t[2], d);
            }
        }
        return;
    }
    if (const auto& h = ctx.rt.profile.hint_camera; h.present && ctx.hook.name == h.hook) {
        // Logging: does a level camera hint's engine camera run while the ball is out?
        Runtime& rt = ctx.rt;
        ++rt.hint_hits;
        const auto cam = ReadScalar(ctx, h.camera_field);
        if (cam && *cam != 0 && *cam == ctx.regs.x(ctx.regs.ctx, h.camera_reg) && Eval(ctx, h.when)) {
            ++rt.hint_ball_hits;
        }
        if (const s64 now = SteadyNowNs(); rt.hint_ball_hits != 0 && now - rt.hint_logged_ns > 1'000'000'000) {
            rt.hint_logged_ns = now;
            LOG_INFO(Core, "VIZR hooks: hint camera: {} engine camera updates, {} of them the hint camera in ball form",
                     rt.hint_hits, rt.hint_ball_hits);
        }
        return;
    }
    if (const auto& l = ctx.rt.profile.scan_lens; l.present && std::ranges::find(l.hooks, ctx.hook.name) != l.hooks.end()) {
        RunScanLens(ctx);
        return;
    }
    if (const auto& g = ctx.rt.profile.gui_eye; g.present) {
        // The visor HUD's GUI camera per eye (render thread, the HUD pass).
        if (ctx.hook.name == g.proj_hook) {
            WriteGuiEye(ctx, ctx.regs.x(ctx.regs.ctx, g.proj_reg) + g.proj_off, 0);
            return;
        }
        if (ctx.hook.name == g.view_hook) {
            WriteGuiEye(ctx, 0, ctx.regs.sp(ctx.regs.ctx) + g.view_off);
            return;
        }
        if (!g.helmet_hook.empty() && ctx.hook.name == g.helmet_hook) {
            const u64 sp = ctx.regs.sp(ctx.regs.ctx);
            WriteGuiEye(ctx, sp + g.helmet_proj_off, sp + g.helmet_view_off);
            return;
        }
    }
    if (const auto& mc = ctx.rt.profile.mode_c;
        mc.present && (ctx.hook.name == mc.hook || (mc.backend == "look_at_copy" && ctx.hook.name == mc.hook_right))) {
        // not a camera write: no silence bookkeeping, no camera `when`
        if (mc.backend == "look_at_copy") {
            RunModeCLookAt(ctx, ctx.hook.name == mc.hook);
        } else {
            RunModeC(ctx);
        }
        return;
    }
    NoteHookHit();
    // A keep-alive hook that writes no camera (passthrough rules are attached to every hook, and their fields are not
    // readable here: evaluated, they flip between camera rules every frame) evaluates no camera rule.
    if (std::ranges::find(ctx.rt.profile.alive_hooks, ctx.hook.name) != ctx.rt.profile.alive_hooks.end() &&
        std::ranges::none_of(ctx.hook.cameras, [](const Camera* c) { return c->mode != "passthrough"; })) {
        // A keep-alive hook (a frame tick): the states stay current and the silence rule stays quiet; the
        // view is left to the camera hook (in ball form or a level camera it may not fire: the view stays 3D).
        PublishStates(ctx);
        return;
    }
    for (const Camera* camera : ctx.hook.cameras) {
        if (!Eval(ctx, camera->when)) {
            continue;
        }
        if (g_hook_state.flat_state.load(std::memory_order_relaxed)) {
            PublishStates(ctx); // The explicit flat state wins over every camera rule
            if (g_hook_state.flat_state.load(std::memory_order_relaxed)) {
                return;
            }
        }
        NoteCameraForCuts(ctx, *camera);
        // The player's comfort switch ("2D screen") turns this camera into passthrough.
        const bool as_screen = !camera->view_setting.empty() && ComfortSetting(camera->view_setting) < 0.5f;
        ctx.rt.body_camera = camera->mode != "passthrough" && !as_screen &&
                             std::ranges::find(ctx.rt.profile.arms.body_cameras, camera->id) !=
                                 ctx.rt.profile.arms.body_cameras.end();
        if (camera->mode == "passthrough" || as_screen) {
            g_torso_hold.store(true, std::memory_order_relaxed); // no body camera: the torso waits
            ctx.rt.orbit_head_ready = false;
            if (g_hook_state.camera_active.exchange(false)) {
                LOG_INFO(Core, "VIZR hooks: camera '{}' -> passthrough{}", camera->id,
                         as_screen ? fmt::format(" (comfort: {} = 2D screen)", camera->view_setting) : "");
            }
            g_vignette.store(0.0f, std::memory_order_relaxed);
            ctx.rt.vig_prev_valid = false;
            DropAimState(ctx.rt);
            PublishStates(ctx); // States (morph, fade) stay current in passthrough too
            return;
        }
        if (!ctx.rt.ray_seen) {
            ctx.rt.cursor_ptr = 0; // no cursor this tick (room change, other mode): never read an old one
        }
        ctx.rt.ray_seen = false;
        WriteCamera(ctx, *camera);
        UpdateVignette(ctx.rt, *camera);
        if (ctx.rt.body_camera && !ctx.rt.profile.arms.body_legs.empty()) {
            UpdateGait(ctx);
        }
        PublishStates(ctx);
        ApplyTickRate(ctx);
        if (ctx.rt.profile.snap.present) {
            RunSnap(ctx);
            RunTorso(ctx);
        }
        if (ctx.rt.profile.arms.present) {
            const auto& fields = ctx.rt.profile.arms.char_fields;
            for (std::size_t k = 0; k < ctx.rt.arm_chars.size() && k < fields.size(); ++k) {
                ctx.rt.arm_chars[k] = ReadScalar(ctx, fields[k]).value_or(0);
            }
            RunLeftArmForce(ctx);
        }
        return;
    }
    // No `when` matched: passthrough (format draft).
    ctx.rt.body_camera = false;
    ctx.rt.orbit_head_ready = false; // the head camera feeds no candidate box without a body camera
    g_vignette.store(0.0f, std::memory_order_relaxed);
    ctx.rt.vig_prev_valid = false;
    DropAimState(ctx.rt);
    PublishStates(ctx); // States (morph, fade) stay current in passthrough too
    if (g_hook_state.camera_active.exchange(false)) {
        LOG_INFO(Core, "VIZR hooks: no camera matched -> passthrough");
        // Once per entry into passthrough (every session: it explains a flat view): the value the first camera's
        // `when` field holds, module-relative, so its address can be checked against the live one.
        const auto main = ctx.rt.modules.find("main");
        for (const Camera* camera : ctx.hook.cameras) {
            if (camera->when.field.empty()) {
                continue;
            }
            const u64 v = ReadScalar(ctx, camera->when.field).value_or(0);
            const u64 rel = main != ctx.rt.modules.end() && v >= main->second.first ? v - main->second.first : v;
            LOG_INFO(Core, "VIZR hooks: no camera matched: {} = main+{:#x} (camera '{}' wants another value)",
                     camera->when.field, rel, camera->id);
            break;
        }
    }
}

void MaybeLogSecond(Runtime& rt, s64 now) {
    s64 last = rt.last_log_ns.load(std::memory_order_relaxed);
    if (now - last < 1'000'000'000 ||
        !rt.last_log_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        return;
    }
    // The menu changed something: the saver thread writes user.toml (asked at most once a second).
    rt.saver.Request(g_user_settings_changes.load());
    // Per hook: every second in test runs (with the callback timing the test checks read); every 10 s otherwise, with
    // the counts only (they still explain a session log, e.g. whether a hook ran while a button was held).
    if (!rt.rig_logs && now - rt.hook_lines_ns < 10'000'000'000) {
        return;
    }
    rt.hook_lines_ns = now;
    for (auto& hook : rt.hooks) {
        const u64 hits = hook->window_hits.exchange(0);
        const u64 writes = hook->window_writes.exchange(0);
        if (!rt.rig_logs) {
            LOG_INFO(Core, "VIZR hooks 10s: {} hits={} writes={} mode={}", hook->name, hits, writes,
                     g_hook_state.camera_active.load() ? "camera" : "passthrough");
            continue;
        }
        const u64 ns = hook->window_ns.exchange(0);
        const u64 max_ns = hook->window_max_ns.exchange(0);
        LOG_INFO(Core,
                 "VIZR hooks 1s: {} hits={} writes={} avg_us={:.2f} max_us={:.2f} run_p99_us={:.0f} "
                 "mode={} pose_id={} total_hits={} psi_deg={:.2f} psi_range_deg={:.2f}",
                 hook->name, hits, writes, hits != 0 ? static_cast<double>(ns) / static_cast<double>(hits) / 1000.0 : 0.0,
                 static_cast<double>(max_ns) / 1000.0, hook->P99Us(),
                 g_hook_state.camera_active.load() ? "camera" : "passthrough",
                 g_pose_ledger.LatestId(), g_hook_state.hits.load(), rt.psi * 57.29578f,
                 rt.psi_max >= rt.psi_min ? (rt.psi_max - rt.psi_min) * 57.29578f : 0.0f);
    }
    if (rt.rig_logs && rt.gcam_yaw_max >= rt.gcam_yaw_min) {
        LOG_INFO(Core, "VIZR hooks 1s: game camera yaw range {:.2f} deg, pitch range {:.2f} deg (yaw {:.2f}, pitch {:.2f})",
                 rt.gcam_yaw_max - rt.gcam_yaw_min, rt.gcam_pitch_max - rt.gcam_pitch_min, rt.gcam_yaw_max,
                 rt.gcam_pitch_max);
    }
    rt.gcam_yaw_min = rt.gcam_pitch_min = 1e9f;
    rt.gcam_yaw_max = rt.gcam_pitch_max = -1e9f;
    rt.psi_min = 1e9f;
    rt.psi_max = -1e9f;
}

// ---- install ------------------------------------------------------------------------------------

std::filesystem::path ProfilesRoot() {
    if (const char* env = std::getenv("VIZR_PROFILES"); env != nullptr && *env != '\0') {
        return std::filesystem::path{env};
    }
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "vizr" / "profiles";
}

/// Is every byte the patch writes already in guest memory? (Eden applied the pchtxt at load.)
bool PatchApplied(Core::Memory::Memory& memory, u64 main_base, const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file) {
        return false;
    }
    bool enabled = false;
    bool any = false;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.starts_with("@enabled")) {
            enabled = true;
        } else if (line.starts_with("@stop")) {
            break;
        } else if (enabled && !line.empty() && line[0] != '#' && line[0] != '@') {
            // IPSwitch allows "// comment" after a value; strip it.
            if (const auto comment = line.find("//"); comment != std::string::npos) {
                line.erase(comment);
            }
            const auto space = line.find(' ');
            if (space == std::string::npos) {
                continue;
            }
            // Eden adds offset_shift to reach the NSO file offset; with the 0x100 header that is
            // exactly the module offset written in the pchtxt.
            const std::string offset_text = line.substr(0, space);
            std::string hex;
            for (const char c : std::string_view{line}.substr(space + 1)) {
                if (!std::isspace(static_cast<unsigned char>(c))) {
                    hex += c;
                }
            }
            u64 offset = 0;
            const auto parsed = std::from_chars(offset_text.data(), offset_text.data() + offset_text.size(),
                                                offset, 16);
            if (parsed.ec != std::errc{} || hex.empty() || hex.size() % 2 != 0) {
                // A "string" value or anything else we cannot check: never claim the patch is in.
                LOG_WARNING(Core, "VIZR hooks: cannot check pchtxt line '{}' in {}", line,
                            path.filename().string());
                return false;
            }
            for (std::size_t i = 0; i < hex.size(); i += 2) {
                u8 expected = 0;
                const auto byte = std::from_chars(hex.data() + i, hex.data() + i + 2, expected, 16);
                if (byte.ec != std::errc{} || byte.ptr != hex.data() + i + 2) {
                    LOG_WARNING(Core, "VIZR hooks: cannot check pchtxt line '{}' in {}", line,
                                path.filename().string());
                    return false;
                }
                const u64 at = main_base + offset + i / 2;
                if (!memory.IsValidVirtualAddressRange(at, 1) || memory.Read8(at) != expected) {
                    return false;
                }
            }
            any = true;
        }
    }
    return any;
}

void Disable(const std::string& why) {
    // "Visible message": an error line that names the profile problem; Qt status text comes with
    // The overlay.
    LOG_ERROR(Core, "VIZR profile disabled: {}", why);
}

} // namespace

bool IsHookPc(void* process, u64 pc) {
    const Runtime* rt = g_runtime.load(std::memory_order_acquire);
    // Only the application's JITs: an applet with code at the same address is never hooked.
    return rt != nullptr && rt->process == process &&
           std::binary_search(rt->pcs.begin(), rt->pcs.end(), pc);
}

void OnHook(const void* process, u64 pc, const RegReader& regs, Core::Memory::Memory& memory) {
    Runtime* rt = g_runtime.load(std::memory_order_acquire);
    if (rt == nullptr || rt->process != process) {
        return;
    }
    const auto it = std::lower_bound(rt->pcs.begin(), rt->pcs.end(), pc);
    if (it == rt->pcs.end() || *it != pc) {
        return;
    }
    HookEntry& hook = *rt->hooks[static_cast<std::size_t>(it - rt->pcs.begin())];
    if (hook.disabled.load(std::memory_order_relaxed)) {
        return;
    }
    // One hook at a time across the guest's threads (the Runtime's threading contract).
    const std::scoped_lock hook_lock(rt->hook_mutex);
    // The callback's own timing (two clock reads, the histogram) only in test runs; a player's hit costs one clock read.
    const s64 start = rt->rig_logs ? NowNs() : 0;
    try {
        RunHook(HitContext{*rt, hook, regs, memory});
    } catch (const std::exception& e) {
        hook.disabled = true;
        g_hook_state.camera_active = false;
        LOG_ERROR(Core, "VIZR hooks: hook '{}' failed ({}); disabled for this launch",
                  hook.name, e.what());
    } catch (...) {
        hook.disabled = true;
        g_hook_state.camera_active = false;
        LOG_ERROR(Core, "VIZR hooks: hook '{}' failed; disabled for this launch", hook.name);
    }
    const s64 end = NowNs();
    hook.window_hits.fetch_add(1, std::memory_order_relaxed);
    if (rt->rig_logs) {
        const u64 took = static_cast<u64>(end - start);
        hook.window_ns.fetch_add(took, std::memory_order_relaxed);
        hook.histogram_us[(std::min<u64>)(took / 1000, hook.histogram_us.size() - 1)].fetch_add(
            1, std::memory_order_relaxed);
        u64 max = hook.window_max_ns.load(std::memory_order_relaxed);
        while (took > max && !hook.window_max_ns.compare_exchange_weak(max, took)) {
        }
    }
    MaybeLogSecond(*rt, end);
}

void Install(Core::System& system, Loader::AppLoader& loader) {
    Shutdown();
    ResetHookState();
    // A profile can never crash Eden: any exception here = no hooks. InstallImpl publishes some globals (stereo mode,
    // flat layers, snap mode, menu labels) before the runtime itself, so a failed install resets them all: nothing
    // stays half-installed.
    try {
        InstallImpl(system, loader);
    } catch (const std::exception& e) {
        ResetHookState();
        Disable(fmt::format("install failed: {}", e.what()));
    } catch (...) {
        ResetHookState();
        Disable("install failed");
    }
}

namespace {

/// Test diagnostics (VIZR_LAG_PROBE, VIZR_FLAT_FINDER, VIZR_LENS_*,...) act only with VIZR_RIG_OUT, which
/// the launcher clears for players.
/// The one test-mode gate: VIZR_RIG_OUT is read once per process.
bool RigMode() {
    static const bool rig = [] {
        const char* out = std::getenv("VIZR_RIG_OUT");
        return out != nullptr && *out != '\0';
    }();
    return rig;
}

void InstallImpl(Core::System& system, Loader::AppLoader& loader) {
    const auto& id_bytes = system.GetApplicationProcessBuildID();
    std::string build_id;
    for (const u8 b : id_bytes) {
        build_id += fmt::format("{:02x}", b);
    }
    const std::string title_id = fmt::format("{:016X}", system.GetApplicationProcessProgramID());
    const auto root = ProfilesRoot();
    std::string error;
    auto profile = Profile::Load(root, title_id, build_id, error);
    if (!profile) {
        if (error.empty()) {
            LOG_INFO(Core, "VIZR hooks: no profile for {} in {}", title_id, root.string());
        } else {
            Disable(error);
        }
        return;
    }
    // Format v0.2: what this profile would write differently, what the format removed, what is not read yet.
    for (const auto& note : profile->notes) {
        LOG_WARNING(Core, "VIZR hooks: profile: {}", note);
    }
    Kernel::KProcess* process = system.ApplicationProcess();
    if (process == nullptr || !process->Is64Bit()) {
        Disable("32-bit hooks are not supported yet (M3 is A64 only)");
        return;
    }
    Core::Memory::Memory& memory = system.ApplicationMemory();

    auto rt = std::make_unique<Runtime>();
    rt->profile = std::move(*profile);
    rt->space = rt->profile.space;
    rt->test_roll = LocalRoll(rt->space, TestRollDegrees);
    rt->probe_roll = LocalRoll(rt->space, ProbeRollDegrees);
    rt->process = process;
    for (std::size_t col = 0; col < 3; ++col) {
        for (std::size_t row = 0; row < 3; ++row) {
            rt->basis[row][col] = rt->space.local_basis[col][row];
        }
    }
    // Test runs only (gated on VIZR_RIG_OUT, as VIZR_RIG_CAM_BACK): the scan lens test knobs.
    if (RigMode()) {
        if (const char* lens = std::getenv("VIZR_LENS_REWRITE"); lens != nullptr && std::string_view{lens} == "0") {
            rt->lens_write = false; // count the scan lens hooks only
            LOG_WARNING(Core, "VIZR hooks: rig: scan lens rewrite off (VIZR_LENS_REWRITE=0)");
        }
        if (const char* mode = std::getenv("VIZR_LENS_MODE"); mode != nullptr) {
            const std::string_view m{mode};
            rt->lens_mode = m == "line" ? 0 : m == "rigid" ? 1 : m == "ray" ? 2 : 3;
            LOG_WARNING(Core, "VIZR hooks: rig: scan lens mode {} (VIZR_LENS_MODE)", rt->lens_mode);
        }
    }
    if (const char* probe = std::getenv("VIZR_LAG_PROBE"); probe != nullptr && *probe != '\0' && RigMode()) {
        rt->probe_every = std::strtoull(probe, nullptr, 10); // test runs only
    }

    // Module map from the loader: names "main", "subsdk0".., size = gap to the next module.
    Loader::AppLoader::Modules modules;
    if (loader.ReadNSOModules(modules) != Loader::ResultStatus::Success || modules.empty()) {
        Disable("the loader reports no NSO modules");
        return;
    }
    for (auto it = modules.begin(); it != modules.end(); ++it) {
        const auto next = std::next(it);
        const u64 size = next != modules.end() ? next->first - it->first : 0x10000000ULL;
        rt->modules[it->second] = {it->first, size};
    }
    if (!rt->modules.contains("main")) {
        Disable("no 'main' module");
        return;
    }

    // Symbols every hook and field asks for, per module (one .dynsym pass each).
    // A hook no enabled feature claims is not installed (it would only reach the camera rules).
    const auto claimed = ClaimedHooks(rt->profile);
    std::unordered_set<std::string> unclaimed;
    for (const auto& hook : rt->profile.hooks) {
        if (hook.enabled && !claimed.contains(hook.name)) {
            unclaimed.insert(hook.name);
            LOG_WARNING(Core, "VIZR hooks: unclaimed hook skipped: {}", hook.name);
        }
    }
    std::map<std::string, std::vector<std::string>, std::less<>> wanted;
    for (const auto& hook : rt->profile.hooks) {
        if (hook.enabled && !unclaimed.contains(hook.name) && !hook.sym.empty()) {
            wanted[hook.module].push_back(hook.sym);
        }
    }
    for (const auto& field : rt->profile.fields) {
        if (field.root == RootKind::Sym) {
            wanted["main"].push_back(field.sym);
        }
    }
    std::map<std::string, std::map<std::string, std::vector<u64>, std::less<>>, std::less<>> symbols;
    for (const auto& [mod_name, names] : wanted) {
        const auto mod = rt->modules.find(mod_name);
        if (mod == rt->modules.end()) {
            Disable(fmt::format("module '{}' is not loaded", mod_name));
            return;
        }
        symbols[mod_name] = FindSymbols(memory, mod->second.first, names);
    }

    // Hooks: all or none (of the claimed ones).
    for (const auto& hook : rt->profile.hooks) {
        if (!hook.enabled || unclaimed.contains(hook.name)) {
            continue;
        }
        const auto mod = rt->modules.find(hook.module);
        if (mod == rt->modules.end()) {
            Disable(fmt::format("hook '{}': module '{}' is not loaded", hook.name, hook.module));
            return;
        }
        const auto [base, size] = mod->second;
        std::optional<u64> by_sym;
        if (!hook.sym.empty()) {
            const auto& hits = symbols[hook.module][hook.sym];
            if (hits.size() > 1) {
                Disable(fmt::format("hook '{}': symbol {} matches {} symbols (never first-match)",
                                    hook.name, hook.sym, hits.size()));
                return;
            }
            if (hits.size() == 1) {
                by_sym = hits[0] + hook.sym_off;
            }
        }
        const std::optional<u64> by_offset =
            hook.offset ? std::optional<u64>{base + *hook.offset} : std::nullopt;
        if (by_sym && by_offset && *by_sym != *by_offset) {
            LOG_WARNING(Core, "VIZR hooks: hook '{}': sym gives {:#x}, offset gives {:#x}; using sym",
                        hook.name, *by_sym - base, *by_offset - base);
        }
        const std::optional<u64> landing = by_sym ? by_sym : by_offset;
        if (!landing) {
            Disable(fmt::format("hook '{}': symbol {} not found and no offset", hook.name, hook.sym));
            return;
        }
        const u64 check_at = *landing + static_cast<u64>(hook.expect_from);
        if (*landing < base || *landing >= base + size || *landing % 4 != 0 ||
            !memory.IsValidVirtualAddressRange(check_at, hook.expect.size())) {
            Disable(fmt::format("hook '{}': {:#x} is outside module '{}'", hook.name, *landing, hook.module));
            return;
        }
        std::vector<u8> actual(hook.expect.size());
        memory.ReadBlock(check_at, actual.data(), actual.size());
        if (actual != hook.expect) {
            Disable(fmt::format("hook '{}': expected bytes [{}] at {}+{:#x}, found [{}] (wrong build?)",
                                hook.name, Hex(hook.expect), hook.module, check_at - base, Hex(actual)));
            return;
        }
        auto entry = std::make_unique<HookEntry>();
        entry->name = hook.name;
        entry->pc = *landing;
        LOG_INFO(Core, "VIZR hooks: '{}' at {}+{:#x} via {}, expect OK", hook.name, hook.module,
                 *landing - base, by_sym ? "sym" : "offset");
        rt->hooks.push_back(std::move(entry));
    }
    if (rt->hooks.empty()) {
        Disable("no enabled hooks");
        return;
    }
    std::sort(rt->hooks.begin(), rt->hooks.end(),
              [](const auto& a, const auto& b) { return a->pc < b->pc; });
    for (const auto& hook : rt->hooks) {
        rt->pcs.push_back(hook->pc);
    }

    // Fields: static/sym roots resolved once; hooks named must exist.
    for (const auto& field : rt->profile.fields) {
        auto state = std::make_unique<FieldState>();
        state->field = &field;
        if (!field.hook.empty() && !unclaimed.contains(field.hook) &&
            std::none_of(rt->hooks.begin(), rt->hooks.end(),
                         [&](const auto& h) { return h->name == field.hook; })) {
            Disable(fmt::format("field '{}' names hook '{}', which is not installed", field.name, field.hook));
            return;
        }
        if (field.root == RootKind::Static) {
            const auto mod = rt->modules.find(field.module);
            if (mod == rt->modules.end()) {
                Disable(fmt::format("field '{}': module '{}' is not loaded", field.name, field.module));
                return;
            }
            state->root_address = mod->second.first + field.module_offset;
        } else if (field.root == RootKind::Sym) {
            const auto& hits = symbols["main"][field.sym];
            if (hits.size() != 1) {
                Disable(fmt::format("field '{}': symbol {} matches {} symbols", field.name, field.sym,
                                    hits.size()));
                return;
            }
            state->root_address = hits[0];
        }
        rt->fields.push_back(std::move(state));
    }

    // Cameras: a camera runs at the hook of its target field; a passthrough camera at every
    // hook (its `when` fields decide; unreadable fields make it false).
    for (const auto& camera : rt->profile.cameras) {
        const Field* target = rt->profile.FindField(camera.target);
        if (camera.mode != "passthrough") {
            if (target == nullptr || target->hook.empty()) {
                Disable(fmt::format("camera '{}': target '{}' is not a hook field", camera.id, camera.target));
                return;
            }
            if (rt->profile.FindField(camera.source) == nullptr) {
                Disable(fmt::format("camera '{}': source '{}' is not a field", camera.id, camera.source));
                return;
            }
            if (camera.look_at) {
                for (const std::string* name : {&camera.source, &camera.source_at, &camera.source_up, &camera.target,
                                                &camera.target_at, &camera.target_up}) {
                    const Field* f = rt->profile.FindField(*name);
                    if (f == nullptr || f->type != FieldType::Vec3f) {
                        Disable(fmt::format("camera '{}': look_at field '{}' is not a vec3f field", camera.id, *name));
                        return;
                    }
                }
            }
        }
        for (auto& hook : rt->hooks) {
            if (camera.mode == "passthrough" || target->hook == hook->name) {
                hook->cameras.push_back(&camera);
            }
        }
    }
    for (const auto& camera : rt->profile.cameras) {
        if (camera.mode != "passthrough") {
            g_hook_state.silence_frames = camera.hook_silence_frames;
            g_hook_state.pose_lag_frames = camera.pose_lag_frames;
            rt->pose_lag_default = camera.pose_lag_frames;
            rt->pose_lag_at = &camera.pose_lag_frames_at;
            // M4 yaw source: explicit key, else "player_yaw" when the address file has it.
            const std::string yaw_name = !camera.yaw_source.empty() && camera.yaw_source != "camera" ? camera.yaw_source
                                         : rt->profile.FindField("player_yaw") ? std::string{"player_yaw"}
                                                                               : std::string{};
            if (!yaw_name.empty()) {
                const Field* yaw = rt->profile.FindField(yaw_name);
                if (yaw == nullptr || (yaw->type != FieldType::Mat3x4 && yaw->type != FieldType::Mat4)) {
                    LOG_ERROR(Core, "VIZR hooks: yaw_source '{}' is not a mat3x4/mat4 field; using the camera",
                              yaw_name);
                } else {
                    rt->yaw_field = yaw;
                    const auto back = Column(rt->basis, 2);
                    rt->yaw_forward = {-back[0], -back[1], -back[2]}; // default: [space] forward_axis
                    if (const std::string& axis = camera.yaw_source_forward; axis.size() == 2) {
                        rt->yaw_forward = {0.0f, 0.0f, 0.0f};
                        rt->yaw_forward[static_cast<std::size_t>(axis[1] - 'x')] = axis[0] == '-' ? -1.0f : 1.0f;
                    }
                    LOG_INFO(Core, "VIZR hooks: game yaw from '{}' (forward {:+.0f},{:+.0f},{:+.0f})", yaw_name,
                             rt->yaw_forward[0], rt->yaw_forward[1], rt->yaw_forward[2]);
                }
            }
            LOG_INFO(Core, "VIZR hooks: hook_silence_frames={} pose_lag_frames={} (camera '{}')",
                     camera.hook_silence_frames, camera.pose_lag_frames, camera.id);
            break;
        }
    }

    // Per-element HUD settings (game.toml [hud]); the mod menu stores into the same state.
    // The player's settings (user.toml) over game.toml [hud].
    {
        UserSettings user;
        user.hud = rt->profile.hud;
        rt->user_toml = UserToml::PathFor(rt->profile.dir);
        rt->saver.Start(rt->user_toml);
        std::vector<std::string> notes;
        UserToml::Load(rt->user_toml, user, notes);
        for (const auto& note : notes) {
            LOG_WARNING(Core, "VIZR hooks: user.toml: {}", note);
        }
        LOG_INFO(Core, "VIZR hooks: player settings from {} (whole HUD scale {:.2f}, world scale {:.2f}, eye height {:+.2f} m)",
                 rt->user_toml.string(), user.whole.scale, user.world_scale, user.eye_height_m);
        {
            const std::scoped_lock lock(g_user_settings_write);
            g_user_settings.Store(user);
        }
        {
            const std::scoped_lock lock(g_menu_labels_mutex);
            g_menu_labels.clear();
            g_menu_labels.insert(rt->profile.menu_labels.begin(), rt->profile.menu_labels.end());
        }
        // The presenter applies settings when this count changes; it may have read the defaults before the
        // install. Bump it so the player's settings reach it.
        g_user_settings_changes.fetch_add(1, std::memory_order_relaxed);
        // Format v0.2: a v0.1 name or user.toml's [state] was read: save once now (new names, runtime.toml), not only
        // after the next menu change. Otherwise the file already holds these settings.
        if (std::ranges::any_of(notes, [](const std::string& n) {
                return n.find("format v0.2") != std::string::npos;
            })) {
            LOG_INFO(Core, "VIZR hooks: player settings: migrating {} to format v0.2 (saved within a second)",
                     rt->user_toml.string());
        } else {
            rt->saver.MarkSaved(g_user_settings_changes.load());
        }
        rt->profile.hud = user.hud; // the log line below shows what applies
    }
    {
        std::string changed;
        for (std::size_t i = 0; i < rt->profile.hud.count; ++i) {
            const HudElement& e = rt->profile.hud.elements[i];
            if (!e.Identity()) {
                changed += fmt::format(" {}(offset {:.2f},{:.2f} scale {:.2f} alpha {:.2f} depth {:.1f})",
                                       rt->profile.hud.names[i].data(),
                                       e.offset[0], e.offset[1], e.scale, e.alpha, e.depth);
            }
        }
        const bool hooked = rt->profile.hud_widgets.present &&
                            std::ranges::any_of(rt->hooks, [&](const auto& h) { return h->name == rt->profile.hud_widgets.enter_hook; }) &&
                            std::ranges::any_of(rt->hooks, [&](const auto& h) { return h->name == rt->profile.hud_widgets.exit_hook; });
        LOG_INFO(Core, "VIZR hooks: HUD elements {}:{}", hooked ? "hooked" : "NOT hooked (no [hud_widgets] or hooks)",
                 changed.empty() ? std::string{" all as the game draws them"} : changed);
    }

    // Stereo mode for this launch.
    g_snap_instant = rt->profile.snap.present; // Instant snap turn, else the XR thread's closed loop
    for (const std::string& name : rt->profile.fade_states) {
        const auto& states = rt->profile.states;
        const auto it = std::ranges::find_if(states, [&](const auto& s) { return s.first == name; });
        if (it == states.end()) {
            LOG_WARNING(Core, "VIZR hooks: [fade] state '{}' is not in [states]; ignored", name);
        } else {
            rt->fade_mask |= 1u << static_cast<u32>(it - states.begin());
        }
    }
    if (rt->profile.aim.scan_from_head && !rt->profile.aim.scan_state.empty()) {
        const auto& states = rt->profile.states;
        const auto it = std::ranges::find_if(states, [&](const auto& s) { return s.first == rt->profile.aim.scan_state; });
        if (it == states.end()) {
            LOG_WARNING(Core, "VIZR hooks: [aim] scan_state '{}' is not in [states]; the scan ray follows the hand",
                        rt->profile.aim.scan_state);
        } else {
            rt->scan_state_mask = 1u << static_cast<u32>(it - states.begin());
        }
    }
    if (RigMode()) {
        rt->rig_logs = true;
        if (const char* old = std::getenv("VIZR_RIG_ANCHOR_APPLIED"); old != nullptr && *old == '1') {
            rt->rig_anchor_applied = true;
            LOG_WARNING(Core, "VIZR hooks: rig: the shoulder anchor faces the applied body (VIZR_RIG_ANCHOR_APPLIED)");
        }
        // The network gate's positive control: what the guest's news:* service does on first use (a build without
        // VIZR_OFFLINE fetches the release list on its own thread; this one only logs that news is off).
        if (const char* probe = std::getenv("VIZR_RIG_NET_PROBE"); probe != nullptr && std::string_view{probe} == "news") {
            LOG_WARNING(Core, "VIZR rig: network probe: opening the built-in news (VIZR_RIG_NET_PROBE=news)");
            Service::News::EnsureBuiltinNewsLoaded();
        }
        if (const char* back = std::getenv("VIZR_RIG_CAM_BACK"); back != nullptr && *back != '\0') {
            rt->rig_cam_back = std::clamp(static_cast<float>(std::atof(back)), 0.0f, 20.0f);
            LOG_WARNING(Core, "VIZR hooks: rig camera pulled back {:.1f} units (VIZR_RIG_CAM_BACK)", rt->rig_cam_back);
        }
        // Test-only field writes ("field=value,field=value", u32), e.g. the Morph Ball grant so the
        // ball can be tested on a save without it. Only with VIZR_RIG_OUT, which the launcher clears.
        if (const char* list = std::getenv("VIZR_RIG_WRITES"); list != nullptr && *list != '\0') {
            std::string_view rest{list};
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                const std::string_view item = rest.substr(0, comma);
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                const auto eq = item.find('=');
                if (eq == std::string_view::npos) {
                    continue;
                }
                u32 value = 0;
                const std::string_view num = item.substr(eq + 1);
                const bool hex = num.starts_with("0x") || num.starts_with("0X");
                const std::string_view digits = hex ? num.substr(2) : num;
                if (std::from_chars(digits.data(), digits.data() + digits.size(), value, hex ? 16 : 10).ec != std::errc{}) {
                    LOG_ERROR(Core, "VIZR hooks: VIZR_RIG_WRITES '{}': bad number; skipped", item);
                    continue;
                }
                rt->rig_writes.emplace_back(std::string{item.substr(0, eq)}, value);
            }
            LOG_WARNING(Core, "VIZR hooks: RIG WRITES ACTIVE ({}): {} field(s); not a real save state", list,
                        rt->rig_writes.size());
        }
    }
    // A launch-only grant ("field=value,..." u32, e.g. an item the save does not have yet), applied as the test writes
    // are. Only the fields the profile lists in [session_grants] can be written; anything else is refused. Kept in the
    // save only if the player saves in game.
    if (const char* list = std::getenv("VIZR_SESSION_WRITES"); list != nullptr && *list != '\0' && RigMode()) {
        LOG_ERROR(Core, "VIZR hooks: session writes refused in test mode (VIZR_RIG_OUT); test runs use VIZR_RIG_WRITES");
    } else if (list != nullptr && *list != '\0') {
        std::size_t added = 0;
        std::string_view rest{list};
        while (!rest.empty()) {
            const auto comma = rest.find(',');
            const std::string_view item = rest.substr(0, comma);
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            const auto eq = item.find('=');
            u32 value = 0;
            if (eq == std::string_view::npos ||
                std::from_chars(item.data() + eq + 1, item.data() + item.size(), value).ec != std::errc{}) {
                LOG_ERROR(Core, "VIZR hooks: VIZR_SESSION_WRITES '{}': not field=number; skipped", item);
                continue;
            }
            const std::string field{item.substr(0, eq)};
            if (std::ranges::find(rt->profile.session_grants, field) == rt->profile.session_grants.end()) {
                LOG_ERROR(Core, "VIZR hooks: VIZR_SESSION_WRITES '{}': not a field the profile allows ([session_grants]); "
                          "refused", item);
                continue;
            }
            rt->rig_writes.emplace_back(field, value);
            ++added;
        }
        LOG_WARNING(Core, "VIZR hooks: session writes: {} ({} field(s), this launch only; kept if the game is saved)", list,
                    added);
    }
    g_stereo_mode = rt->profile.stereo == "afr"      ? StereoMode::Afr
                    : rt->profile.stereo == "mode_c" ? StereoMode::ModeC
                                                     : StereoMode::Mono;
    if (g_stereo_mode.load() == StereoMode::ModeC) {
        const Profile::ModeC& spec = rt->profile.mode_c;
        const auto installed = [&](const std::string& name) {
            return std::any_of(rt->hooks.begin(), rt->hooks.end(), [&](const auto& h) { return h->name == name; });
        };
        if (!spec.present || !installed(spec.hook) || (spec.backend == "look_at_copy" && !installed(spec.hook_right))) {
            // Without the eye cameras both eyes would be the centre image: stay mono.
            LOG_ERROR(Core, "VIZR hooks: stereo mode_c needs the address file [mode_c] and its hook installed; mono");
            g_stereo_mode = StereoMode::Mono;
        } else {
            g_modec_marker_left = spec.marker_left;
            g_modec_marker_right = spec.marker_right;
            g_modec_hold_clear_target = spec.backend == "look_at_copy"; // The eyes draw into the presented buffer
        }
    }
    {
        FlatConfig flat;
        for (std::size_t i = 0; i < rt->profile.flat_layers.size() && flat.count < flat.hashes.size(); ++i) {
            flat.scales[flat.count] = rt->profile.flat_scales[i];
            flat.hashes[flat.count++] = rt->profile.flat_layers[i];
        }
        flat.hud_fit = rt->profile.hud_fit;
        flat.hud_fit_deg = rt->profile.hud_fit_deg;
        flat.hud_scale = rt->profile.hud_scale;
        flat.hud_depth_m = rt->profile.hud_depth_m;
        const char* finder = std::getenv("VIZR_FLAT_FINDER");
        flat.finder = finder != nullptr && finder[0] == '1' && RigMode(); // test runs only
        g_flat_config.Store(flat);
        g_flat_generation.fetch_add(1, std::memory_order_relaxed); // new hud values: new flat pipelines
        g_flat_active = flat.count > 0 || flat.finder;
        // Mode C: say which path the HUD takes.
        const std::string hud_path = g_stereo_mode.load() != StereoMode::ModeC ? std::string{}
                                     : flat.count > 0 ? std::string{", hud path: flat layer (hashes)"}
                                                      : std::string{", hud path: game HUD pass per eye"};
        LOG_INFO(Core, "VIZR hooks: stereo {}, hud_scale {}, hud_depth_m {:.2f}, flat layers {}{}{}; menus (camera hook silent) at scale 1.0",
                 rt->profile.stereo,
                 flat.hud_fit ? fmt::format("fit +/-{:.0f} deg", flat.hud_fit_deg) : fmt::format("{:.2f}", flat.hud_scale),
                 flat.hud_depth_m, flat.count, flat.finder ? ", flat finder ON" : "", hud_path);
    }

    // Touch -> Pro Controller layout (profile data). A bad layout disables only the Touch map.
    if (!rt->profile.layout_path.empty()) {
        std::string layout_error;
        std::vector<std::string> layout_warnings;
        std::vector<std::string> state_names;
        for (const auto& [name, when] : rt->profile.states) {
            state_names.push_back(name);
        }
        const InputLayout layout = Profile::LoadLayout(rt->profile.dir / rt->profile.layout_path,
                                                       rt->profile.recenter, layout_error, layout_warnings,
                                                       state_names);
        for (const auto& warning : layout_warnings) {
            LOG_WARNING(Core, "VIZR hooks: Touch layout: {}", warning);
        }
        // One line the test runs gate reads: "Touch layout VALID" or "Touch layout INVALID".
        if (layout.valid) {
            g_input_layout.Store(layout);
            LOG_INFO(Core, "VIZR hooks: Touch layout VALID {} (recenter = {}, chord sets {}, skipped entries {})",
                     rt->profile.layout_path, rt->profile.recenter,
                     (layout.modifier != TouchInput::Count ? 1 : 0) + (layout.modifier2 != TouchInput::Count ? 1 : 0),
                     layout_warnings.size());
        } else {
            LOG_ERROR(Core, "VIZR hooks: Touch layout INVALID (no Touch input): {}", layout_error);
        }
    }

    // Frame-rate patch: present_hz only when the patch bytes are really in memory.
    for (const auto& patch : rt->profile.patches) {
        if (!patch.enabled || patch.present_hz == 0) {
            continue;
        }
        if (!patch.pchtxt.empty() &&
            PatchApplied(memory, rt->modules["main"].first, rt->profile.dir / patch.pchtxt)) {
            g_present_hz = patch.present_hz;
            LOG_INFO(Core, "VIZR hooks: patch '{}' is applied; composing at {} Hz", patch.name,
                     patch.present_hz);
        } else {
            LOG_WARNING(Core, "VIZR hooks: patch '{}' wants {} Hz but its bytes are not in memory (enable it in Eden's mods); staying at 60 Hz",
                        patch.name, patch.present_hz);
        }
    }

    LOG_INFO(Core, "VIZR hooks: profile '{}' ({}) installed, {} hook(s), {} field(s), {}",
             rt->profile.name, title_id, rt->hooks.size(), rt->fields.size(),
             g_test_roll_flag.load() ? "test roll 30 deg" : "loopback");

    std::vector<u64> pcs = rt->pcs;
    g_hook_state.installed = true;
    g_runtime.store(rt.release(), std::memory_order_release);
    // Code translated before the table existed must be retranslated (cheat engine does the same).
    for (const u64 pc : pcs) {
        Core::InvalidateInstructionCacheRange(process, pc, 4);
    }
}

} // namespace

void Shutdown() {
    // Guest cores are stopped, so no hook or translation can be running.
    delete g_runtime.exchange(nullptr, std::memory_order_acq_rel);
    ResetHookState();
}

} // namespace VIZR::Hooks

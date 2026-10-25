// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "common/common_types.h"

namespace VIZR {

/// Set by the --no-vr launch flag. Turns VR headset output off for this process only, without
/// changing the saved vr_output setting.
inline std::atomic<bool> g_no_vr_flag{false};

/// Set by the --vr-test-roll launch flag: the hook rolls the game camera by a fixed 30 degrees
/// (repeats the roll proof on this build). Without it the hook writes the
/// camera back unchanged (loopback).
inline std::atomic<bool> g_test_roll_flag{false};

/// Rate at which Eden composes guest frames and signals the guest's vsync (stock: 60). Set from
/// An enabled profile patch with present_hz; 60 otherwise.
inline std::atomic<u32> g_present_hz{60};
/// The headset's refresh as the XR session reports it after its request (0 until known): the tick rate follows it.
inline std::atomic<u32> g_display_hz{0};
/// The profile's cap for "Match headset" ([tick_rate] match_max; 0 = none): the presenter asks the runtime for it.
inline std::atomic<u32> g_tick_match_max_hz{0};
/// The presenter's fresh-frame watchdog: when the game's frames stopped keeping up with the headset at a game rate above
/// 60, the game rate is held at this for the rest of the session (0 = no cap), and the rate it was capped from.
inline std::atomic<u32> g_tick_session_cap_hz{0};
inline std::atomic<u32> g_tick_capped_from_hz{0};
/// The left stick is pushed (the presenter, every frame): the rig's movement log runs only then and shortly after.
inline std::atomic<bool> g_move_stick_held{false};

/// Single writer, any number of readers, no lock (seqlock). The value is kept in relaxed atomic
/// words, so a torn read is retried, never used.
template <typename T>
class SeqLocked {
    static_assert(std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>);
    static constexpr std::size_t Words = (sizeof(T) + sizeof(u64) - 1) / sizeof(u64);

public:
    /// Starts holding T{} (its member defaults), not all-zero words. A reader before the first Store got
    /// zeros: the presenter's first frame read user settings with every Compatibility value 0, so "Hold last 3D frame"
    /// (default on) was off in every launch.
    SeqLocked() {
        Store(T{});
    }

    void Store(const T& value) {
        std::array<u64, Words> raw{};
        std::memcpy(raw.data(), &value, sizeof(T));
        const u32 v = version.load(std::memory_order_relaxed);
        version.store(v + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < Words; ++i) {
            words[i].store(raw[i], std::memory_order_relaxed);
        }
        version.store(v + 2, std::memory_order_release);
    }

    T Load() const {
        std::array<u64, Words> raw{};
        u32 before = 0;
        u32 after = 0;
        do {
            before = version.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < Words; ++i) {
                raw[i] = words[i].load(std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            after = version.load(std::memory_order_relaxed);
        } while ((before & 1) != 0 || before != after);
        T out{};
        std::memcpy(&out, raw.data(), sizeof(T));
        return out;
    }

private:
    std::atomic<u32> version{0};
    std::array<std::atomic<u64>, Words> words{};
};

/// A head pose from the OpenXR runtime, in the session's LOCAL space (Y up, -Z forward, metres).
struct HeadPose {
    std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
    std::array<float, 3> position{};
    s64 display_time{}; // XrTime (ns) the pose was predicted for
    s64 sample_ns{};    // steady_clock ns when the XR thread sampled it (freshness check)
    std::array<float, 3> recenter_position{}; // The head's LOCAL position at the last recenter (6DoF origin)
    float recenter_yaw{};  // Head yaw (rad, about LOCAL up) that counts as "straight ahead"
    float headset_vfov{};  // The eyes' vertical FOV (rad, up + down), 0 until known
    float ipd_m{};         // Distance between the runtime's two eye positions (m), 0 until known
    /// The player's torso yaw (rad, about LOCAL up, left positive) since the last recenter, estimated from the
    /// headset and both controllers; the hook runtime turns the game body by it (Move forward follows = Body).
    float torso_offset{};
    bool torso_valid{};
    u32 recenter_count{};  // bumped by every recenter (the torso offset restarts at 0)
    bool valid{};
};

/// M5 stereo mode (profile [render] stereo): Mono = one image in both eyes; Afr = alternate
/// Eye frames, each camera write renders one eye (mode A).
/// ModeC = the game renders its frame once per eye (patch + render-job hook).
enum class StereoMode : u8 { Mono, Afr, ModeC };
inline std::atomic<StereoMode> g_stereo_mode{StereoMode::Mono};

/// Per-element HUD settings from the profile [hud] and the mod
/// menu. The identity is the game's own HUD. Which elements exist is per game (address file
/// [hud_widgets] elements); the core only keeps a fixed-capacity list in that order.
inline constexpr std::size_t HudMaxElements = 16;
struct HudElement {
    std::array<float, 2> offset{}; // screen heights at the element's depth, +x right, +y up
    float scale{1.0f};             // about the element root's position
    float alpha{1.0f};             // multiplies the game's alpha (0 hides)
    float depth{0.0f};             // frame units along the view axis, + = farther
    bool Identity() const {
        return offset[0] == 0.0f && offset[1] == 0.0f && scale == 1.0f && alpha == 1.0f && depth == 0.0f;
    }
};
struct HudConfig {
    std::array<HudElement, HudMaxElements> elements{};
    std::array<std::array<char, 24>, HudMaxElements> names{}; // from the address file, for logs and the menu
    u32 count{};
};

/// Everything the player can change in the mod menu. Defined, saved and
/// parsed through the table in common/vizr_settings.h.
struct UserSettings {
    HudConfig hud{};          // per element (offset, scale, alpha, depth)
    HudElement whole{};       // the whole HUD (scale, depth, height = offset[1]) about the frame centre
    float world_scale{1.0f};  // multiplies the profile's units_per_meter
    float eye_height_m{0.0f}; // added to the camera's height offset
    float snap_turn{1.0f};    // 1 = snap turn (default), 0 = smooth (the right stick passes through)
    float snap_deg{30.0f};    // Degrees per snap
    float snap_fire{0.7f};          // The right stick's |x| past which a push turns
    float snap_rearm{0.3f};         // ... and below which it counts as released
    float snap_rearm_ms{120.0f};    // A release re-arms the next snap only after this long below snap_rearm
    float snap_reverse_ms{300.0f};  // An opposite-direction push this soon after a snap is ignored (spring-back)
    float crouch_morph{1.0f}; // 1 = crouching presses the layout's crouch_press (morph ball), 0 = off
    float crouch_drop_m{0.45f}; // How far below the standing head height counts as crouched
    float selected_visor{1.0f}; // The head gesture's selected ring direction (0 up, 1 right, 2 down, 3 left)
    float cannon_forward_m{0.0f}; // The hand-held object moved along the hand (+ = forward)
    float cannon_up_m{0.0f};      // ... along the hand's up
    float cannon_scale{1.0f};     // ... and its size
    float show_body{1.0f};        // 1 = the character's body below the view (where the profile has one), 0 = hidden
    float body_height{1.0f};      // The body's size (1 = the game's model)
    float body_follow{1.0f};      // 1 = standing stance, torso lean and planted feet from the head: the chest follows
                                  // the spine and the body sits behind the eye point, clear of the first-person view
    float head_position{1.0f};    // 1 = the head's room position moves the view (6DoF), 0 = rotation only
    float lean_limit_m{0.5f};     // How far (m) the view follows the head from the recenter point
    float arm_follow{1.0f};       // 1 = the arm runs from the cannon toward the body's shoulder, 0 = the game's
    float comfort_fade{1.0f};     // 1 = black over forced camera moves ([fade] states), 0 = see them
    // [compat], the menu's Compatibility tab. 0 = automatic (today's behaviour) unless noted.
    float compat_render_scale{0.0f}; // next launch: 0 = automatic (the runtime's size, the launcher's cap), 1/1.5/2/3
    float compat_refresh_hz{0.0f};   // live: 0 = automatic (the highest up to 120), 72/80/90/120 where offered
    float compat_game_fps{60.0f};    // the game's tick rate: 60 (default), 0 = match the headset's refresh, 90 or 120
    float compat_shaders{0.0f};      // next launch: 0 = automatic, 1 = smooth (async), 2 = exact
    float compat_gpu_accuracy{0.0f}; // next launch: 0 = automatic, 1 = normal (Eden Low), 2 = high
    float compat_frame_hold{1.0f};   // live: 1 = hold the last 3D frame on a stutter, 0 = allow the flat frame (debug)
    float visor_true_helmet{0.0f};
    float left_hand_forward_m{0.0f}; // The left palm's grip point moved along the hand (+ = forward)
    float left_hand_up_m{0.0f};      // ... and along its up
    float arm_length_m{0.63f};       // The player's shoulder-to-wrist length; the drawn arm is scaled to it
    float move_forward{2.0f};        // the left stick's forward: 0 the head, 1 the left controller, 3 the right
                                     // controller, 2 (the default) the body, which follows the player's real torso;
                                     // 4: the body follows the head every frame, the stick the left controller
    float grapple_pick{0.0f};        // the left hand's grapple pick: 0 (the default) among the points the game's
                                     // view offers, 1 anywhere the hand points (the profile's [grapple_aim.points])
    float body_shoulder_weight{0.7f}; // Body mode: how far the body turns toward the hands' direction (0 = the head's yaw)
    float body_smoothing_s{0.25f};   // ... and how smoothly it follows (time constant, s)
    float move_hand_offset_deg{20.0f}; // A controller's facing turned this far toward the body centre (a relaxed hand points outward)
    // Per-camera comfort, named by the profile's
    // cameras (view_setting / vignette_setting): 1 = 3D, 0 = the flat screen; vignette = the tunnel's strength.
    float ball_view{1.0f};
    float ball_vignette{0.5f};
    float cutscene_view{1.0f};
    float cutscene_vignette{0.5f};
    float shoulder_right_m{0.0f}; // Added to the profile's shoulder (so the arm does not come out of the chest)
    float shoulder_down_m{0.0f};
    float shoulder_back_m{0.0f};
};
inline SeqLocked<UserSettings> g_user_settings;
/// SeqLocked allows one writer at a time, and the settings are written from the install thread (the player's user.toml)
/// and the XR thread (the mod menu, the visor gesture). Every Load-modify-Store of g_user_settings holds this lock, so
/// no change is lost and no two Stores interleave.
inline std::mutex g_user_settings_write;

/// The game's own words for menu settings (game.toml [menu_labels], set at install): key = a setting's user.toml key or
/// "tab.<name>". The settings table's labels are game-neutral; a profile can name things as its game does.
inline std::mutex g_menu_labels_mutex;
inline std::map<std::string, std::string, std::less<>> g_menu_labels;
inline std::string MenuLabel(std::string_view key, std::string_view fallback) {
    const std::scoped_lock lock(g_menu_labels_mutex);
    const auto it = g_menu_labels.find(key);
    return it != g_menu_labels.end() ? it->second : std::string{fallback};
}
/// Bumped by the menu on every change; the hook runtime saves user.toml when it moved.
inline std::atomic<u64> g_user_settings_changes{0};
/// F1 in Eden's window: open / close the mod menu (the XR thread takes it).
inline std::atomic<bool> g_menu_toggle_request{false};
/// The mod menu is open: the game gets a neutral pad (XR thread).
inline std::atomic<bool> g_menu_open{false};

/// Mode C eye markers: the clear colours the game's patch gives each eye's pass (profile
/// [mode_c] eye_marker_left/right). Written once at install, before the guest runs.
inline std::array<float, 4> g_modec_marker_left{};
inline std::array<float, 4> g_modec_marker_right{};
/// Mode C: marker-L clears seen (the game frame number on the GPU thread).
inline std::atomic<u64> g_modec_frame{0};
/// Each eye draws straight into the presented frame buffer, so at ([mode_c] backend look_at_copy)
/// marker R the clear target itself holds the finished left eye and is the image to hold. Prime (render_job) holds
/// the image its content-to-main copy wrote and never the clear target (its content target). Set once at install.
inline std::atomic<bool> g_modec_hold_clear_target{false};
/// Mode C: set by the presenter while it draws the left-eye slot, so the display lookup returns
/// the held left copy of the presented image.
inline thread_local bool g_modec_present_held = false;
/// Mode C counters for the per-second presenter line.
struct ModeCStats {
    std::atomic<u64> markers_l{0};
    std::atomic<u64> markers_r{0};
    std::atomic<u64> held_copies{0};
    std::atomic<u64> no_hold{0};     // marker R with no candidate image to hold
    std::atomic<u64> held_miss{0};   // left slot asked for the held copy of an image without one
    std::atomic<u64> paired{0};      // frames published as an L/R pair
    std::atomic<u64> stale{0};       // frames published mono (no left image of this frame)
    std::atomic<u64> eye_sets{0};    // render-job hook camera writes
    std::atomic<u64> centre_kept{0}; // pass 0 L found its own last write (node not refreshed)
};
inline ModeCStats g_modec_stats;

/// Which eye a camera write rendered (pose ledger); Both = mono.
enum class Eye : u8 { Left = 0, Right = 1, Both = 2 };

/// M5 flat layers (HUD, visor): pipelines whose shader hash is listed are
/// scaled about the view centre by hud_scale and drawn at a constant disparity for hud_depth_m.
struct FlatConfig {
    std::array<u64, 16> hashes{}; // shader hashes (any stage of the pipeline)
    std::array<float, 16> scales{}; // per entry: its own scale (> 0), or 0 = hud_scale / fit
    u32 count{};
    bool hud_fit{};              // hud_scale = "fit": the full game frame within +/-hud_fit_deg
    float hud_fit_deg{35.0f};    // The fit cone's half-angle ([render] hud_fit_deg)
    float hud_scale{1.0f};       // a number 0.3..1.0 when not "fit"
    float hud_depth_m{1.0f};     // apparent distance of the flat layers
    bool finder{};               // VIZR_FLAT_FINDER: cycle the flat flag through live hashes
};
inline SeqLocked<FlatConfig> g_flat_config;
/// Flat pipelines drawn while the camera hook is silent (the game does not tick in the pause
/// Screen or the map) keep scale 1.0, so menus are full size; gameplay gets the fit.
inline constexpr u64 FlatMenuVariantBit = u64{1} << 55;
/// The guest size of the image the game presents (presenter); flat layers only apply to
/// render targets of at least half this size (the minimap's own texture smeared when shrunk).
inline std::atomic<u32> g_present_guest_width{0};
inline std::atomic<u32> g_present_guest_height{0};
/// Fast path for the pipeline cache: true when the config lists hashes or the finder is on.
inline std::atomic<bool> g_flat_active{false};

/// The eye the GPU is drawing now (AFR alternates; set from the last published frame's eye).
inline std::atomic<Eye> g_draw_eye{Eye::Both};

/// GraphicsPipelineCacheKey::vizr_variant: the low byte is the FlatVariant, the bits above it
/// the flat-value generation, so a pipeline compiled with old values (IPD, FOV, hud_scale,
/// hud_depth_m) is never used once they change. Variant pipelines are never
/// written to the disk cache.
enum class FlatVariant : u64 { None = 0, Mono = 1, Left = 2, Right = 3 };
inline std::atomic<u64> g_flat_generation{1};

/// The IPD and vertical FOV the flat values were last computed from (XR thread only).
struct FlatBasis {
    float ipd_m{};
    float vfov{};
};

/// XR thread, every frame: bumps the generation when the IPD moves by more than 0.5 mm or the
/// vertical FOV by more than 0.5 degrees since the values were last taken.
inline void NoteFlatBasis(FlatBasis& last, float ipd_m, float vfov) {
    const bool moved = std::abs(ipd_m - last.ipd_m) > 0.0005f || std::abs(vfov - last.vfov) > 0.0087f;
    if (moved) {
        last = {ipd_m, vfov};
        g_flat_generation.fetch_add(1, std::memory_order_relaxed);
    }
}

/// True while an OpenXR session can exist for this renderer (runtime found and not turned off).
inline std::atomic<bool> g_xr_active{false};

/// The newest head pose; written by the XR thread after each xrWaitFrame, read by the hook.
/// Only the XR thread talks to OpenXR (PROFILE-FORMAT-DRAFT core rule 7).
inline SeqLocked<HeadPose> g_latest_head;

/// Pose ledger: every camera the hook wrote gets an
/// id and the head pose it used, so the presenter can submit an image with the pose it was
/// rendered with. 16 entries = 0.25 s at 60 Hz.
class PoseLedger {
public:
    static constexpr u64 Size = 16;

    /// What a camera write used: the head pose (un-recentered, LOCAL) and the vertical FOV the
    /// game renders that frame with (the projection layer's FOV).
    struct Rendered {
        HeadPose pose{};
        float render_vfov{};
        /// The eye this camera write rendered. The presenter routes an
        /// image by this, never by frame parity.
        Eye eye{Eye::Both};
    };

    /// Hook thread. Returns the new pose id (never 0).
    u64 Append(const HeadPose& pose, float render_vfov = 0.0f, Eye eye = Eye::Both) {
        const u64 id = next_id.fetch_add(1, std::memory_order_relaxed) + 1;
        entries[id % Size].Store(Entry{id, Rendered{pose, render_vfov, eye}});
        latest_id.store(id, std::memory_order_release);
        return id;
    }

    /// Any thread. Empty if the id is 0 or already overwritten.
    std::optional<Rendered> Find(u64 id) const {
        if (id == 0) {
            return std::nullopt;
        }
        const Entry entry = entries[id % Size].Load();
        if (entry.id != id) {
            return std::nullopt;
        }
        return entry.rendered;
    }

    u64 LatestId() const {
        return latest_id.load(std::memory_order_acquire);
    }

private:
    struct Entry {
        u64 id{};
        Rendered rendered{};
    };
    std::atomic<u64> next_id{0};
    std::atomic<u64> latest_id{0};
    std::array<SeqLocked<Entry>, Size> entries{};
};

inline PoseLedger g_pose_ledger;

/// Hook state shared by the core (hook), nvnflinger (frame tag) and the renderer (silence count).
/// All of it is inert (no hooks, tag 0) unless a profile installed hooks.
struct HookState {
    std::atomic<bool> installed{false};
    /// True while the last hook hit wrote a camera; false in passthrough (a `when` chose it, or
    /// the hook went silent).
    std::atomic<bool> camera_active{false};
    std::atomic<u32> frames_since_hook{0};
    std::atomic<u32> silence_frames{3};
    /// Hotfix: when the camera hook
    /// last fired; passthrough needs this long without it as well as silence_frames published frames.
    std::atomic<s64> last_hook_ns{0};
    std::atomic<u64> passthrough_events{0};
    /// The profile's explicit flat
    /// state ([render] flat_state: not in a session, loading, a movie, the map, a pause menu) holds. With a flat state the
    /// camera hook going quiet no longer means passthrough; only the keep-alive hooks going quiet does (title, loading).
    std::atomic<bool> flat_state{false};
    std::atomic<u64> hits{0};
    /// Lag probe (VIZR_LAG_PROBE): the pose id whose camera gets the probe roll; 0 = none.
    std::atomic<u64> lag_probe_id{0};
    /// The gameplay camera's pose_lag_frames (profile); the presenter looks up tag - this.
    std::atomic<u32> pose_lag_frames{0};
};

inline HookState g_hook_state;

/// M4 frames-dropped-per-hitch counters: headset refreshes missed, the part of
/// them that fell inside a game hitch, and game hitches. Cumulative for the session.
struct XrDropStats {
    std::atomic<u64> xr_missed{0};
    std::atomic<u64> xr_missed_in_hitch{0};
    std::atomic<u64> game_hitches{0};
    /// steady_clock ns: while a hitch is open (no game frame for 2 periods), until one period after it closed.
    std::atomic<s64> hitch_until_ns{0};
};

inline XrDropStats g_xr_drops;

/// steady_clock ns of the last published game frame (0 = none yet).
inline std::atomic<s64> g_last_published_ns{0};

s64 SteadyNowNs();
/// The game frame period from the base rate (60 Hz or present_hz).
s64 GamePeriodNs();
/// True while a game hitch is open, or closed less than one game period ago.
bool GameHitchActive(s64 now_ns);

// --- M4 Touch -> Pro Controller ------------------------------------------

/// Touch inputs the XR thread reads (oculus/touch_controller).
enum class TouchInput : u8 {
    LeftX, LeftY, LeftMenu, LeftStickClick, LeftTrigger, LeftGrip,
    RightA, RightB, RightStickClick, RightTrigger, RightGrip,
    RightStickUp, RightStickDown, RightStickLeft, RightStickRight, // chord sources only
    LeftStickUp, LeftStickDown, LeftStickLeft, LeftStickRight,     // chord sources only
    LeftHandHead, LeftHandCannon, // M7 gestures (left hand at the head / on the right controller): chord modifiers
    Count,
};

/// Pro Controller targets; values 0..15 equal Eden's VirtualGamepad::VirtualButton ids.
enum class ProTarget : s8 {
    None = -1, A = 0, B, X, Y, LStickClick, RStickClick, L, R, ZL, ZR, Plus, Minus,
    DLeft, DUp, DRight, DDown,
    Recenter = 100, // handled by the XR thread, never sent to the game
};

/// A profile's layout file, flattened (trivially copyable so it can live in a SeqLocked).
struct InputLayout {
    std::array<ProTarget, static_cast<std::size_t>(TouchInput::Count)> map{};
    std::array<ProTarget, static_cast<std::size_t>(TouchInput::Count)> chord{};
    TouchInput modifier{TouchInput::Count}; // Count = no chord modifier
    ProTarget modifier_tap{ProTarget::None}; // sent as a tap when the modifier is released unused
    /// [chords] modifier_mode = "hold": the modifier keeps its [map] target, chords are one tap per
    /// press or flick, no stick is zeroed. false = "tap" (modifier swallowed, chords level-held).
    bool modifier_hold{};
    /// Second chord set (a [chords.<name>] sub-table, e.g. Prime's beam select on the left grip):
    /// always "hold" mode with the latch; `hold_also2` is pressed for as long as `modifier2` is
    /// held. While both modifiers are held the first set wins.
    std::array<ProTarget, static_cast<std::size_t>(TouchInput::Count)> chord2{};
    TouchInput modifier2{TouchInput::Count}; // Count = no second set
    ProTarget hold_also2{ProTarget::None};
    /// Hold-mode stick chords fire on the stick's return when it was past the flick threshold for
    /// at most this long; longer is a strafe/turn (no chord). [chords] flick_max_ms, default 250.
    u32 flick_max_ms{250};
    float deadzone{0.12f};
    /// Hotfix: [map.menu] overrides used while the game is in a menu (title, pause, map: the camera
    /// hook is silent), e.g. Touch A = Pro A (confirm) and Touch B = Pro B (back) when gameplay maps them elsewhere.
    std::array<ProTarget, static_cast<std::size_t>(TouchInput::Count)> map_menu{};
    std::array<bool, static_cast<std::size_t>(TouchInput::Count)> menu_override{};
    /// Layout [when]: a source's own [map] target is sent only while one of these game
    /// states holds (bit i = the address file's [states] entry i); 0 = always.
    std::array<u32, static_cast<std::size_t>(TouchInput::Count)> when{};
    /// Layout [gestures]. crouch_press is tapped when the head drops below the standing height by
    /// the menu's crouch distance (and again on standing up); crouch_state = the [states] bits that mean "already
    /// down" (0 = the XR thread keeps its own toggle). ring_labels: the visor ring's up/right/down/left labels
    /// (empty = no ring).
    ProTarget crouch_press{ProTarget::None};
    u32 crouch_state{};
    u32 crouch_block{}; // [states] bits while which no crouch tap is sent (Prime: morphing / unmorphing)
    std::array<std::array<char, 16>, 4> ring_labels{};
    /// The head gesture picks with these presses (up, right, down, left): hold + flick (headset correction)
    /// chooses, the switch happens when the hand leaves; a tap toggles between ring_home (Combat) and the selected
    /// one. home_state = the [states] bits that mean "the home visor is on".
    std::array<ProTarget, 4> ring_press{ProTarget::None, ProTarget::None, ProTarget::None, ProTarget::None};
    s32 ring_home{0};
    u32 home_state{};
    /// The game's visor slot per ring direction (up, right, down, left); set = the game's own
    /// dial previews the choice and no custom ring is drawn. -1 = no slot.
    std::array<s32, 4> ring_slots{-1, -1, -1, -1};
    /// Per ring direction the [states] bits meaning "owned"; 0 = always (the ring offers only what the player owns).
    /// An unowned direction is not drawn and a flick toward it chooses nothing.
    std::array<u32, 4> ring_owned{};
    /// Per ring direction the [states] bits meaning "this visor is on" (head-tap cycling); all 0 = the old toggle.
    std::array<u32, 4> ring_state{};
    bool valid{};
};
/// The address file's [states], evaluated by the camera hook each frame (bit i = state i).
inline std::atomic<u32> g_input_states{0};
/// The headset shows black while one of the address file's [fade] states holds (MORPH-BALL.md, transitions)
/// (Prime: the morph-ball transition, when the game's camera flies out of or into the head). Camera hook writes.
inline std::atomic<bool> g_fade_active{false};
/// The comfort vignette's strength now (0 = none, 1 = the narrowest tunnel), from the game camera's own
/// motion while a camera with a vignette_setting writes. Camera hook writes, the XR thread draws it.
inline std::atomic<float> g_vignette{0.0f};
/// A short black fade over camera cuts (cutscene shot changes, entering and leaving a
/// cutscene): the headset shows black until this steady-clock time (ns). Camera hook writes.
inline std::atomic<s64> g_fade_until_ns{0};
/// The game's yaw about up (radians, the camera hook's psi) and when it was last written, for the
/// closed-loop snap turn on the XR thread.
inline std::atomic<float> g_game_yaw{0.0f};
inline std::atomic<s64> g_game_yaw_ns{0};
/// The right hand's aim pose relative to the head (OpenXR view space), sampled by the
/// XR thread with the head pose.
struct HandPose {
    std::array<float, 3> position{};
    std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
    s64 sample_ns{};
    bool valid{};
};
inline SeqLocked<HandPose> g_right_hand;
inline SeqLocked<HandPose> g_left_hand; // The left hand, the same way
/// The left stick in the game body's frame = m x the stick, so forward walks where the head (or the left controller)
/// faces. Written by the hook runtime each frame from the body and view yaw; identity when the body yaw is unknown.
struct StickXf {
    std::array<float, 4> m{1.0f, 0.0f, 0.0f, 1.0f}; // row-major 2x2: (x', y') = (m0 x + m1 y, m2 x + m3 y)
    s64 sample_ns{};
};
inline SeqLocked<StickXf> g_move_stick;
/// The sticks the presenter last sent to the game (lx, ly, rx, ry), for the scan-release log.
struct SentPad {
    std::array<float, 4> sticks{};
};
inline SeqLocked<SentPad> g_sent_pad;
/// An instant snap turn the XR thread asks for (degrees, + = right); the camera hook applies
/// it to the player's body yaw and clears it. g_snap_instant: the profile has [snap] (else the closed loop).
inline std::atomic<float> g_snap_request_deg{0.0f};
inline std::atomic<bool> g_snap_instant{false};
/// The torso is on hold this frame ([snap] torso_skip_fields: ball, cinematics, grapple, frozen, a scan or combat lock):
/// the presenter's sustained-head rule does not count, so a long look at a lock target never turns the body.
inline std::atomic<bool> g_torso_hold{false};
/// The game's own visor dial previews this slot while the hand is at the head (-1 = the game
/// draws it as usual). XR thread writes, the [visor_dial] hook reads.
inline std::atomic<int> g_visor_preview{-1};

/// Set by the hook runtime when the profile has a layout; read by the XR thread.
inline SeqLocked<InputLayout> g_input_layout;

/// The Pro Controller state the XR thread produces each headset frame.
struct ProPad {
    u32 buttons{}; // bit n = ProTarget n
    float lx{}, ly{}, rx{}, ry{};
    bool operator==(const ProPad&) const = default;
};

/// Registered by the frontend (it owns the InputSubsystem); called on the XR thread when the
/// pad changes. Null = no sink (e.g. yuzu_cmd): Touch input is ignored.
inline std::atomic<void (*)(const ProPad&)> g_gamepad_sink{nullptr};

/// nvnflinger, when the game queues a buffer: the pose id to stamp on it (0 = no hook camera).
/// The raw newest id; the consumer applies the profile's pose_lag_frames.
inline u64 CurrentFrameTag() {
    if (!g_hook_state.camera_active.load(std::memory_order_acquire)) {
        return 0;
    }
    return g_pose_ledger.LatestId();
}

/// Renderer, once per published game frame: counts frames since the last hook hit and drops
/// the camera to passthrough (logged once) after hook_silence_frames.
void NoteGameFramePublished();

/// Hook, on every hit: resets the silence count.
void NoteHookHit();

/// The camera hook has not fired for more than hook_silence_frames published frames (pause, map,
/// or no profile camera this frame). False without an installed profile.
bool HookSilent();

/// Clears the per-launch hook state (new game launch, shutdown). Pose ids keep counting up.
void ResetHookState();

} // namespace VIZR

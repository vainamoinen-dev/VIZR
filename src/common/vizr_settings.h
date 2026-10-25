// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// VIZR MM: every user setting is defined ONCE here. The mod menu's
// sliders, the user.toml lines, the parser and the migration (missing keys keep their defaults) all come
// from this table; there is no second list to keep in step.

#pragma once

#include <array>
#include <cstddef>
#include <string>

#include "common/vizr.h"

namespace VIZR::Settings {

enum class Scope {
    Element, ///< once per HUD element of the game (address file [hud_widgets] elements): [hud.<name>]
    Whole,   ///< the whole HUD: [hud_all]
    World,   ///< [world]
    Comfort, ///< [comfort]
    State,   /// < [state]: remembered choices, not in the menu (the selected visor)
    Compat,  /// < [compat]: the Compatibility tab
};

/// The menu tab a setting shows on. Auto = from its scope (Element/Whole: HUD, World: World,
/// Comfort: Comfort, State: not in the menu); the user.toml section stays the scope's either way.
enum class Tab { Auto, Hud, Comfort, Body, CannonArms, World, Compat };

struct Spec {
    Scope scope;
    const char* key;   ///< user.toml key inside its section
    const char* label; ///< menu label
    float min;
    float max;
    float step;        ///< one arrow click / the slider's display precision
    float def;
    float* (*field)(UserSettings& settings, std::size_t element);
    Tab tab{Tab::Auto};
    /// A choice instead of a slider: the values and their labels (choice_count of each).
    const float* choice_values{nullptr};
    const char* const* choice_labels{nullptr};
    std::size_t choice_count{0};
    /// Takes effect at the next launch (the menu marks it and shows "Restart VIZR to apply").
    bool next_launch{false};
};

/// The tab a setting shows on (Hud, Comfort, Body, CannonArms, World), or Auto for State (not shown).
constexpr Tab MenuTab(const Spec& spec) {
    if (spec.tab != Tab::Auto) {
        return spec.tab;
    }
    switch (spec.scope) {
    case Scope::Element:
    case Scope::Whole:
        return Tab::Hud;
    case Scope::World:
        return Tab::World;
    case Scope::Comfort:
        return Tab::Comfort;
    case Scope::State:
        return Tab::Auto;
    case Scope::Compat:
        return Tab::Compat;
    }
    return Tab::Auto;
}

// The Compatibility tab's choices (value, label).
inline constexpr std::array<float, 5> RenderScaleValues{0.0f, 1.0f, 1.5f, 2.0f, 3.0f};
inline constexpr std::array<const char*, 5> RenderScaleLabels{"Automatic", "1x", "1.5x", "2x", "3x"};
inline constexpr std::array<float, 5> RefreshValues{0.0f, 72.0f, 80.0f, 90.0f, 120.0f};
inline constexpr std::array<float, 4> GameFpsValues{60.0f, 0.0f, 90.0f, 120.0f};
inline constexpr std::array<const char*, 4> GameFpsLabels{"60 (default)", "Match headset (up to 90)", "90",
                                                           "120 (needs a GPU that never dips; runs slow otherwise)"};
inline constexpr std::array<const char*, 5> RefreshLabels{"Automatic", "72 Hz", "80 Hz", "90 Hz", "120 Hz"};
inline constexpr std::array<float, 3> ShaderValues{0.0f, 1.0f, 2.0f};
inline constexpr std::array<const char*, 3> ShaderLabels{"Automatic", "Smooth (async; textures may pop in)",
                                                         "Exact (stalls on new shaders)"};
inline constexpr std::array<float, 3> AccuracyValues{0.0f, 1.0f, 2.0f};
inline constexpr std::array<const char*, 3> AccuracyLabels{"Automatic", "Normal", "High"};
inline constexpr std::array<float, 2> HoldValues{1.0f, 0.0f};
// What the left stick's forward follows.
inline constexpr std::array<float, 5> MoveForwardValues{2.0f, 0.0f, 1.0f, 3.0f, 4.0f};
inline constexpr std::array<const char*, 5> MoveForwardLabels{"Body (default)", "Head", "Left hand", "Right hand",
                                                              "Body follows the head (walk: left hand)"};
// Which grapple points the left hand picks from.
inline constexpr std::array<float, 2> GrapplePickValues{0.0f, 1.0f};
inline constexpr std::array<const char*, 2> GrapplePickLabels{"In view (default)", "Anywhere the hand points"};
inline constexpr std::array<const char*, 2> HoldLabels{"On", "Off (debug: allow the flat frame)"};

inline constexpr std::array<Spec, 51> Table{{
    {Scope::Element, "offset_x", "Left / right", -1.0f, 1.0f, 0.01f, 0.0f,
     [](UserSettings& s, std::size_t e) { return &s.hud.elements[e].offset[0]; }},
    {Scope::Element, "offset_y", "Down / up", -1.0f, 1.0f, 0.01f, 0.0f,
     [](UserSettings& s, std::size_t e) { return &s.hud.elements[e].offset[1]; }},
    {Scope::Element, "scale", "Size", 0.3f, 2.0f, 0.05f, 1.0f,
     [](UserSettings& s, std::size_t e) { return &s.hud.elements[e].scale; }},
    {Scope::Element, "alpha", "Opacity", 0.0f, 1.0f, 0.05f, 1.0f,
     [](UserSettings& s, std::size_t e) { return &s.hud.elements[e].alpha; }},
    {Scope::Element, "depth", "Farther / nearer", -5.0f, 5.0f, 0.1f, 0.0f,
     [](UserSettings& s, std::size_t e) { return &s.hud.elements[e].depth; }},
    {Scope::Whole, "scale", "Whole HUD size", 0.3f, 1.5f, 0.05f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.whole.scale; }},
    {Scope::Whole, "depth", "Whole HUD farther / nearer", -5.0f, 5.0f, 0.1f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.whole.depth; }},
    {Scope::Whole, "height", "Whole HUD down / up", -0.5f, 0.5f, 0.01f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.whole.offset[1]; }},
    {Scope::Whole, "true_helmet", "True-helmet HUD size (off: the game's HUD size)", 0.0f, 1.0f, 1.0f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.visor_true_helmet; }},
    {Scope::World, "scale", "World scale", 0.5f, 2.0f, 0.01f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.world_scale; }},
    {Scope::World, "eye_height_m", "Eye height (m)", -0.5f, 0.5f, 0.01f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.eye_height_m; }},
    {Scope::Comfort, "snap_turn", "Snap turn (off: smooth turn)", 0.0f, 1.0f, 1.0f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.snap_turn; }},
    {Scope::Comfort, "snap_deg", "Snap angle (deg)", 15.0f, 90.0f, 5.0f, 30.0f,
     [](UserSettings& s, std::size_t) { return &s.snap_deg; }},
    {Scope::Comfort, "snap_fire", "Snap: stick push that turns", 0.5f, 0.95f, 0.05f, 0.7f,
     [](UserSettings& s, std::size_t) { return &s.snap_fire; }},
    {Scope::Comfort, "snap_rearm", "Snap: stick released below", 0.1f, 0.5f, 0.05f, 0.3f,
     [](UserSettings& s, std::size_t) { return &s.snap_rearm; }},
    {Scope::Comfort, "snap_rearm_ms", "Snap: released this long before the next (ms)", 0.0f, 300.0f, 10.0f, 120.0f,
     [](UserSettings& s, std::size_t) { return &s.snap_rearm_ms; }},
    {Scope::Comfort, "snap_reverse_ms", "Snap: no opposite turn within (ms)", 0.0f, 600.0f, 50.0f, 300.0f,
     [](UserSettings& s, std::size_t) { return &s.snap_reverse_ms; }},
    {Scope::Comfort, "crouch_gesture", "Crouch gesture (the game's crouch action)", 0.0f, 1.0f, 1.0f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.crouch_morph; }},
    {Scope::Comfort, "crouch_drop_m", "Crouch depth (m below standing)", 0.2f, 0.8f, 0.05f, 0.45f,
     [](UserSettings& s, std::size_t) { return &s.crouch_drop_m; }},
    {Scope::Comfort, "hand_forward_m", "Weapon hand forward / back (m)", -0.3f, 0.3f, 0.01f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.cannon_forward_m; }, Tab::CannonArms},
    {Scope::Comfort, "hand_up_m", "Weapon hand up / down (m)", -0.2f, 0.2f, 0.01f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.cannon_up_m; }, Tab::CannonArms},
    {Scope::Comfort, "move_forward", "Move forward follows", 0.0f, 4.0f, 1.0f, 2.0f,
     [](UserSettings& s, std::size_t) { return &s.move_forward; }, Tab::Auto, MoveForwardValues.data(),
     MoveForwardLabels.data(), MoveForwardValues.size(), false},
    {Scope::Comfort, "grapple_pick", "Grapple pick", 0.0f, 1.0f, 1.0f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.grapple_pick; }, Tab::Auto, GrapplePickValues.data(),
     GrapplePickLabels.data(), GrapplePickValues.size(), false},
    {Scope::Comfort, "body_shoulder_weight", "Body: shoulder weight (toward the hands)", 0.0f, 1.0f, 0.05f, 0.7f,
     [](UserSettings& s, std::size_t) { return &s.body_shoulder_weight; }, Tab::Body},
    {Scope::Comfort, "body_smoothing_s", "Body: smoothing (s)", 0.05f, 1.0f, 0.05f, 0.25f,
     [](UserSettings& s, std::size_t) { return &s.body_smoothing_s; }, Tab::Body},
    {Scope::Comfort, "move_hand_offset_deg", "Controller forward: turn toward the body centre (deg)", 0.0f, 45.0f, 1.0f,
     20.0f, [](UserSettings& s, std::size_t) { return &s.move_hand_offset_deg; }},
    {Scope::Comfort, "arm_length_m", "Arm length, shoulder to wrist (m)", 0.45f, 0.85f, 0.01f, 0.63f,
     [](UserSettings& s, std::size_t) { return &s.arm_length_m; }},
    {Scope::Comfort, "left_hand_forward_m", "Left hand forward / back (m)", -0.15f, 0.15f, 0.01f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.left_hand_forward_m; }},
    {Scope::Comfort, "left_hand_up_m", "Left hand up / down (m)", -0.15f, 0.15f, 0.01f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.left_hand_up_m; }},
    {Scope::Comfort, "hand_scale", "Weapon size", 0.5f, 1.5f, 0.05f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.cannon_scale; }, Tab::CannonArms},
    {Scope::Comfort, "arm_follow", "Arm follows your body (off: the game's arm)", 0.0f, 1.0f, 1.0f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.arm_follow; }, Tab::CannonArms},
    {Scope::Comfort, "comfort_fade", "Fade on form change", 0.0f, 1.0f, 1.0f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.comfort_fade; }},
    {Scope::Comfort, "ball_view", "Alternate form in 3D (off: a 2D screen)", 0.0f, 1.0f, 1.0f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.ball_view; }},
    {Scope::Comfort, "ball_vignette", "Alternate-form vignette (tunnel while moving; 0 = off)", 0.0f, 1.0f, 0.1f, 0.5f,
     [](UserSettings& s, std::size_t) { return &s.ball_vignette; }},
    {Scope::Comfort, "cutscene_view", "Cutscenes in 3D (off: a 2D screen)", 0.0f, 1.0f, 1.0f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.cutscene_view; }},
    {Scope::Comfort, "cutscene_vignette", "Cutscene vignette (tunnel on camera moves; 0 = off)", 0.0f, 1.0f, 0.1f, 0.5f,
     [](UserSettings& s, std::size_t) { return &s.cutscene_vignette; }},
    {Scope::Comfort, "show_body", "Show your body", 0.0f, 1.0f, 1.0f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.show_body; }, Tab::Body},
    {Scope::Comfort, "body_height", "Body size", 0.8f, 1.2f, 0.01f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.body_height; }, Tab::Body},
    {Scope::Comfort, "body_follow", "Body stands and leans with your head (off: the game's stance)", 0.0f, 1.0f, 1.0f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.body_follow; }, Tab::Body},
    {Scope::Comfort, "head_position", "Head position moves the view", 0.0f, 1.0f, 1.0f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.head_position; }},
    {Scope::Comfort, "lean_limit_m", "Lean limit (m)", 0.1f, 0.8f, 0.05f, 0.5f,
     [](UserSettings& s, std::size_t) { return &s.lean_limit_m; }},
    {Scope::Comfort, "shoulder_right_m", "Shoulder right / left (m)", -0.2f, 0.2f, 0.01f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.shoulder_right_m; }, Tab::Body},
    {Scope::Comfort, "shoulder_down_m", "Shoulder down / up (m)", -0.2f, 0.2f, 0.01f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.shoulder_down_m; }, Tab::Body},
    {Scope::Comfort, "shoulder_back_m", "Shoulder back / forward (m)", -0.2f, 0.2f, 0.01f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.shoulder_back_m; }, Tab::Body},
    {Scope::Compat, "render_scale", "Render scale", 0.0f, 3.0f, 0.5f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.compat_render_scale; }, Tab::Auto, RenderScaleValues.data(),
     RenderScaleLabels.data(), RenderScaleValues.size(), true},
    {Scope::Compat, "refresh_hz", "Headset refresh", 0.0f, 120.0f, 1.0f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.compat_refresh_hz; }, Tab::Auto, RefreshValues.data(),
     RefreshLabels.data(), RefreshValues.size(), false},
    {Scope::Compat, "game_fps", "Game frame rate", 0.0f, 120.0f, 1.0f, 60.0f,
     [](UserSettings& s, std::size_t) { return &s.compat_game_fps; }, Tab::Auto, GameFpsValues.data(),
     GameFpsLabels.data(), GameFpsValues.size(), false},
    {Scope::Compat, "shaders", "Shader compile", 0.0f, 2.0f, 1.0f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.compat_shaders; }, Tab::Auto, ShaderValues.data(),
     ShaderLabels.data(), ShaderValues.size(), true},
    {Scope::Compat, "gpu_accuracy", "GPU accuracy", 0.0f, 2.0f, 1.0f, 0.0f,
     [](UserSettings& s, std::size_t) { return &s.compat_gpu_accuracy; }, Tab::Auto, AccuracyValues.data(),
     AccuracyLabels.data(), AccuracyValues.size(), true},
    {Scope::Compat, "frame_hold", "Hold last 3D frame on stutter", 0.0f, 1.0f, 1.0f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.compat_frame_hold; }, Tab::Auto, HoldValues.data(),
     HoldLabels.data(), HoldValues.size(), false},
    {Scope::State, "selected_visor", "Selected ring choice", 0.0f, 3.0f, 1.0f, 1.0f,
     [](UserSettings& s, std::size_t) { return &s.selected_visor; }},
}};

/// The user.toml section of a setting: "hud.<element name>", "hud_all" or "world".
inline std::string Section(const Spec& spec, const UserSettings& settings, std::size_t element) {
    switch (spec.scope) {
    case Scope::Element:
        return std::string{"hud."} + settings.hud.names[element].data();
    case Scope::Whole:
        return "hud_all";
    case Scope::World:
        return "world";
    case Scope::Comfort:
        return "comfort";
    case Scope::State:
        return "state";
    case Scope::Compat:
        return "compat";
    }
    return {};
}

} // namespace VIZR::Settings

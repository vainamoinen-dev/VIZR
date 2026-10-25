// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Game profile loader (format v0.1): game.toml + <build-id>.toml.

#pragma once

#include <array>
#include <map>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/vizr.h"

namespace VIZR::Profile {

/// Addr: an address only (e.g. a list inline in its parent): its address is used, a value read is an error.
enum class FieldType { U8, U16, U32, U64, Ptr, F32, Vec3f, Quat, Mat3x4, Mat4, Addr };

/// Where a field's address chain starts.
enum class RootKind { Reg, Sp, Static, Sym };

struct Field {
    std::string name;
    std::string hook; // empty = readable anywhere (static/sym root)
    RootKind root{};
    u32 reg{};               // RootKind::Reg: x0..x30
    std::string module;      // RootKind::Static
    u64 module_offset{};     // RootKind::Static
    std::string sym;         // RootKind::Sym (mangled)
    std::vector<s64> chain;  // each step: add the offset, then dereference (the address-file chain rule)
    s64 off{};
    FieldType type{};
};

struct Hook {
    std::string name;
    std::string module;
    std::string sym; // mangled; empty = offset only
    u64 sym_off{};
    std::optional<u64> offset;
    s64 expect_from{};
    std::vector<u8> expect;
    bool enabled{true};
};

/// A `when` expression; `op` picks which members are used.
struct When {
    /// State (format v0.2): `{ state = "<name>" }`, a named address-file [states] entry holds (`field` = the name).
    enum class Op { Always, Equals, In, Mask, EqualsAddr, EqualsField, IsNull, Not, AllOf, AnyOf, State };
    Op op{Op::Always};
    std::string field;
    u64 value{};
    std::vector<u64> values;
    u64 mask{};
    bool set{};
    std::string addr_module;
    u64 addr_offset{};
    std::string other_field;
    std::vector<When> children;
    u32 state_index{}; // Op::State: the state's bit, resolved when the profile loads
};

struct Camera {
    std::string id;
    std::string mode; // passthrough | first_person | third_person_orbit | diorama
    When when;
    std::string combine;
    std::string source;
    std::string target;
    /// [space] rotation_sense = "look_at": source and target are { pos, at, up }
    /// vec3f triples. `source` / `target` above then name the pos fields (the target's hook is where the camera
    /// runs); these name the at and up fields.
    bool look_at{};
    std::string source_at, source_up, target_at, target_up;
    u32 hook_silence_frames{3};
    u32 pose_lag_frames{0};
    /// Optional pose_lag_frames_at = { <tick rate> = frames }: the pipeline depth at another game tick rate ([tick_rate]);
    /// a rate it does not name uses pose_lag_frames.
    std::map<u32, u32> pose_lag_frames_at;
    // M4
    std::string fov_target;       // field name; empty = the game's FOV is never written
    std::string fov_kind{"rad_vertical"};
    std::string fov_mode{"headset"}; // headset | game
    float fov_max_deg{110.0f};
    float height_offset_m{0.0f};
    /// game_yaw_plus_head: keep the game's own pitch up to this many degrees (0 = drop it, the first-person rule).
    /// The ball camera looks down at the ball from about 1.85 m; level, the ball is out of view. (MORPH-BALL.md)
    float game_pitch_cap_deg{0.0f};
    /// game_yaw_plus_head: the game camera matrix column that is its look direction, for yaw and pitch (-1 = the
    /// [space] basis forward, the first-person rule); Prime's ball camera looks along its column 2.
    int game_look_column{-1};
    /// game_yaw_plus_head: field whose matrix gives the game yaw (e.g. the player body), so camera
    /// bob/wander never reaches the head. Empty = "player_yaw" if that field exists, else the camera. "camera" = this
    /// camera's own yaw (a cutscene shot looks where the shot looks, not where the player character faces).
    std::string yaw_source;
    std::string yaw_source_forward; // body-local forward axis ("+z"...); empty = [space] forward_axis
    /// Black for this many ms when this camera starts or ends, when cut_field changes
    /// while it holds (Prime: the current camera object = a new cutscene shot), or when the game camera jumps more
    /// than cut_jump_units / cut_jump_deg in one tick (0 = that test off). 0 ms = no cut fades.
    u32 cut_fade_ms{0};
    std::string cut_field;
    float cut_jump_units{0.0f};
    float cut_jump_deg{0.0f};
    /// Comfort settings this camera obeys, by their user.toml [comfort] key (empty = none).
    /// view_setting < 0.5 = the camera shows the flat screen (as passthrough); vignette_setting = the tunnel's
    /// strength while the game camera moves.
    std::string view_setting;
    std::string vignette_setting;
};

/// [space]. The camera-local basis maps OpenXR view axes to game camera axes.
struct Space {
    char up_axis{'y'};
    bool right_handed{true};
    float units_per_meter{1.0f};
    bool row_major{true};
    bool camera_to_world{true};
    /// Rotation_sense = "look_at" (eye, target, up vec3f). The core builds a camera-to-world 3x4 from them
    /// (columns right, up, back = the camera-local x, y, z), so camera_to_world is true and forward_axis must be -z.
    bool look_at{};
    std::string forward_axis{"-z"};
    /// Columns = game camera-local vectors for OpenXR right (+X), up (+Y), back (+Z).
    std::array<std::array<float, 3>, 3> local_basis{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
};

/// Address file [grapple_aim]: the left hand picks the grapple point. At `hook` (the
/// game's orbit selection, after it stored its own camera candidate) the candidate id at player+candidate_off is
/// replaced by the grapple point (entity vtable) from the player's in-range list nearest the left hand's ray, or by
/// the game's invalid id when none is within max_angle_deg and the range.
struct GrappleAim {
    bool candidate_writes{true}; // false: the game picks its own candidates (the logs still run)
    bool present{};
    /// Optional [grapple_aim.zl_log]: at
    /// hook, twice a second while the game's own candidate (x<candidate_reg>, after our write) is a grapple point: that
    /// candidate, what +candidate_off holds, the hand's angle to the point, u32 player offsets, and scalar fields.
    /// Optional [[grapple_aim.zl_trace]]: log points in the game's lock-on input path. Each logs its register (and the
    /// player's u32 offsets) when the register's value changes, and every call for 0.5 s after any non-zero value
    /// (at most 30 lines per window), so one press reads as held / pressed / locked lines.
    struct ZlTrace {
        std::string hook;
        std::string label;
        u32 reg{};
        u32 player_reg{};
        std::vector<u64> player_offs;
    };
    std::vector<ZlTrace> zl_trace;
    /// Optional [grapple_aim.press_log]: at each ZL press edge (the zl trace), every session: the candidate lists'
    /// counts (`lists`: u32 count at the offset, data pointer at +list_data_off), the nearest listed target (id, a grapple
    /// point or another object, distance, angle to the head's forward and to the left hand's ray), the orbit-disable
    /// count (u32 at disable_off) and the visibility read list (at *(PL + read_list_off): u32 count at read_count_off,
    /// entries of read_stride bytes at the pointer at read_data_off, each a u32 id at read_id_off and flags at
    /// read_flags_off, visible when flags & 3 == 3).
    struct PressLog {
        bool present{};
        std::vector<u64> lists;
        u64 disable_off{};
        u64 read_list_off{}, read_count_off{}, read_data_off{}, read_stride{}, read_id_off{}, read_flags_off{};
    } press_log;
    struct ZlLog {
        bool present{};
        std::string hook;
        u32 candidate_reg{};
        std::vector<u64> player_offs;
        std::vector<std::string> fields;
    } zl_log;
    /// Where the game has just chosen its candidate (Prime: the idle path every tick and the held-ZL
    /// Path), each with the registers holding the player and the game-logic object there.
    struct Site {
        std::string hook;
        u32 player_reg{};
        u32 gl_reg{};
    };
    std::vector<Site> sites;
    /// Optional [grapple_aim.points]: the game's own list of grapple points, for the "anywhere the hand points" pick
    /// (user setting grapple_pick 1). The in-range orbit list (list_off) holds only what the game's camera box sees;
    /// this one holds every grapple point loaded, so a point off to the side or behind can be picked too.
    /// list = *(u64)(table_chain's result + list_ptr_off) (before table_add); L = list + list_off: i32 bucket count,
    /// u64 bucket array at L + list_data_off. Bucket k's head = *(array + k * bucket_stride); its first GOC =
    /// *(head + head_goc_off), the next = *(goc + goc_next_off); entity = *(goc + goc_entity_off). An entity counts
    /// when its vtable is [grapple_aim] vtable, (*(u8)(entity + active_off) & active_mask) != 0 and its u32 area
    /// (entity + area_off) is the player's (player + player_area_off); its id (u32 at entity + id_off) must resolve
    /// through the entity table back to the same entity before it is written as the candidate.
    struct Points {
        bool present{};
        u64 list_ptr_off{};
        u64 list_off{};
        u64 list_data_off{};
        u64 bucket_stride{};
        u64 head_goc_off{};
        u64 goc_next_off{};
        u64 goc_entity_off{};
        u64 id_off{};
        u64 area_off{};
        u64 active_off{};
        u64 active_mask{};
        u64 player_area_off{};
        float min_range{}; // nearer than this (game units) is never a pick
    } points;
    u64 candidate_off{};
    u64 list_off{};          // u32 count at +0, data pointer at list_data_off
    u64 list_data_off{};
    std::vector<u64> table_chain; // from x<gl_reg>: each step reads a u64 at +step
    u64 table_add{};              // then + table_add: the entity table itself (inline in its parent, not a pointer)
    u64 deleted_off{};            // An entity with (*(u8)(entity + deleted_off) & deleted_mask) is skipped
    u64 deleted_mask{};
    u64 id_index_mask{};          // table slot = id & id_index_mask
    u64 stride{}, id_check_off{}, flags_off{}, flags_mask{}, flags_value{}, entity_off{};
    std::string vtable_module;
    u64 vtable_offset{};
    std::vector<u64> point_chain; // from the entity: u64 reads, then a row-major 3x4 (translation = column 3)
    float max_angle_deg{};
    /// The left aim ray turned about the world's up for the pick (degrees, positive = left; 0 = the hand's own).
    float left_aim_yaw_offset_deg{};
    /// In the Scan Visor ([aim] scan_state) the HEAD picks: any listed object whose
    /// byte at scan_flag_off (from the entity) has scan_flag_mask set (the game's own scannable test, ValidateScanning)
    /// within scan_max_angle_deg of the head ray, else the invalid id (nothing locks unless you look at it).
    /// scan_max_angle_deg 0 = off (the game's camera pick in the Scan Visor).
    float scan_max_angle_deg{0.0f};
    /// Holder = *(actor + scan_holder_off); scannable iff holder != 0 (CActorMP1:GetScannableObjectInfo)
    /// and (*(u64)(holder + scan_info_off) != 0 or *(u8)(holder + scan_info_flag_off) != 0).
    u64 scan_holder_off{};
    u64 scan_info_off{};
    u64 scan_info_flag_off{};
    std::vector<u64> scan_lists;   // player offsets of the orbit lists the head pick walks (u32 count, data at +list_data_off)
    /// Test log only (optional): the player's orbit state, locked target, scan state and scan time (u32, u32, u32, f32);
    /// empty = no "scan state" line.
    std::vector<u64> scan_state_offs;
    /// Optional u8 player fields
    /// (Prime: free look PL+0x5bc / +0x5bd) logged ten times a second for 2 s after every lock release (orbit 1 -> 0),
    /// with the sticks we sent and the game camera / body yaw. Needs scan_state_offs.
    std::vector<u64> release_log_offs;
    std::string range_field;   // f32 field: the lock range (game units)
    std::string invalid_field; // u32 field: the game's invalid id
};

/// Address file [hud_widgets]: the game's GUI widget layout (Prime: the HUD's widget tree).
struct HudWidgets {
    bool present{};
    std::string enter_hook;           // [hooks] entry before the HUD's draw copies the widgets
    std::string exit_hook;            // [hooks] entry after it (restore)
    u64 world_xf_offset{};            // 3x4 row-major world transform
    u64 first_child_offset{};
    u64 next_sibling_offset{};
    u64 alpha_offset{};               // f32
    /// A widget counts as drawn this frame (for the HUD crop check) only if every
    /// drawn_flags [offset, mask] byte test passes and its alpha (alpha_offset) is above 1e-5 unless the
    /// alpha_bypass [offset, mask] byte test passes. Empty = every widget counts.
    std::vector<std::pair<u64, u8>> drawn_flags;
    std::optional<std::pair<u64, u8>> alpha_bypass;
    float units_per_screen_height{};  // offset conversion at the HUD's depth
    /// Optional: the HUD camera (vertical FOV deg, distance to the HUD plane, aspect) for the test log of each element's on-screen extent
    /// log; without it the extent is not logged.
    std::optional<std::array<float, 3>> extent_camera;
    std::string state_field;          // u32 HUD state field, may be empty
    struct Element {
        std::string name;                 // game.toml [hud] key and menu label
        std::string root_field;           // ptr field (on enter_hook) holding the element's root widget
        std::optional<u32> only_in_state; // applies only while state_field reads this value
        /// In these HUD states the
        /// element's root is another field (Prime: the Scan Visor's own deco frame replaces the combat one), so one
        /// slider places both. Checked before only_in_state.
        std::vector<std::pair<u32, std::string>> root_field_in_state;
        /// The profile's own placement under the player's (offset in screen heights, added; scale,
        /// multiplied), e.g. to pull a corner element inside every eye's view (Prime: radar, minimap).
        std::array<float, 2> base_offset{};
        float base_scale{1.0f};
        /// Kind "corners" = not a widget subtree but a window given by corner points
        /// (Prime's minimap): at corners_enter_hook (x<corners_reg> = the object) the points (count u32 at
        /// count_off, data pointer at data_off, used only if the byte at engaged_off is set; stride bytes apart,
        /// x, y, z floats) are moved and scaled like a widget, and put back at corners_exit_hook.
        std::string kind{"widget"};
        std::string corners_enter_hook;
        std::string corners_exit_hook;
        u32 corners_reg{};
        u64 count_off{};
        u64 data_off{};
        u64 engaged_off{};
        u64 stride{12};
    };
    std::vector<Element> elements;        // at most VIZR::HudMaxElements
    std::vector<std::string> whole_roots; // ptr fields: the frame roots the whole-HUD settings move
};

/// Address file [hands]: where the game keeps the right hand's object (Prime: the arm cannon).
struct Hands {
    bool present{};
    std::string hook;                 // [hooks] entry after the game's last write to the targets, each frame
    std::vector<std::string> write;   // mat3x4 fields on that hook: written with the hand's pose
    std::array<float, 3> offset_m{};  // object origin relative to the aim pose (OpenXR hand axes: right, up, back)
    std::string fire_hook;            // optional [hooks] entry at the shot (log only)
    std::string fire_field;           // mat3x4 field on fire_hook: the object's pose at the shot
    float fire_origin_forward{};      // move that pose this far along its column 2 (Prime: undo the 3-unit set-back)
};

/// Address file [aim]: the game's aiming cursor casts its ray from the hand, and the aiming
/// reticle moves to the hit point.
struct Aim {
    bool present{};
    std::string ray_hook;                   // where the cast's camera transform can be replaced
    u32 cursor_reg{};                       // register holding the cursor object at ray_hook
    u32 ray_xform_reg{};                    // register + offset of that 3x4 camera transform
    s64 ray_xform_off{};
    std::array<std::string, 3> ray_columns{"right", "forward", "up"}; // the cast's column convention
    u64 cursor_on_plane_off{};              // cursor: vec3, the ray point at the reticle's plane distance
    u64 cursor_hit_off{};                   // cursor: vec3, the hit point
    u64 cursor_distance_off{};              // cursor: f32, the hit distance (0 = not logged)
    /// While scan_state holds the ray keeps the
    /// game's camera (= the head) when scan_source = "head" (default); "hand" casts from the hand as in combat.
    bool scan_from_head{true};
    std::string scan_state;
    std::string reticle_hook;               // every reticle's world 3x4 passes here
    u32 reticle_xform_reg{};
    std::string reticle_skip_lr_module;     // a caller whose transforms are left alone
    u64 reticle_skip_lr_offset{};
    std::string reticle_lr_module;          // the aiming reticle's callers (return addresses); empty = by position
    std::vector<u64> reticle_lr_offsets;
    /// Reticles the game places relative to its own camera (Prime: the scan visor's circle
    /// and brackets), re-placed relative to the head: xf' = head * game_camera^-1 * xf, at the reticle hook.
    std::vector<u64> head_reticle_lr_offsets;
    /// Optional head_reticle_target = { reg, off, margin }: while a lock holds ([orbit_head] lock_field), the head
    /// reticles' target point is the vec3 at x<reg> + off; they move out to its depth (less margin, never nearer than
    /// their own plane), scaled about the eye so their size on screen and their layout stay.
    s32 head_reticle_target_reg{-1};
    u64 head_reticle_target_off{};
    float head_reticle_target_margin{};
    /// Optional rotate_only_lrs: reticles already at their object's depth (Prime: the grapple icons, the scan
    /// indicators) that face the game camera: their rotation is re-made relative to the head, the position kept.
    std::string rotate_only_lr_module;
    std::vector<u64> rotate_only_lr_offsets;
};

/// Address file [snap]: instant snap turn by rotating the body yaw.
struct Snap {
    bool present{};
    std::string yaw_field;                  // mat3x4: the body transform (rotate its 3x3 about world up)
    std::string push_field;                 // u8: set to 1 so the next physics step takes the new heading
    std::vector<std::string> skip_fields;   // u32: a nonzero value means no snap (orbit, grapple, morph)
    /// A nonzero value means the body does not follow the torso this tick (morph ball,
    /// cinematics, grapple, frozen); the view keeps the body's last torso turn, so nothing jumps.
    std::vector<std::string> torso_skip_fields;
    float sign{1.0f};                       // + = right for a right push
};

/// Address file [arms]. Diagnostic stage: at `hook` (inside the animation
/// pose build, x<char_reg> = the character), the characters named by `char_fields` (u64 fields read at the camera
/// hook) get their pose nodes logged twice (ids, parent-local rotation, scale, translation). Node data:
/// pose = char + u16 at char+pose_off_at; coords table = u64 at pose+coords_table_at; node set = the u64 chain
/// node_set_chain from char; count = u16 at set+count_at; ids = u32[count] at set+ids_at; an id's coords are at
/// *(table + bank*8) + offset*8 + 8 (bank = id bits 0-4, offset = bits 5-15). Optional left-arm force each camera
/// hook: left_flags_field |= flags_set, &= ~flags_clear; left_xf_field = the left hand's pose.
struct Arms {
    bool present{};
    std::string hook;
    u32 char_reg{};
    std::vector<std::string> char_fields;
    u64 pose_off_at{};
    u64 coords_table_at{};
    std::vector<u64> node_set_chain;
    u64 count_at{};
    u64 ids_at{};
    std::string left_flags_field;
    u32 flags_set{};
    u32 flags_clear{};
    std::string left_xf_field;
    std::array<float, 3> left_offset_m{};
    /// The hand axes written into the left arm transform's columns 0-2: "left", "right", "up", "down", "forward",
    /// "back".
    std::array<std::string, 3> left_columns{"left", "up", "forward"};
    /// The left arm is already on in idle (the glove on the cannon); its render transform is
    /// rebuilt every tick, so it is written at `left_render_hook` (the arm's render entry, x<left_arm_reg> = the arm)
    /// at arm + left_xf_off: columns = the left controller's left/up/forward, translation such that `left_wrist`
    /// (the wrist in the arm's model space, x left, y up, z forward, world units) lands on the controller.
    std::string left_render_hook;
    u32 left_arm_reg{};
    u64 left_xf_off{};
    std::array<float, 3> left_wrist{};
    /// left_ik = the left arm as two bones. At left_render_hook its transform is body-fixed instead: rotation = the
    /// body's yaw frame (columns right, up, back), translation so that left_shoulder_model (the shoulder in the arm
    /// model's space, before the transform's column scale) lands on the body's left shoulder (shoulder_offset_m
    /// mirrored). At ik_hook, for its character (**(arm + left_char_off)), left_shoulder_node and left_elbow_node turn
    /// so that left_wrist_node reaches the left controller: a two-bone solve whose bend plane goes through the
    /// animation's own elbow. Needs ik (the right arm's ik_hook and pose registers).
    bool left_ik{};
    u64 left_char_off{};                 // All left_ik keys are required (the profile's; no game default in core)
    u32 left_shoulder_node{};
    u32 left_elbow_node{};
    u32 left_wrist_node{};
    std::array<float, 3> left_shoulder_model{};
    /// The left arm's pose space = its transform's frame with these axis signs (used both to place the shoulder and to
    /// bring the controller into pose space; the right arm's pose_signs are its own).
    std::array<float, 3> left_pose_signs{1.0f, 1.0f, 1.0f};
    /// The wrist joint -> the palm's
    /// grip point, metres along the controller's right, up, back. The solve puts the palm point on the controller.
    std::array<float, 3> left_grip_m{};
    /// Where the pose keeps the active anchor's id (u32) and the abs banks pointer (u64): the
    /// shoulder's parent frame P is that anchor's abs. Required with left_ik.
    /// The hand's turn from
    /// the controller, degrees: pitch (about the controller's right), yaw (up), roll (back), applied in that frame.
    std::array<float, 3> left_grip_rot_deg{};
    /// The left shoulder from the
    /// headset: out (along the neck's left), down (the neck below the head), back, metres. The neck faces the sum of the
    /// head-to-hand directions (both controllers), within 90 deg of the head, so looking around does not swing it.
    std::array<float, 3> left_shoulder_anchor_m{};
    /// The reach nudge: [fraction, downward damping, start (fraction of the arm length)]: beyond the start the
    /// shoulder moves toward the hand by fraction x the excess, its downward part scaled by the damping.
    std::array<float, 3> left_reach_nudge{};
    u64 left_model_scale_off{}; // The arm is drawn with +0x920 x Scale(S), S = 3 floats here
    u64 left_anchor_id_off{};
    u64 left_abs_banks_off{};
    /// The right arm on char_fields[0]. The upper arm is a separate model hanging
    /// off `elbow_node` and turning with it, so the elbow is turned until its rest upper-arm direction (elbow ->
    /// shoulder_node at this frame's pose) points at the body's shoulder (shoulder_offset_m from the head: right, up,
    /// back, yaw only); `gun_node` (the elbow's child carrying the cannon) gets the opposite turn so the cannon stays
    /// at the hand. Indices into the character's node id list; rotations stored w, x, y, z (rot_order "wxyz"); the
    /// pose is flagged dirty (pose+dirty_at |= dirty_or).
    bool ik{};
    u32 shoulder_node{};
    u32 elbow_node{};
    u32 gun_node{};                // the elbow's child that carries the held object (counter-rotated)
    float upper_arm_sign{1.0f};    // +1: the upper arm runs from the elbow toward the shoulder node; -1: away from it
    /// The solve runs at `ik_hook` (inside the pose solve, after the animation rebuilt the parent-local
    /// coords and before the absolute pass; x<ik_pose_reg> = the pose, x<ik_char_reg> = the character, both required
    /// with ik_hook); `hook` then only sets the dirty flag. pose_signs (below): pose space = the model transform's
    /// frame with these axis signs.
    std::string ik_hook;
    u32 ik_pose_reg{};
    u32 ik_char_reg{};
    std::array<float, 3> pose_signs{1.0f, 1.0f, 1.0f}; // pose space = the model transform's frame with these axis signs
    std::array<float, 3> shoulder_offset_m{0.18f, -0.22f, 0.05f};
    u64 dirty_at{};
    u32 dirty_or{};
    std::string ik_log;            // "once" = every 600th solve; "frames" = every applied solve (test runs, capped)
    bool node_log{true};           // the node dump (diagnostic); off in shipped staging
    /// The third-person body drawn in first person. char_fields[body_char_index] is its
    /// CChar; at ik_hook its body_hide_nodes get a zero parent-local scale (the head under the camera, the arms
    /// the first-person cannon and glove already draw); children collapse with them. -1 = no body.
    int body_char_index{-1};
    std::vector<u32> body_hide_nodes;
    std::vector<u32> body_off_nodes; // menu "Show body" off: these get scale 0 too (the root: the whole body)
    /// The camera rules (ids) whose view is the body's own eyes: only while one of them matched this tick is anything
    /// hidden (a cutscene or third-person view must show the character whole). Required with body_char_index.
    std::vector<std::string> body_cameras;
    /// Procedural legs on the body. body_legs = [hip, knee, ankle] left then right (live node
    /// indices). The gait comes from the feet: body_feet_field (mat3x4, the player's feet xf; column 2 = body
    /// forward) differentiated per camera tick; body_air_field (u32) != 0 = in the air (a fixed tuck). Rotations are
    /// composed onto the animation's parent-local ones about these local axes: hip forward swing, hip side swing,
    /// knee bend (ankle = -0.5 knee about the knee axis). Speeds in world units per second.
    /// The grapple beam starts at the arm's base transform (left_beam_off) times a fixed
    /// local transform (left_beam_local_off); at left_beam_hook (x<left_arm_reg> = the arm) the base gets the hand's
    /// arm transform times that local one inverted, so the beam leaves the drawn hand.
    std::string left_beam_hook;
    u64 left_beam_off{};
    u64 left_beam_local_off{};
    /// The base is written only while the arm's state byte (left_beam_state_off) masked with left_beam_state_mask
    /// equals left_beam_state_equals: the update that reads it for the beam. Otherwise the game keeps its own.
    u64 left_beam_state_off{};
    u32 left_beam_state_mask{};
    u32 left_beam_state_equals{};
    /// (full body part 3). body_stand: parent-local rotations (w, x, y, z) that replace the animation's on
    /// these nodes (the skeleton's rest pose: standing, not the game's slight-crouch idle). body_root_nodes get
    /// translation body_root_rest x Body size + the feet's planted offset - the crouch drop, and scale = Body size.
    /// body_model_scale = world units per model unit. Model axes: +x the body's left, +y up, +z forward.
    struct StandNode {
        u32 node{};
        std::array<float, 4> rot{1.0f, 0.0f, 0.0f, 0.0f};
    };
    std::vector<StandNode> body_stand;
    /// The game's own anchors above the skeleton carry the idle's tilt and must stay as they are (forcing
    /// them upright stretched part of the mesh). So body_upright_node's stand rotation is applied relative to the
    /// live rotations of body_upright_chain (top down): its model-space rotation becomes the rest one.
    u32 body_upright_node{};
    std::vector<u32> body_upright_chain;
    /// Hidden nodes also get translation 0, so their geometry collapses onto the parent's joint. (Collapsing onto the
    /// hidden node itself fanned the geometry across the view: standing, the neck and collar joints sit at eye level.)
    bool body_hide_to_parent{};
    /// How a model-space turn reaches the lean node: +1 = conj(P) turn P q (abs = parent x child), -1 = P turn conj(P).
    float body_lean_order{};
    std::vector<u32> body_root_nodes;
    std::array<float, 3> body_root_rest{};
    float body_model_scale{};
    /// The torso leans toward the head by atan(lean / body_torso_height), split evenly over body_lean_nodes (top
    /// Down, each the child of the one before; the chest follows spine_04/03/02, there is no
    /// chest bone); body_lean_chain = the nodes above the first (top down), whose rotations make up its parent's.
    std::vector<u32> body_lean_nodes;
    std::vector<u32> body_lean_chain;
    /// Nodes collapsed onto their grandparent's joint: scale 0, translation so the joint
    /// lands there (Prime: Head_1 onto spine_04, behind and below the eyes; zeroing Neck_1 or the collars stretched
    /// shared geometry across the view).
    struct Collapse {
        u32 node{};
        u32 parent{};
    };
    std::vector<Collapse> body_collapse;
    float body_torso_height{}; // model units, hips to eyes
    float body_step_m{};       // the feet re-plant under the head when it is this far (m) from them
    float body_leg_length{};   // model units, hip to ankle (crouch fold)
    float body_crouch_max{};   // model units, the deepest pelvis drop
    float body_crouch_hip_sign{};  // the hip flex's sign about body_swing_axis for a crouch (required)
    /// The model's axes (keys "+x".."-z", required with body_stand): the body's left, up and forward.
    std::array<float, 3> body_model_left{}, body_model_up{}, body_model_forward{};
    std::vector<u32> body_legs;
    std::string body_feet_field;
    std::string body_air_field;
    std::array<float, 3> body_swing_axis{0.0f, 0.0f, 1.0f};
    std::array<float, 3> body_side_axis{0.0f, 1.0f, 0.0f};
    std::array<float, 3> body_knee_axis{0.0f, 0.0f, 1.0f};
    std::array<float, 3> body_feet_forward{}; // the feet xf's local body-forward axis (key "+z"...); required with body_legs
    float body_stride{};           // world units per full gait cycle at walk (required with body_legs: game units)
    float body_walk_speed{};       // amplitudes fade in from body_still_speed up to this
    float body_run_speed{};        // walk -> run amplitudes
    float body_still_speed{};
    bool body_leg_log{};           // test log: speed, direction, phase once a second
};

/// Address file [visor_dial]: at `hook` (the HUD's visor dial update), the dial object
/// `*(x<menu_reg> + menu_off)` is told to show `choice_off` = the previewed slot, turned by `turn` at `turn_off`,
/// state `state` at `state_off`, and flags |= `flags_or` at `flags_off` (u32), every frame while a preview is on.
/// Address file [tick_rate]: the game's fixed simulation step, rewritten in code to follow the headset's refresh (the
/// Compatibility "Game frame rate"). `sites` are module offsets; `original` the bytes each site must hold before the
/// first write; `rates` per tick rate (Hz) the bytes for every site, in the same order. All sites are written together.
struct TickRate {
    bool present{};
    std::string module;
    std::vector<u64> sites;
    std::vector<std::vector<u8>> original;
    std::map<u32, std::vector<std::vector<u8>>> rates;
    u32 match_max{}; // optional: "Match headset" never goes above this rate (0 = no cap)
};

/// Address file [orbit_head]: the game picks lock-on / grapple candidates from a box in front of ITS first-person camera.
/// At `hook` the register `reg` (the camera transform pointer the box is built from) is pointed at `buffer`, guest
/// memory the camera hook fills with the head camera's 3x4 each frame, so candidates come from where the player looks.
/// While `lock_field` is 1..4 (a lock) the game turns the body toward the target; the view subtracts that turn.
/// Optional `facing_buffer`: guest memory the camera hook fills with a 3x4 whose column 2 is the head's flat forward and
/// a u32 at +0x30 (1 = valid), for a patch that tests the head's facing where the game tests the body's.
struct OrbitHead {
    bool present{};
    std::string hook;
    u32 reg{};          // required: the register the candidate box's camera pointer is in
    std::string buffer_module;
    u64 buffer_offset{};
    bool facing{};
    std::string facing_module;
    u64 facing_offset{};
    u64 facing_target_off{}; // optional: the player's lock target id; a [grapple_aim] grapple point keeps the game's facing
    std::string lock_field;
    s64 lock_min{}, lock_max{}; // required with lock_field: the values that mean a lock
};

/// Address file [camera_now]: the hands are placed at a hook that runs before the camera hook of the same tick (Prime:
/// the gun update), so the head camera is the previous tick's. The camera manager (x<reg> at the camera hook) is kept;
/// at the hand hook, while every `skip_counts` u32 of it is 0, its camera at `camera_off` gives the game camera's world
/// position (the pointer chain from it, ending at the 3x4; plus the vec3 at manager + shake_off rotated by it), and
/// the head camera moves by how far that moved since the camera hook (the view's position is the game camera's plus the
/// head's offset). Checked at every camera hook against the camera the hook writes from (max_error units).
struct CameraNow {
    bool present{};
    u32 reg{};
    u64 camera_off{};
    std::vector<u64> skip_counts;
    std::vector<u64> chain;
    s64 shake_off{-1};
    float max_error{0.01f};
    /// Optional lock_state_off: the player's lock state (u32; 0 = none), logged by the rig's camera probe.
    s64 lock_state_off{-1};
};

/// Address file [left_window]: the left arm's draw window, from its Render entry (or the rain draw) to
/// the return of its draw call. The drawn arm's pose is the one solved inside it; with ik_window_only the left arm's IK
/// runs only there (the tick-time solves keep the game's pose). probe: the rig logs one frame's order a second.
struct LeftWindow {
    bool present{};
    std::vector<std::string> open_hooks;   // besides the arms' left_render_hook (which always opens it)
    std::vector<std::string> close_hooks;
    /// The arm's PreRender entry (x = the arms' left_arm_reg) builds the pose the skin keeps, before the
    /// Render entry: the arm transform is written there too and it opens the window, when the arm's flags word
    /// (u32 at arm + prerender_flags_off) & mask == equals (drawn and not hidden).
    std::string prerender_hook;
    u64 prerender_flags_off{};
    u32 prerender_flags_mask{};
    u32 prerender_flags_equals{};
    /// The arm's state probe, every camera hook, logged once a second: the flags word (at
    /// prerender_flags_off; bit0 = shown), the state (u32 at state_off; shown_state = the free-look state the game's show
    /// path enters, its entries counted) and the player's words at player_offs (PL+0x598 grappling, PL+0x4d4 morphed).
    u64 state_off{};
    u32 shown_state{};
    std::vector<u64> player_offs;
    /// The grapple device's parts are drawn at the arm transform (+0x920) x these locals (3x4 each),
    /// which the game rewrites every frame while the beam is owned (the only time it draws them); logged at the
    /// Render entry (translation, column lengths, rotation rows).
    std::vector<u64> device_locals;
    std::string owned_field;              // a [fields] scalar: the beam owned (PS+0x90 > 0), logged with the locals
    std::vector<u64> arm_words;           // more u32 words of the arm on the state line (the clip-end gate's skips)
    /// Write the first device_abs_slots.size() device locals (device_locals[k]) at the Render
    /// entry while the player is not grappling (u32 at PL + device_skip_player_off == 0): abs with its translation x the arm scale, the
    /// abs from the left arm pose's first abs bank (*(u64)*(u64)(pose + abs_banks_off)) at device_abs_slots[k], or
    /// (slot 0) abs(device_parent_slot) x device_fallback_local[k] (3x4 row-major; needed only for a slot 0).
    bool device_write{};
    u64 device_skip_player_off{};
    u64 device_abs_banks_off{};
    std::vector<u64> device_abs_slots;
    u64 device_parent_slot{};
    std::vector<std::array<float, 12>> device_fallback_local;
    /// The device locators' rel CCoords (children of L_elbow; quaternion w,x,y,z, scale, translation)
    /// by rel id, logged on the rig once a second at our solve (b52-rel-log: constant, the rest values).
    std::vector<u32> device_rel_ids;
    /// Build 52 (b52-rel-log): the abs slots at the Render entry are not our window solve's (a tick-time compose of
    /// the clip pose), so the parts are composed from OUR elbow: at device_compose_hook (after the abs program; the
    /// pose in x<device_compose_pose_reg>), right after our window solve, L_elbow's abs (rel id device_elbow_id in
    /// the abs banks) and each written part's locator rel (device_part_rel_ids[k]) are captured; at the Render entry
    /// part k = captured elbow x rel_k (rotation x scale, translation), the translation x the arm scale.
    std::string device_compose_hook;
    u32 device_compose_pose_reg{19};
    u32 device_elbow_id{0x300};
    std::vector<u32> device_part_rel_ids;
    /// The game's local = [abs(locator).R x m.R | abs(locator).t x S], m from the CChar's render context
    /// legacy table (ctx = *(*(CChar+8)+0x20): matrices *(ctx+0x30), 0x30 each; u16 ids *(ctx+0x38); count i32 ctx+0x40),
    /// matched by each written part's locator id here (device_part_bind_ids[k]). A part without its m is not written.
    std::vector<u32> device_part_bind_ids;
    /// Rig only: a hook just after bl GetNodeId in CAnimDataMP1::GetLocatorTransform (w0 = the id);
    /// the grapple arm's three calls are told apart by the caller's return address two frames up (UpdateState
    /// main+0xc804e0 gear, +0xc8054c / +0xc805b8 nozzles). Logged on the 1 s device line.
    std::string device_nodeid_hook;
    bool ik_window_only{};
    bool probe{};
};

/// Address file [lock_ring]: the lock-on ring's reticle calls (callers by return address) are placed toward the locked
/// target from the head, at `distance` along the head's forward (the lens depth), with the head's rotation.
struct LockRing {
    bool present{};
    std::string lr_module;
    std::vector<u64> lr_offsets;
    u64 target_off{};   // required: the player's locked target id
    float distance{};   // required: along the head's forward (the lens depth); with orbit_point, the nearest depth
    /// Optional orbit_point = { reg, off }: the ring's own target point (a vec3 at x<reg> + off at the ring's
    /// callers). The ring then sits at that point's distance less surface_margin (never nearer than `distance`), on
    /// the head-to-point ray, scaled so its size on screen stays the game's.
    s32 orbit_point_reg{-1};
    u64 orbit_point_off{};
    float surface_margin{};
};

struct VisorDial {
    bool present{};
    std::string hook;
    u32 menu_reg{};
    u64 menu_off{};
    u64 choice_off{};
    u64 turn_off{};
    float turn{0.5f};
    u64 state_off{};
    u32 state{};
    u64 flags_off{};
    u32 flags_or{};
    /// Register mode (the hook sits before the game's own SetSelection call): the choice goes into
    /// these w registers and `float_value` into s<float_reg>; no field writes. Empty = field mode.
    std::vector<u32> choice_regs;
    u32 float_reg{};
    float float_value{0.5f};
    float float_alt{};             // alternate float_value / float_value - float_alt each frame (the game
                                   // skips a call whose arguments repeat)
    /// Override_frames > 0 = register mode overrides only that many calls after each new
    /// choice (value, value - float_alt, ...), then lets the game's own call settle the dial. While the preview holds,
    /// `update_hook` (the dial update; x<menu_reg>+menu_off = the dial) writes choice_off = the slot (highlight),
    /// fade_off = the f32 at fade_src_off (the title at full alpha) and flags_off |= flags_or.
    u32 override_frames{};
    std::string update_hook;
    u64 fade_off{};
    u64 fade_src_off{};
};

/// Address file [mode_c]: where the game's render job keeps the pass and the eye,
/// where its draw camera is, and the patch's eye marker colours.
/// Address file [gui_eye]: the game's GUI camera (the visor HUD: frame, rim, lens, deco,
/// the helmet) is built once per frame in its own space and replayed for both eyes, so both eyes got the same image
/// (no depth: "floating on a plane"). On the render thread, while mode C's render job runs the HUD pass (eye_pass) for
/// an eye: at proj_hook the GUI camera command's projection (a CMatrix4f at x<proj_reg> + proj_off) gets the world
/// camera's vertical FOV and the game frame's aspect (so the HUD's angular size matches the world); at view_hook the
/// view-to-world 3x4 at sp + view_off moves by -/+ half the IPD along its column 0, scaled to GUI units
/// (units_per_m / hud_depth_m: the HUD pivot at hud_depth_m). helmet_hook: the same two writes for the helmet mesh's
/// own camera (sp + helmet_proj_off, sp + helmet_view_off). The head pose is never written into the GUI camera: the
/// eye image already carries it.
/// Address file [scan_lens]: the Scan Visor lens and its range rings are morph sprites
/// queued by the scan reticle's Draw with ONE shared transform whose rotation is the game camera's (no head), so the lens
/// sat on a plane and turned into an ellipse as the head turned. At each hook (the queue functions' entries), when the
/// caller (LR) is one of `callers`, the transform at x<xform_reg> gets the same head rewrite as the scan brackets ([aim]
/// head_reticle_lr_offsets), once per Draw (the calls share it: a transform equal to the last one written is skipped).
struct ScanLens {
    bool present{};
    std::vector<std::string> hooks;
    std::string module;
    std::vector<u64> callers; // return addresses (module offsets) of the reticle's queue calls (at most 4)
    u32 xform_reg{};
    /// The game places the sprites `distance` units ahead of its camera with the camera's rotation; the
    /// rewrite writes them fresh: head position + distance x head forward, the head's rotation (never the game's P / C).
    float distance{};
};

struct GuiEye {
    bool present{};
    std::string proj_hook;
    u32 proj_reg{};        // Every key is required (the profile's; no game default in core)
    u64 proj_off{};
    std::string view_hook;
    u64 view_off{};
    std::string helmet_hook;
    u64 helmet_proj_off{}; // required with helmet_hook
    u64 helmet_view_off{};
    float units_per_m{};
    float aspect{};
    u32 eye_pass{};
    float eye_sign{};      // +1: eye R moves along +column 0 (a test checks the left/right image shift once)
    /// "world": the GUI projection gets the world camera's FOV (the HUD at true angular size: smaller, the visor rim in
    /// view); "game": the game's own GUI projection is kept (only the per-eye depth offset is added).
    std::string projection;
};

struct ModeC {
    bool present{};
    /// "render_job" (Prime) or "look_at_copy": the game's own
    /// patch draws the world twice per frame, a hook before each draw holds the draw's look-at camera copy.
    std::string backend{"render_job"};
    std::string hook;                  // the [hooks] entry at the render-job cave; look_at_copy: before eye L's draw
    // look_at_copy: the hook before eye R's draw, the register holding the camera copy at both hooks, the
    // copy's layout (sead::LookAtCamera: view matrix 3x4 row-major, eye, target, up), and u8 fields set to 0 at eye L's
    // Hook every frame (the game's temporal interlace off).
    std::string hook_right;
    u32 camera_reg{};       // all five required with look_at_copy (the profile names the engine's layout)
    u64 cam_matrix_off{};
    u64 cam_pos_off{};
    u64 cam_at_off{};
    u64 cam_up_off{};
    std::vector<std::string> zero_fields;
    u32 job_reg{0};                    // register holding the render job at the hook
    u32 scene_reg{};                   // register holding the scene at the hook
    u64 pass_offset{};                 // job: u32 pass number
    std::vector<u32> eye_passes;       // passes that get the eye camera; the first one saves the centre
    std::array<u64, 2> eye_offsets{};  // job: u32 that equals marker_bits for eye L / eye R
    u32 marker_bits{};                 // the f32 bits those words hold for the eye
    u64 draw_camera_offset{};          // scene: pointer to the draw camera node
    u64 node_transform_offset{};       // node: 3x4 camera-to-world, row major
    std::array<float, 4> marker_left{};  // eye L clear colour
    std::array<float, 4> marker_right{}; // eye R clear colour
};

struct Patch {
    std::string name;
    bool enabled{};
    u32 present_hz{}; // 0 = none
    std::string pchtxt; // relative to the profile folder, from the address file
};

struct GameProfile {
    std::filesystem::path dir;
    std::string title_id;
    std::string name;
    std::string build_id; // main NSO, 64 hex chars
    Space space;
    std::vector<Camera> cameras;
    std::vector<Hook> hooks;
    std::vector<Field> fields;
    std::vector<Patch> patches;
    // M4 [input]
    std::string layout_path;      // relative to dir; empty = no Touch mapping
    std::string recenter{"left_stick_click"};
    // M5 [render]
    std::string stereo{"mono"}; // "mono" | "afr" (mode A) | "mode_c"
    bool hud_fit{};             // hud_scale = "fit"
    float hud_fit_deg{35.0f};   // [render] hud_fit_deg, the fit cone's half-angle (20..60)
    float hud_scale{1.0f};      // hud_scale = 0.3..1.0
    float hud_depth_m{1.0f};
    std::vector<u64> flat_layers; // shader hashes drawn as flat layers (HUD)
    std::vector<float> flat_scales; // per flat_layers entry: `scale` (0.3..1.0), 0 = hud_scale
    ModeC mode_c;                 // address file [mode_c]; needed for stereo = "mode_c"
    GuiEye gui_eye;               // Address file [gui_eye] (needs [mode_c] render_job)
    ScanLens scan_lens;           // Address file [scan_lens]
    /// Address file [hint_camera]. A level camera hint runs an engine camera of
    /// its own and the game camera (and the camera hook) stops; `hook` runs for every engine camera with the camera in
    /// x<camera_reg>. A hit counts when that camera is `camera_field`'s value and `when` holds (the ball).
    /// Test measurement:
    /// address file [arm_measure]. At `compose_hook` (after the arm's abs program; x<pose_reg> = the pose) the composed abs
    /// transforms of `abs_ids` (shoulder, elbow, wrist) are logged next to the solve's intended positions. Test runs only.
    struct ArmMeasure {
        bool present{};
        std::string compose_hook;
        u32 pose_reg{};
        std::array<u32, 3> abs_ids{};
        /// After the skinning matrices are built (x<render_data_reg> = the render data, its skin array pointer at
        /// +skin_ptr_off, skin_stride bytes per 3x4; the character in x<char_reg>): the rendered joints = skin_k x rest_k.
        std::string render_hook;
        u32 render_data_reg{};
        u32 char_reg{};
        u64 skin_ptr_off{};
        u64 skin_stride{};
        std::array<std::array<float, 3>, 3> rest{};
        /// Where the arm's final model-to-world M is built (M = +0x920 x Scale(S)): at draw_hook, M =
        /// 12 floats at sp + draw_xf_sp_off, x<draw_model_reg> = the model data (arm + model_data_off), S at its +0.
        std::string draw_hook;
        u32 draw_model_reg{};
        u64 draw_xf_sp_off{};
        u64 model_data_off{};
        /// The hand's forward (fingers) and back-of-hand directions in model space at rest (the wrist bind's local -Z
        /// and -X): the drawn hand's axes = skin_wrist x these.
        std::array<float, 3> hand_forward_rest{};
        std::array<float, 3> hand_back_rest{};
        /// At the BuildPose hook for the left char: the solve-flags byte at pose + flags_off, and
        /// the solver list at pose + *(s16)(pose + list_rel_off) + list_add.
        u64 flags_off{};
        u64 list_rel_off{};
        u64 list_add{};
    } arm_measure;
    struct HintCamera {
        bool present{};
        std::string hook;
        u32 camera_reg{};
        std::string camera_field;
        When when;
    } hint_camera;
    HudWidgets hud_widgets;       // address file [hud_widgets]; needed for [hud]
    Hands hands;                  // address file [hands]
    Aim aim;                      // address file [aim]
    Snap snap;                    // address file [snap]
    VisorDial visor_dial;         // address file [visor_dial]
    OrbitHead orbit_head;         // address file [orbit_head]
    CameraNow camera_now;         // address file [camera_now]
    LeftWindow left_window;       // address file [left_window]
    TickRate tick_rate;           // address file [tick_rate]
    std::vector<std::string> move_log_fields; // address file [move_log] fields: logged with the rig's movement lines
    std::vector<std::string> scan_log_fields; // address file [scan_log] fields: logged on the Scan Visor's 1 s line
    LockRing lock_ring;           // address file [lock_ring]
    Arms arms;                    // address file [arms]
    /// Address file [states] (named game conditions for the layout's [when]), at most 32.
    std::vector<std::pair<std::string, When>> states;
    /// Format v0.2: the load's migration notes (v0.1 keys to rewrite, removed keys, keys not read yet), logged
    /// once at install.
    std::vector<std::string> notes;
    std::vector<std::string> fade_states; // address file [fade] states = [...]
    /// Passthrough by explicit state. `flat_state` names a [states] entry that holds while (game.toml [render])
    /// the player is not in gameplay (flat_state_index once resolved); `alive_hooks` are hooks that fire every in-game
    /// frame (camera_write, a frame tick): only when all of them go quiet does the silence rule pass through (title,
    /// loading). Empty = the build-33 silence rule (the camera hook alone).
    std::string flat_state;
    int flat_state_index{-1};
    std::string flat_state_hook; // the hook where flat_state's fields are readable (others keep its last value)
    std::vector<std::string> alive_hooks;
    /// A state of kind "entity_vtable": the entity an id field names has this vtable.
    struct EntityState {
        std::string id_field, table_field;
        u64 stride{}, id_check_off{}, flags_off{}, flags_mask{}, flags_value{}, entity_off{};
        u64 id_index_mask{}; // the slot index bits of an id (the rest is a version)
        std::string vtable_module;
        u64 vtable_offset{};
    };
    std::vector<std::optional<EntityState>> state_entities; // parallel to states
    GrappleAim grapple_aim{};
    VIZR::HudConfig hud{};    // game.toml [hud] mapped onto hud_widgets.elements; identity when absent
    /// game.toml [menu_labels]: the game's own words for menu settings (key = the setting's user.toml key, or
    /// "tab.<name>"); the core's labels are neutral.
    std::map<std::string, std::string> menu_labels;
    /// Address file [session_grants] fields: the only fields a launch-only grant (VIZR_SESSION_WRITES, the launcher's
    /// -SessionWrites) may write; none listed = no grant is applied.
    std::vector<std::string> session_grants;

    const Field* FindField(std::string_view field_name) const;
};

/// Loads profiles/<title_id>/game.toml and <build_id>.toml. Returns nothing and sets `error`
/// when either file is missing or invalid; `error` stays empty when there is no profile at all.
std::optional<GameProfile> Load(const std::filesystem::path& profiles_root,
                                const std::string& title_id, const std::string& build_id,
                                std::string& error);

/// Loads a layout file into the flat form the XR thread uses. The
/// profile's [input] recenter source is mapped to Recenter. An entry this build cannot read
/// (unknown key, input, target or sub-table) is skipped and described in `warnings`; only an
/// unreadable file sets `error` and returns an invalid layout.
VIZR::InputLayout LoadLayout(const std::filesystem::path& path, const std::string& recenter,
                                 std::string& error, std::vector<std::string>& warnings,
                                 const std::vector<std::string>& state_names = {});

} // namespace VIZR::Profile

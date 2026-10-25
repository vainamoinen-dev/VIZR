# SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# VIZR: stage mode C for one launch, on a profile COPY and one build dir, never the
# live profile.
# - the address file gets the render-job hook and the [mode_c] table;
# - the world-twice patch (disabled in the profile) goes into the build's load dir as @enabled (IPSwitch applies
#   lines only inside an @enabled block). Returns the patch folder, for removal after the run.
# Every path hangs off one root: VIZR_ROOT, else the caller's, else the first folder above this script with profiles\.
$VIZRRoot = if ($env:VIZR_ROOT) { $env:VIZR_ROOT.TrimEnd('\') } elseif ($VIZRRoot) { $VIZRRoot } else {
    $d = $PSScriptRoot; while ($d -and -not (Test-Path "$d\profiles")) { $d = Split-Path $d }; $d
}
function Set-StagedModeC {
    param(
        [Parameter(Mandatory)][string]$Profiles,   # the staged profiles root
        [Parameter(Mandatory)][string]$Bin         # the build's bin folder
    )
    $title = '010012101468C000'
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    $hook = 'mode_c_render_job = { module = "main", offset = 0x102a858, expect = "09 A0 42 B9 3F 09 00 71 40 00 00 54 49 02 00 35" }   # cave in front of CGameRenderJob::Execute, x0 = job, x2 = scene'
    $text = $text -replace '(?m)^\[hooks\][^\r\n]*', ("`$0`r`n" + $hook)
    $text = $text.TrimEnd() + "`r`n`r`n" + @'
[mode_c]
hook = "mode_c_render_job"
job_reg = 0                      # x0 = CGameRenderJob*
scene_reg = 2                    # x2 = CScene*
pass_offset = 0x2a0              # job: pass (0 world, 2 HUD)
eye_passes = [0, 2]              # each eye runs both; pass 0 saves the centre
eye_offsets = [0xbfc, 0xc00]     # job clear colour .r / .g
marker_bits = 0x3a800000         # 2^-10
draw_camera_offset = 0x108       # CScene::DrawCamera
node_transform_offset = 0x18     # CCameraSceneNode transform (3x4 camera-to-world)
eye_marker_left = [0.0009765625, 0.0, 0.0009765625, 1.0]
eye_marker_right = [0.0, 0.0009765625, 0.0009765625, 1.0]
'@
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
    $mod = "$Bin\user\load\$title\VIZR-C"
    New-Item -ItemType Directory -Force "$mod\exefs" | Out-Null
    # Every staging writes the whole patch set, so drop the last one's first (a test-only A/B patch such as
    # occlusion_off must never stay behind for the launcher).
    Get-ChildItem "$mod\exefs" -Filter '*.pchtxt' | ForEach-Object { [IO.File]::Delete($_.FullName) }
    $pch = (Get-Content "$Profiles\$title\patches\67c9ca14\mode_c_world_twice.pchtxt") |
        ForEach-Object { if ($_ -eq '@disabled') { '@enabled' } else { $_ } }
    [IO.File]::WriteAllLines("$mod\exefs\mode_c_world_twice.pchtxt", $pch, (New-Object Text.UTF8Encoding($false)))
    # Every patch the profile marks @enabled is installed (hud_lag_off, scan_no_snap,
    # headbob_rotation_off, rotation_shake_off, ...), not a hard-coded set; mode_c_world_twice (above) is mode C's own.
    foreach ($src in Get-ChildItem "$Profiles\$title\patches\67c9ca14" -Filter '*.pchtxt') {
        if ($src.Name -eq 'mode_c_world_twice.pchtxt') { continue }
        $pch = Get-Content $src.FullName
        if ($pch -contains '@enabled') {
            [IO.File]::WriteAllLines("$mod\exefs\$($src.Name)", $pch, (New-Object Text.UTF8Encoding($false)))
        }
    }
    # The visor HUD's GUI camera per eye (depth for the visor; the head pose is not
    # written into it).
    $text = [IO.File]::ReadAllText($address.FullName)
    $guiHooks = (@(
        'gui_cam_proj = { module = "main", offset = 0xbd9768, expect = "94 32 00 91 61 82 00 91 E0 03 14 AA A1 63 D5 97" }  # before BindProjection; x19 = CGuiCameraCommand'
        'gui_cam_view = { module = "main", offset = 0xbd9798, expect = "E1 03 00 91 E0 03 14 AA 98 62 D5 97 F4 4F 48 A9" }  # after LookAt; sp = view-to-world'
        'helmet_cam   = { module = "main", offset = 0xbda82c, expect = "E1 03 07 91 E0 03 15 AA 71 5F D5 97 E1 C3 0F 91" }  # the helmet mesh camera'
        'scan_lens_render = { module = "main", offset = 0xbe4d00, expect = "E8 03 01 AA 01 E0 00 91 E0 03 08 AA 01 00 00 14" }  # QueuePreVisor_Render (x2 = the sprite xf)'
        'scan_lens_ranges = { module = "main", offset = 0xbe4df4, expect = "E8 03 01 AA 01 E0 00 91 E0 03 08 AA 01 00 00 14" }  # QueuePreVisor_RenderRanges'
    ) -join "`r`n")
    $text = $text -replace '(?m)^\[hooks\][^\r\n]*', ("`$0`r`n" + $guiHooks)
    $text = $text.TrimEnd() + "`r`n`r`n" + @'
[gui_eye]                        # The visor HUD per eye, so it has depth
proj_hook = "gui_cam_proj"
proj_reg = 19
proj_off = 0x20                  # CGuiCameraCommand+0x20: the projection CMatrix4f (row-major)
view_hook = "gui_cam_view"
view_off = 0x0                   # sp: the view-to-world 3x4
helmet_hook = "helmet_cam"
helmet_proj_off = 0x1c0
helmet_view_off = 0x3f0
units_per_m = 21.7               # With the projection-ratio scale the visor HUD
                                 # plane sits ~21.7 GUI units ahead (10 gave 26.2 px where 1 m needs 59.7 px)
aspect = 1.7777778
eye_pass = 2                     # the render job's HUD pass
eye_sign = -1.0                  # The GUI camera's column 0 points to the player's left (+1 gave a doubled,
                                 # divergent HUD, -45.8 px; the HUD disparity test checks the sign)
projection = "game"              # The game HUD's own size; the HUD tab's "Visor size: True helmet" switches to the world FOV

[scan_lens]                      # The Scan Visor lens + range rings follow the head
hooks = ["scan_lens_render", "scan_lens_ranges"]
module = "main"
callers = [0xcc6af4, 0xcc6d14, 0xcc6d28, 0xcc6d3c]   # CReticleScanTarget::Draw's four queue calls (return addresses)
xform_reg = 2                    # x2 = &CTransform4f (row-major 3x4), shared by the four calls
distance = 1.25                  # GetCursorPlaneDistance (fmov #1.25): the sprites ride 1.25 units ahead of the head
'@
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
    return $mod
}

# The cannon and the shot follow the right hand, the
# game's cursor ray is cast from the hand and the aiming reticle sits at its hit point, instant snap turn, and the
# left trigger only in the Scan Visor or at a grapple point. Profile COPY only.
function Set-StagedHandAim {
    param([Parameter(Mandatory)][string]$Profiles)
    $title = '010012101468C000'
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    $hooks = @(
        'gun_write = { module = "main", offset = 0xc94430, expect = "C8 06 40 F9 E1 03 15 AA 08 11 40 F9 08 8D 40 F9" }   # after the last game write to the gun transform; x19 = CPlayerGunMP1*'
        'shot_spawn = { module = "main", offset = 0xc99b28, expect = "E3 43 01 91 E5 03 18 AA 41 53 54 D3 00 01 3F D6" }  # the shot spawn transform at sp+0x50 (log only)'
        'cursor_ray = { module = "main", offset = 0xcb640c, expect = "E0 03 16 AA 7D 62 0C 94 A1 43 59 BC 00 08 21 1E" }  # after GetCurrentCameraTransform in CAimingCursorMP1::Update; x19 = cursor'
        'reticle_queue = { module = "main", offset = 0xb57d04, expect = "E8 03 01 AA 01 E0 00 91 E0 03 08 AA 01 00 00 14" } # QueueRenderPreVisorGUI; x2 = &reticle world 3x4'
    ) -join "`r`n"
    $fields = @(
        'gun_xform            = { hook = "gun_write", base = "reg:x19", off = 0x1e4, type = "mat3x4" }   # CPlayerGunMP1 world transform'
        'gun_aim_xform        = { hook = "gun_write", base = "reg:x19", off = 0x1b4, type = "mat3x4" }   # assist-aim transform'
        'shot_spawn_xform     = { hook = "shot_spawn", base = "reg:sp", off = 0x50, type = "mat3x4" }        # the shot spawn transform'
        'player_xf_push       = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x18", off = 0x284, type = "u8" }   # 35.3: push the GOC xf to the body next PrePhysicsUpdate'
        'player_orbit_state   = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x18", off = 0x4e0, type = "u32" }'
        'player_grapple_state = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x18", off = 0x598, type = "u32" }'
        'player_next_orbit_id = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x18", off = 0x518, type = "u32" }   # 32: the next orbit target (grapple point check)'
        'entity_table         = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x8", off = 0x3e0060, type = "addr" }  # 35.2: *(GL+8)+0x3e0060, the id table: INLINE, an address only (never read a value from it)'
        'visor_target         = { base = "sym:gpGameState -> +0x0 -> +0x20", off = 0x1c, type = "u32" }  # 32: target visor 0 combat 1 xray 2 scan 3 thermal'
    ) -join "`r`n"
    $text = $text -replace '(?m)^\[hooks\][^\r\n]*', ("`$0`r`n" + $hooks)
    $text = $text -replace '(?m)^\[fields\][^\r\n]*', ("`$0`r`n" + $fields)
    $text = $text.TrimEnd() + "`r`n`r`n" + @'
[hands]
hook = "gun_write"
write = ["gun_xform", "gun_aim_xform"]
offset_m = [0.0, 0.0, 0.25]       # The model origin is the elbow (the cannon sat too far forward), the grip ~0.27 m ahead; move the origin back so the controller is at the grip
fire_hook = "shot_spawn"
fire_field = "shot_spawn_xform"
fire_origin_forward = 0.0        # The spawn already is the muzzle (no set-back active); +3 overshot

[aim]
ray_hook = "cursor_ray"
cursor_reg = 19
ray_xform_reg = 29
ray_xform_off = -160             # x29-0xa0
ray_columns = ["left", "up", "forward"]   # col0 = up x fwd (engine left), col1 = up, col2 = forward
cursor_on_plane_off = 0x10
cursor_hit_off = 0x94
cursor_distance_off = 0x58     # test log only (VIZR_RIG_OUT): the wall walk stops on it
reticle_hook = "reticle_queue"
scan_source = "head"             # In the Scan Visor the scan ray follows the head
scan_state = "scan_visor"
reticle_xform_reg = 2
reticle_skip_lr = "main+0xe62f80"  # CPlayerVisorMP1::DrawScanObjectIndicators
reticle_lrs = ["main+0xcc79ec", "main+0xcc7a58", "main+0xcc7a8c", "main+0xcc7ac0"]   # CReticleCombatAiming centre + arms
head_reticle_lrs = ["main+0xcc67a0", "main+0xcc68dc", "main+0xcc6934", "main+0xcc698c", "main+0xcc69e4"]   # the scan reticle on the head's plane (the lens circle is HUD, head-locked already)

[snap]
yaw_field = "player_yaw"
push_field = "player_xf_push"
skip_fields = ["player_orbit_state", "player_grapple_state", "player_morph_state"]
torso_skip_fields = ["player_grapple_state", "player_morph_state", "mp1_cine_count_a", "mp1_cine_count_b", "player_frozen_time", "player_orbit_state"]   # The body does not follow the torso there
sign = -1.0                      # With +1 a right push snapped left

[states]
scan_visor = { field = "visor_target", equals = 2 }
grapple_point = { kind = "entity_vtable", id_field = "player_next_orbit_id", table_field = "entity_table", stride = 16, id_index_mask = 0xffff, id_check_off = 0x18, flags_off = 0x28, flags_mask = 6, flags_value = 2, entity_off = 0x40, vtable = "main+0x20f0818" }
'@
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
    $layout = "$Profiles\$title\layouts\prime-remastered.toml"
    $l = [IO.File]::ReadAllText($layout)
    if ($l -notmatch '(?m)^\[when\]') {
        $l = $l.TrimEnd() + "`r`n`r`n[when]   # No lock-on: ZL only in the Scan Visor or at a grapple point`r`nleft_trigger = [`"scan_visor`", `"grapple_point`"]`r`n"
        [IO.File]::WriteAllText($layout, $l, (New-Object Text.UTF8Encoding($false)))
    }
}

# The gestures replace the left-trigger visor chord. Left hand to the head +
# a left-stick flick = visor (the ring shows), left hand on the cannon + flick = beam (Pro Controller X held = the game's
# swap-select mode), crouch = morph ball (Pro Controller Y, checked against the morph state). Needs Set-StagedHandAim first
# ([states]). Profile COPY only.
function Set-StagedGestures {
    param([Parameter(Mandatory)][string]$Profiles)
    $title = '010012101468C000'
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    if ($text -notmatch '(?m)^\[states\]') { throw 'Set-StagedGestures: no [states] (stage Set-StagedHandAim first)' }
    $text = $text -replace '(?m)^\[states\][^\r\n]*', ("`$0`r`n" + 'morphed = { field = "player_morph_state", equals = 1 }   # 0 standing, 1 ball, 2 morphing, 3 unmorphing' + "`r`n" + 'morph_moving = { field = "player_morph_state", in = [2, 3] }' + "`r`n" + 'combat_visor = { field = "visor_target", equals = 0 }')
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
    $layout = "$Profiles\$title\layouts\prime-remastered.toml"
    $l = [IO.File]::ReadAllText($layout)
    # The head gesture is not a chord (headset correction): the visor chord's modifier goes; [gestures] picks.
    $l = $l -replace '(?m)^modifier\s*=\s*"left_trigger"[^
]*', '# modifier: none (the head gesture picks visors, [gestures])'
    $l = $l -replace '(?m)^# \[chords\.beam\]', '[chords.beam]'
    $l = $l -replace '(?m)^# modifier\s*=\s*"left_grip"', 'modifier          = "left_hand_cannon"  # Hand on the cannon (was the left grip)'
    foreach ($k in 'modifier_mode', 'hold_also', 'left_stick_up', 'left_stick_down', 'left_stick_left', 'left_stick_right') {
        $l = $l -replace "(?m)^# ($k\s*=)", '$1'
    }
    $l = $l.TrimEnd() + "`r`n`r`n" + @'
[gestures]
crouch_press = "y"                  # Pro Controller Y = morph ball
crouch_state = "morphed"
crouch_block = "morph_moving"       # never tap mid-transition
ring_press = { up = "dpad_up", right = "dpad_right", down = "dpad_down", left = "dpad_left" }
ring_home = "up"                    # Combat: a head tap toggles Combat <-> the selected visor
home_state = "combat_visor"
ring = { up = "Combat", right = "Scan", down = "Thermal", left = "X-Ray" }   # tested: up Combat, right Scan; down / left follow
                                    # the game's ring order (not owned on the Frigate: unverified)
'@
    [IO.File]::WriteAllText($layout, $l, (New-Object Text.UTF8Encoding($false)))
}

# Only the visors the player owns: one "owns" state per visor from the player state's item
# counts (*(i32)(PS+0x30+item*8) > 0; items combat 0x11, X-ray 0xd, scan 5, thermal 9) and the ring's ring_owned map.
# Needs Set-StagedGestures first. Profile COPY only.
function Set-StagedOwnedVisors {
    param([Parameter(Mandatory)][string]$Profiles)
    $title = '010012101468C000'
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    $fields = @(
        'power_combat  = { base = "sym:gpGameState -> +0x0 -> +0x20", off = 0xb8, type = "u32" }  # PS+0x30+0x11*8'
        'power_xray    = { base = "sym:gpGameState -> +0x0 -> +0x20", off = 0x98, type = "u32" }  # item 0xd'
        'power_scan    = { base = "sym:gpGameState -> +0x0 -> +0x20", off = 0x58, type = "u32" }  # item 5'
        'power_thermal = { base = "sym:gpGameState -> +0x0 -> +0x20", off = 0x78, type = "u32" }  # item 9'
    ) -join "`r`n"
    $states = @(
        'owns_combat  = { not = { field = "power_combat", equals = 0 } }'
        'owns_xray    = { not = { field = "power_xray", equals = 0 } }'
        'owns_scan    = { not = { field = "power_scan", equals = 0 } }'
        'owns_thermal = { not = { field = "power_thermal", equals = 0 } }'
    ) -join "`r`n"
    $text = $text -replace '(?m)^\[fields\][^\r\n]*', ("`$0`r`n" + $fields)
    $text = $text -replace '(?m)^\[states\][^\r\n]*', ("`$0`r`n" + $states)
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
    $layout = "$Profiles\$title\layouts\prime-remastered.toml"
    $l = [IO.File]::ReadAllText($layout)
    $l = $l -replace '(?m)^ring_home = "up"', ('ring_owned = { up = "owns_combat", right = "owns_scan", down = "owns_thermal", left = "owns_xray" }' + "`r`n" + 'ring_home = "up"')
    [IO.File]::WriteAllText($layout, $l, (New-Object Text.UTF8Encoding($false)))
}

# The morph ball's third-person camera with the head's
# rotation (the game's ball-camera position and yaw, no game pitch/roll), a black comfort fade over the morph
# transition, and the game's own visor dial previewing the head-gesture choice (our ring then not drawn).
# Needs the earlier staging steps (Set-StagedHandAim to M9) first. Profile COPY only.
function Set-StagedLeftArmAndBall {
    param([Parameter(Mandatory)][string]$Profiles, [Parameter(Mandatory)][string]$Bin,
          [switch]$BallCamera,   # the 3D ball (in view and head-tracked once its yaw came from this camera)
          [switch]$OcclusionOff, # test A/B: NRenderDebugDefaults::skUseOcclusionBuffer = 0
          [switch]$BallFovGame)  # ball isolation run: the ball camera keeps the game's own FOV
    $title = '010012101468C000'
    if ($OcclusionOff) {
        $occ = @('@nsobid-67c9ca1428222752f951021464a7b3597eb98fec', '# Occlusion buffer off (A/B test: does the ball show in mode C?).',
                 '@flag offset_shift 0x100', '@enabled', '01D0E023 00', '@stop')
        [IO.File]::WriteAllLines("$Bin\user\load\$title\VIZR-C\exefs\occlusion_off.pchtxt", $occ, (New-Object Text.UTF8Encoding($false)))
    }
    # The gun draws the arm only while grappling or when a facing
    # test passes, which it does not in VR; NOP that b.le so the glove is drawn and can follow.
    $mod = "$Bin\user\load\$title\VIZR-C\exefs"
    if (-not (Test-Path $mod)) { throw "Set-StagedLeftArmAndBall: $mod missing (stage Set-StagedModeC first)" }
    $vis = @('@nsobid-67c9ca1428222752f951021464a7b3597eb98fec', '# Left arm always drawn: CPlayerGunMP1::Render b.le at main+0xc92c98 -> nop.',
             '@flag offset_shift 0x100', '@enabled', '00C92C98 1F2003D5', '@stop')
    [IO.File]::WriteAllLines("$mod\left_arm_draw.pchtxt", $vis, (New-Object Text.UTF8Encoding($false)))
    # In play the arm is active only during the gun's idle animations; the cave re-activates it whenever
    # the game turns it off (not while grappling or morphed). With the render-entry write it sits at the hand.
    # A public install carries the left-arm patch at patches\left_arm_on.pchtxt.
    $caveFile = "$VIZRRoot\patches\left_arm_on.pchtxt"
    $cave = (Get-Content $caveFile) | ForEach-Object { if ($_ -eq '@disabled') { '@enabled' } else { $_ } }
    [IO.File]::WriteAllLines("$mod\left_arm_on.pchtxt", $cave, (New-Object Text.UTF8Encoding($false)))
    $game = "$Profiles\$title\game.toml"
    $g = [IO.File]::ReadAllText($game)
    # Inserted before [patches], next to the gameplay camera (appended at the end it landed inside [render],
    # which Set-StagedRender rewrites, and was dropped).
    if ($g -notmatch '(?m)^\[patches\]') { throw 'Set-StagedLeftArmAndBall: no [patches] in game.toml to insert the camera before' }
    if ($BallCamera) { $g = $g -replace '(?m)^\[patches\]', (@'
[[camera]]                           # Watch the ball from the game's ball camera
id      = "morph_ball"
mode    = "third_person_orbit"
view_setting = "ball_view"          # Menu "Morph ball view: 3D / 2D screen"
vignette_setting = "ball_vignette"  # The tunnel while the ball rolls or the camera swings
when    = { field = "mp1_cur_cam_vptr", equals_addr = "main+0x20e9b98" }   # _ZTV14CBallCameraMP1 + 0x10
combine = "game_yaw_plus_head"      # the game's position and yaw, the head's rotation; no game roll
yaw_source = "camera"               # the player body rolls with the ball (its yaw faced backwards); turn with this camera
game_look_column = 2                # The ball camera looks along its column 2 (yaw from the basis faced backwards at times)
game_pitch_cap_deg = 45             # keep the camera's look down at the ball, up to 45 deg (level missed the ball)
source  = "cam_xform"
target  = "cam_xform"
fov     = { target = "cam_fov_rad", kind = "rad_vertical"BALLFOV }
shake   = "translation_only"
pose_space = "standing"
pose_lag_frames = 2
height_offset_m = 0.0

'@ + '[patches]') }
    $g = $g.Replace('BALLFOV', $(if ($BallFovGame) { ', mode = "game"' } else { '' }))   # ball isolation run: the game's own FOV
    [IO.File]::WriteAllText($game, $g, (New-Object Text.UTF8Encoding($false)))
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    $hooks = (@(
        'visor_dial = { module = "main", offset = 0xcace08, expect = "BE 19 00 94 40 03 40 F9 74 76 40 F9 58 CE FE 97" }  # before the visor dial SetSelection call (w1, w2 = visor, s0 = factor)'
        'visor_dial_update = { module = "main", offset = 0xcaf028, expect = "80 76 40 F9 00 1D A8 4E E1 03 1F 2A 4D 0D 00 94" }  # the dial update; x20 = CSamusHud'
    ) -join "`r`n")
    $text = $text -replace '(?m)^\[hooks\][^\r\n]*', ("`$0`r`n" + $hooks)
    $fields = (@(
        'power_morph_amount   = { base = "sym:gpGameState -> +0x0 -> +0x20", off = 0xb0, type = "u32" }  # PS+0x30+16*8: item 16 = Morph Ball (6 is the Morph Ball Bomb); written only by the rig grant'
        'power_morph_capacity = { base = "sym:gpGameState -> +0x0 -> +0x20", off = 0xb4, type = "u32" }'
    ) -join "`r`n")
    $text = $text -replace '(?m)^\[fields\][^\r\n]*', ("`$0`r`n" + $fields)
    # The left arm follows the left controller, written at its render entry (no cave: it is on in idle).
    $leftHook = 'left_arm_render = { module = "main", offset = 0xc7f308, expect = "FF 83 03 D1 FD 7B 09 A9 FD 43 02 91 F9 53 00 F9" }  # CGrappleArmMP1::Render entry; x0 = arm'
    $text = $text -replace '(?m)^\[hooks\][^\r\n]*', ("`$0`r`n" + $leftHook)
    $leftKeys = (@(
        'left_render_hook = "left_arm_render"'
        'left_arm_reg = 0'
        'left_xf_off = 0x920              # the arm render transform (rebuilt every tick by the game)'
        'left_wrist = [0.0, 0.0, 0.287]   # L_wrist in the arm model (x left, y up, z forward): 0.287 from L_elbow'
        'left_ik = true                   # The left arm as two bones, the shoulder on the body'
        'left_char_off = 0x40             # its CChar = **(arm + 0x40)'
        'left_shoulder_node = 2           # L_shoulder; 3 = L_elbow (0.330), 4 = L_wrist (0.287); bones along local +X'
        'left_elbow_node = 3'
        'left_wrist_node = 4'
        'left_shoulder_model = [0.0, 0.166, 0.285]   # the shoulder in the arm model (the wrist is at (0, 0, -0.287)); the live pose overrides it'
        'left_pose_signs = [1.0, 1.0, 1.0]  # one convention for the placement and the solve'
        'left_grip_m = [0.0, 0.0, -0.07]   # Wrist joint -> palm grip point (controller right, up, back, metres): the palm on the controller'
        'left_model_scale_off = 0x28       # the arm is drawn with +0x920 x Scale(S), S = the 3 floats at arm + 0x28 (2.0)'
        'left_grip_rot_deg = [0.0, 0.0, 0.0]   # The hand turned from the controller (pitch, yaw, roll); 0 = the controller''s own axes'
        'left_shoulder_anchor_m = [0.155, 0.13, 0.0]   # the shoulder 0.155 m out from a neck 0.13 m below the headset'
        'left_reach_nudge = [0.15, 0.4, 0.5]   # beyond half the arm length the shoulder follows the hand by 0.15, downward damped 0.4'
        'left_anchor_id_off = 0x14         # *(u32)(pose + 0x14) = the active anchor (Skeleton_Root = its abs)'
        'left_abs_banks_off = 0x44         # *(u64)(pose + 0x44) = the abs banks (same addressing as the rel coords)'
    ) -join "`r`n")
    if ($text -notmatch '(?m)^\[arms\]') { throw 'Set-StagedLeftArmAndBall: no [arms] (stage Set-StagedRightArm first)' }
    $text = $text -replace '(?m)^\[arms\][^\r\n]*', ("`$0`r`n" + $leftKeys)
    $text = $text.TrimEnd() + "`r`n`r`n" + @'
[fade]                           # black over the morph transition (the camera flies out of / into the head)
states = ["morph_moving"]        # morph state 2/3 (Set-StagedGestures), about 1 s

[visor_dial]                     # the game's own dial previews the head-gesture choice
hook = "visor_dial"
choice_regs = [1, 2]             # w1 = w2 = the visor
float_reg = 0
float_value = 0.97
float_alt = 0.01
override_frames = 2              # (c, c, 0.97) stores c, (c, c, 0.96) types its title; then the game settles it
update_hook = "visor_dial_update"
menu_reg = 20
menu_off = 0xe8                  # CSamusHud+0xe8 = the visor dial
choice_off = 0x10                # highlight
fade_off = 0xd4                  # title fade timer: held at its start value while the hand is at the head
fade_src_off = 0xd8
flags_off = 0x20
flags_or = 0x4
'@
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
    $layout = "$Profiles\$title\layouts\prime-remastered.toml"
    $l = [IO.File]::ReadAllText($layout)
    $l = $l -replace '(?m)^ring_home = "up"', ('ring_slots = { up = 0, right = 2, down = 3, left = 1 }   # the visors (0 combat, 1 X-ray, 2 scan, 3 thermal); the game dial shows the choice' + "`r`n" + 'ring_home = "up"')
    [IO.File]::WriteAllText($layout, $l, (New-Object Text.UTF8Encoding($false)))
}

# The right arm's two-bone solve: the shoulder of the beam weapon's arm model
# (node 1) goes to the body, the elbow (node 2) keeps the cannon where the hand put it, and the upper-arm model that
# hangs off the elbow follows. Needs the earlier staging steps (Set-StagedHandAim to M8) first; not with Set-StagedDialArmTest (both add [arms]). COPY only.
function Set-StagedCutscenes {   # Cutscenes in 3D
    param([Parameter(Mandatory)][string]$Profiles, [Parameter(Mandatory)][string]$Bin)
    $title = '010012101468C000'
    $mod = "$Bin\user\load\$title\VIZR-C\exefs"
    if (-not (Test-Path $mod)) { throw "Set-StagedCutscenes: $mod missing (stage Set-StagedModeC first)" }
    # The cutscene letterbox bars are screen-space, a flat band in the headset: CGameCameraLetterbox::Render -> ret.
    $bars = @('@nsobid-67c9ca1428222752f951021464a7b3597eb98fec', '# No cutscene letterbox bars: CGameCameraLetterbox::Render main+0xb2d4c0 -> ret.',
              '@flag offset_shift 0x100', '@enabled', '00B2D4C0 C0035FD6', '@stop')
    [IO.File]::WriteAllLines("$mod\no_letterbox.pchtxt", $bars, (New-Object Text.UTF8Encoding($false)))
    $game = "$Profiles\$title\game.toml"
    $g = [IO.File]::ReadAllText($game)
    $old = '(?ms)^\[\[camera\]\]\s*\r?\nid   = "cutscene".*?\r?\n\r?\n'
    if ($g -notmatch $old) { throw 'Set-StagedCutscenes: no passthrough cutscene camera in game.toml' }
    $new = @'
[[camera]]
id      = "cutscene"                # Either MP1 cinematic list non-empty, now in 3D
mode    = "first_person"
view_setting = "cutscene_view"      # Menu "Cutscenes: 3D / 2D screen"
vignette_setting = "cutscene_vignette"   # The tunnel on camera moves
when    = { any_of = [ { not = { field = "mp1_cine_count_a", equals = 0 } }, { not = { field = "mp1_cine_count_b", equals = 0 } } ] }
combine = "game_yaw_plus_head"      # the shot's position and yaw, the head's rotation: no forced pitch, roll or pans of the head
yaw_source = "camera"               # the shot's own yaw, not the player's body
source  = "cam_xform"               # at hook A cam_xform already is the cinematic camera's world xf
target  = "cam_xform"
fov     = { target = "cam_fov_rad", kind = "rad_vertical" }
shake   = "translation_only"
pose_space = "standing"
pose_lag_frames = 2
height_offset_m = 0.0
cut_fade_ms = 150                   # black 150 ms over each cut, and into and out of the cutscene
cut_field = "mp1_cur_cam_obj"       # a new shot = a new current camera object
cut_jump_units = 2.0                # jump guard for a cut inside one shot's animation
cut_jump_deg = 30.0


'@
    $g = [regex]::Replace($g, $old, $new.Replace('$', '$$'), 1)
    [IO.File]::WriteAllText($game, $g, (New-Object Text.UTF8Encoding($false)))
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    $field = 'mp1_cur_cam_obj     = { hook = "camera_write", base = "reg:x22", off = 0x8, type = "ptr" }  # The current camera actor''s +8 (one per camera object): changes on a shot cut'
    $text = $text -replace '(?m)^\[fields\][^\r\n]*', ("`$0`r`n" + $field.Replace('$', '$$'))
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
}

function Set-StagedBody {   # The third-person body in first person
    param([Parameter(Mandatory)][string]$Profiles, [Parameter(Mandatory)][string]$Bin,
          [switch]$NodeLog)   # test: dump the body's pose nodes once (map live indices to the CHPR names)
    $title = '010012101468C000'
    $mod = "$Bin\user\load\$title\VIZR-C\exefs"
    if (-not (Test-Path $mod)) { throw "Set-StagedBody: $mod missing (stage Set-StagedModeC first)" }
    # CPlayerMP1::Render returns at once in first person (cbz at 0xc72c4c) and PreRender builds no pose
    # (cbz at 0xc729ac); NOP both and the body draws with the player xf, the camera state stays first person.
    # HasTransitionGunModelData (0xc72b58) -> false: no second, third-person cannon at PL+0xbd0 (morph transitions,
    # its other user, are black behind the morph fade).
    $body = @('@nsobid-67c9ca1428222752f951021464a7b3597eb98fec', '# Third-person body in first person.',
              '@flag offset_shift 0x100', '@enabled', '00C72C4C 1F2003D5', '00C729AC 1F2003D5', '00C72B58 00008052C0035FD6', '00C6EB3C 28008052', '@stop')   # Think keeps the model-draw bit on (cset -> mov w8, #1)
    [IO.File]::WriteAllLines("$mod\body_on.pchtxt", $body, (New-Object Text.UTF8Encoding($false)))
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    $field = 'body_char = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x18 -> +0x20 -> +0x18", off = 0x0, type = "u64" }  # the body CChar = *(*(*(PL+0x20)+0x18))'
    $text = $text -replace '(?m)^\[fields\][^\r\n]*', ("`$0`r`n" + $field.Replace('$', '$$'))
    if ($text -notmatch '(?m)^char_fields = \["arm_right_char"\]') { throw 'Set-StagedBody: [arms] char_fields not as staged by Set-StagedRightArm' }
    $text = $text -replace '(?m)^char_fields = \["arm_right_char"\]', 'char_fields = ["arm_right_char", "body_char"]'
    # Hidden: 39 R_shoulder and 10 L_shoulder (the arms; the first-person cannon and glove stay). Head_1 collapses
    # instead (Set-StagedStance): zeroing Neck_1 (33) or the collars (8, 38) stretched the shared yoke across the view.
    $hide = '[39, 10]'
    $text = $text -replace '(?m)^(char_fields = \["arm_right_char", "body_char"\])', "`$1`r`nbody_char_index = 1`r`nbody_hide_nodes = $hide`r`nbody_off_nodes = [0, 1, 2, 3]   # menu Show body off: the root anchors (everything hangs below them)`r`nbody_cameras = [`"gameplay`"] # only the first-person view hides anything"
    if ($NodeLog) { $text = $text -replace '(?m)^node_log = false', 'node_log = true' }
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
}

function Set-StagedLegs {   # Procedural legs on the body
    param([Parameter(Mandatory)][string]$Profiles, [switch]$LegLog)
    $title = '010012101468C000'
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    $field = 'player_move_state = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x18", off = 0x3f0, type = "u32" }  # 0 on ground, else jump/fall'
    $text = $text -replace '(?m)^\[fields\][^\r\n]*', ("`$0`r`n" + $field.Replace('$', '$$'))
    if ($text -notmatch '(?m)^body_char_index = 1') { throw 'Set-StagedLegs: stage Set-StagedBody first' }
    $keys = (@(
        'body_legs = [46, 47, 48, 50, 51, 52]   # live nodes: L hip, knee, ankle, R hip, knee, ankle'
        'body_feet_field = "player_yaw"         # the feet xf (pure yaw, origin at the feet)'
        'body_feet_forward = "+z"               # body forward = the xf''s column 2 (model +Z)'
        'body_air_field = "player_move_state"'
        'body_swing_axis = [0.0, 0.0, 1.0]      # bones run along local +X; forward swing and knee bend about local Z'
        'body_side_axis = [0.0, 1.0, 0.0]'
        'body_knee_axis = [0.0, 0.0, 1.0]'
        'body_stride = 3.5                      # world units per gait cycle at walk (~4 cycles/s at full stick looked frantic)'
        'body_walk_speed = 4.0'
        'body_run_speed = 10.0'
        'body_still_speed = 0.3'
        $(if ($LegLog) { 'body_leg_log = true' } else { 'body_leg_log = false' })
    ) -join "`r`n")
    $text = $text -replace '(?m)^(body_char_index = 1)', ("`$1`r`n" + $keys.Replace('$', '$$'))
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
}

function Set-StagedGrapple {   # The grapple aimed from the left hand
    param([Parameter(Mandatory)][string]$Profiles)
    $title = '010012101468C000'
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    $hooks = (@(
        'orbit_select = { module = "main", offset = 0xc77b40, expect = "61 1A 45 B9 F7 03 00 AA E0 03 16 AA 68 A4 FF 97" }  # UpdateOrbitSelection after it stored its candidate (ZL held / orbiting); x19 = PL, x22 = GL'
        'orbit_idle = { module = "main", offset = 0xc78090, expect = "C0 02 80 52 7F 89 DF 97 E1 03 00 AA" }  # UpdateOrbitInput idle path, every unmorphed tick, after it stored its candidate; x19 = PL, x23 = GL'
        'grapple_arm_update = { module = "main", offset = 0xc803d0, expect = "FF 83 02 D1 EA 2B 00 FD F7 3F 00 F9 E9 A3 05 6D" }  # CGrappleArmMP1::UpdateState entry; x0 = arm'
    ) -join "`r`n")
    $text = $text -replace '(?m)^\[hooks\][^\r\n]*', ("`$0`r`n" + $hooks.Replace('$', '$$'))
    $fields = (@(
        'invalid_unique_id = { base = "static:main+0x459c9a0", off = 0x0, type = "u32" }   # kInvalidUniqueId'
        'orbit_lock_range  = { base = "static:main+0x4b32880 -> +0x0", off = 0x1a4, type = "f32" }   # gpTweakPlayer+0x1a4, the orbit lock range'
    ) -join "`r`n")
    $text = $text -replace '(?m)^\[fields\][^\r\n]*', ("`$0`r`n" + $fields.Replace('$', '$$'))
    if ($text -notmatch '(?m)^left_render_hook = ') { throw 'Set-StagedGrapple: stage Set-StagedLeftArmAndBall first (the left arm)' }
    $beam = (@(
        'left_beam_hook = "grapple_arm_update"   # the beam starts at the hand'
        'left_beam_off = 0x830                   # the arm base transform (beam origin = base x local)'
        'left_beam_local_off = 0x8f0             # the fixed local transform'
        'left_beam_state_off = 0xa60             # the arm state byte: UpdateState runs the swing (the beam) only when'
        'left_beam_state_mask = 0x29             # bit0 set, bit5 clear (active, loaded) and bit3 clear (not gun-controlled)'
        'left_beam_state_equals = 0x01'
    ) -join "`r`n")
    $text = $text -replace '(?m)^(left_render_hook = [^\r\n]*)', ("`$1`r`n" + $beam.Replace('$', '$$'))
    $text = $text.TrimEnd() + "`r`n`r`n" + @'
[grapple_aim]                    # The left hand picks the grapple point
sites = [ { hook = "orbit_idle", player_reg = 19, gl_reg = 23 }, { hook = "orbit_select", player_reg = 19, gl_reg = 22 } ]   # idle every tick; held ZL / orbiting
candidate_off = 0x518            # the orbit candidate the ZL press locks
list_off = 0x520                 # all orbitables in range: u32 count, data pointer at +8
list_data_off = 0x8
table_chain = [0x8]              # *(GL+8): the object list's parent (35.2)
table_add = 0x3e0060             # the list is INLINE at +0x3e0060, not a pointer (GetObjectById 0xb0ed6c)
deleted_off = 0x18               # the game skips an entity with *(u8)(entity+0x18) & 2 (being deleted)
deleted_mask = 2
id_index_mask = 0xffff           # 35.2: slot = id & 0xffff (the version is the high 16 bits)
stride = 16
id_check_off = 0x18
flags_off = 0x28
flags_mask = 6
flags_value = 2
entity_off = 0x40
vtable = "main+0x20f0818"        # CScriptGrapplePointMP1
point_chain = [0x8, 0x20, 0x40]  # the point's GOC xf (3x4, translation = column 3)
max_angle_deg = 12.0
scan_max_angle_deg = 17.0        # The yellow hole's angular radius (never a target outside yellow)
scan_holder_off = 0xa0           # GetScannableObjectInfo: holder = *(actor+0xa0)
scan_info_off = 0x10             #   scannable iff holder && (*(u64)(holder+0x10) || *(u8)(holder+8))
scan_info_flag_off = 0x8
scan_lists = [0x520, 0x530, 0x540]   # in range + the two zone lists
scan_state_offs = [0x4e0, 0x4ec, 0x584, 0x588]   # test log: orbit state, locked target, scan state, scan time
release_log_offs = [0x5bc, 0x5bd]   # Free look (UpdateFreeLookState 0xc6cca4), logged 2 s after a scan-lock release
range_field = "orbit_lock_range"
invalid_field = "invalid_unique_id"

[grapple_aim.zl_log]            # The grapple gate line at 0xc78090 (w25 = the game's own candidate)
hook = "orbit_idle"
candidate_reg = 25
player_offs = [0x4e0, 0x4ec, 0x598]   # orbit state, locked target, grapple state
fields = ["grapple_amount", "visor_current"]   # PS+0x90 (> 0 to fire), PS+0x18 (the visor; 2 blocks)
'@
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
}

function Set-StagedStance {   # The head's room position moves the view; the body stands and follows it
    param([Parameter(Mandatory)][string]$Profiles, [double]$LeanOrder = 0)   # LeanOrder (test): another body_lean_order
    $title = '010012101468C000'
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    if ($text -notmatch '(?m)^body_legs = ') { throw 'Set-StagedStance: stage Set-StagedLegs first (the legs)' }
    $keys = @'
body_stand = [   # the skeleton's rest pose: standing, not the crouch idle
  { node = 3, rot = [0.4408, 0.5529, 0.4408, 0.5529] },    # Skeleton_Root
  { node = 4, rot = [0.0000, 1.0000, 0.0046, 0.0000] },    # spine_01
  { node = 5, rot = [0.9992, 0.0000, 0.0000, 0.0409] },    # spine_02
  { node = 6, rot = [0.9986, 0.0000, 0.0000, 0.0531] },    # spine_03
  { node = 7, rot = [1.0000, 0.0000, 0.0000, 0.0000] },    # spine_04
  { node = 45, rot = [0.9937, 0.0000, 0.0000, -0.1121] },  # pelvis
  { node = 46, rot = [0.0403, 0.0000, 0.0000, 0.9992] },   # L_hip
  { node = 47, rot = [0.9948, 0.0000, 0.0000, 0.1023] },   # L_knee
  { node = 48, rot = [0.9174, 0.0000, 0.0000, -0.3981] },  # L_ankle
  { node = 49, rot = [1.0000, 0.0000, 0.0000, 0.0000] },   # L_toe
  { node = 50, rot = [0.9992, 0.0000, 0.0000, -0.0403] },  # R_hip
  { node = 51, rot = [0.9948, 0.0000, 0.0000, 0.1023] },   # R_knee
  { node = 52, rot = [0.0000, 0.9174, 0.3981, 0.0000] },   # R_ankle
  { node = 53, rot = [0.9055, 0.0000, 0.0000, 0.4243] },   # R_ball
  { node = 54, rot = [0.7071, 0.0000, 0.7071, 0.0000] },   # R_toe
]
body_root_nodes = [0]            # root.anchorBlend: translation and size only (its rotation stays the game's)
body_upright_node = 3            # Skeleton_Root: its rest rotation is applied in model space, cancelling the anchors'
body_upright_chain = [0, 2]      # (the idle's 38 deg anchor tilt leaned the body into the camera;
                                 #  forcing the anchors upright stretched part of the mesh across the view)
body_root_rest = [0.0, 1.133, -0.138]  # the skeleton root at rest (1.183, 0.012) moved 0.05 down and 0.15 back: the eyes sit in front of the shoulder yoke, as a real head does (at the model eye point the camera was inside the yoke)
body_model_scale = 1.48          # world units per model unit
LEANKEYS
body_torso_height = 0.5          # model units, pelvis to eyes (1.183 -> 1.689)
body_step_m = 0.3                # a head this far from the feet re-plants them (a step)
body_leg_length = 0.986          # thigh 0.494 + shin 0.492
body_crouch_max = 0.5
body_crouch_hip_sign = -1.0       # +1 lifted the feet behind (thighs stayed vertical)
body_hide_to_parent = false      # Collapsing onto the parent joint made it worse (plates in the view)
body_lean_order = 1.0            # Lean computed but not visible; LEANORDER (test A/B)
body_model_left = "+x"           # the model faces +Z, Y up
body_model_up = "+y"
body_model_forward = "+z"
'@
    $text = $text -replace '(?m)^(body_legs = [^\r\n]*)', ("`$1`r`n" + $keys.Replace("`r`n", "`n").Replace("`n", "`r`n").Replace('$', '$$'))
    $lean = "body_lean_nodes = [5, 6, 7]       # Spine_02..04, a third each (the chest follows spine_04/03/02)`r`n" +
            "body_lean_chain = [0, 2, 3, 4]    # above spine_02, top down (anchor, blendspace, Skeleton_Root, spine_01)`r`n" +
            "body_collapse = [ { node = 34, parent = 33 } ]   # Head_1 onto spine_04's joint, behind and below the eyes (Neck_1 has no weights)"
    $text = $text.Replace('LEANKEYS', $lean)
    if ($LeanOrder -ne 0) { $text = $text -replace '(?m)^body_lean_order = [^\s#]+', "body_lean_order = $LeanOrder" }
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
}

function Set-StagedPassthrough {   # Passthrough by explicit state; any camera in ball form stays 3D
    param([Parameter(Mandatory)][string]$Profiles)
    $title = '010012101468C000'
    # [render]: the keep-alive hooks and the explicit flat state; the camera hook going quiet (ball form, a
    # level camera, the unmorph) no longer means the flat screen.
    # Inserted under the [render] header, not through Set-StagedRender (it rewrites [render] with only the keys it is
    # given: a rewrite once lost stereo = "mode_c" that way and ran mono).
    $game = "$Profiles\$title\game.toml"
    $g = [IO.File]::ReadAllText($game)
    if ($g -notmatch '(?m)^\[render\]') { throw 'Set-StagedPassthrough: no [render] in game.toml' }
    $renderKeys = (@(
        'alive_hooks = ["camera_write", "io_tick"]   # Only when both go quiet (title, loading) is the view flat'
        'flat_state = "flat_screen"                   # The address file state that means "not in gameplay"'
        'flat_state_hook = "io_tick"                  # its pause / GUI fields are read at the frame tick'
    ) -join "`r`n")
    $g = $g -replace '(?m)^\[render\][^\r\n]*', ("`$0`r`n" + $renderKeys)
    [IO.File]::WriteAllText($game, $g, (New-Object Text.UTF8Encoding($false)))
    # game.toml: after the ball camera, any camera while morphed is watched as the ball (it went flat six times in
    # ball form came from cameras the profile did not know: the morph transition and level cameras).
    $game = "$Profiles\$title\game.toml"
    $g = [IO.File]::ReadAllText($game)
    if ($g -notmatch '(?m)^\[patches\]') { throw 'Set-StagedPassthrough: no [patches] in game.toml to insert the camera before' }
    $g = $g -replace '(?m)^\[patches\]', (@'
[[camera]]                           # Any camera while morphed or morphing (after "morph_ball")
id      = "morph_any"
mode    = "third_person_orbit"
view_setting = "ball_view"
vignette_setting = "ball_vignette"
when    = { field = "player_morph_state", in = [1, 2, 3] }   # 1 morphed, 2 morphing, 3 unmorphing
combine = "game_yaw_plus_head"
yaw_source = "camera"
game_pitch_cap_deg = 45
source  = "cam_xform"
target  = "cam_xform"
fov     = { target = "cam_fov_rad", kind = "rad_vertical" }
pose_lag_frames = 2
height_offset_m = 0.0

'@ + '[patches]')
    [IO.File]::WriteAllText($game, $g, (New-Object Text.UTF8Encoding($false)))
    # The address file: the frame tick and the explicit-state fields and states.
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    $hook = 'io_tick = { module = "main", sym = "_ZN13CGameIOWinMP19TimerTickERK21CArchMsgParmTimerTickR18CArchitectureQueue", sym_off = 0x0, offset = 0xe51f00, expect = "FD 7B BB A9 F9 0B 00 F9 FD 03 00 91 F8 5F 02 A9" }   # every frame of a game session (pause, map, death too); x0 = CGameIOWinMP1'
    $text = $text -replace '(?m)^\[hooks\][^\r\n]*', ("`$0`r`n" + $hook)
    $fields = (@(
        'hud_iowin        = { base = "static:main+0x459a428", off = 0x0, type = "ptr" }   # CHUDIOWin::Instance (null outside a session)'
        'grapple_amount   = { base = "sym:gpGameState -> +0x0 -> +0x20", off = 0x90, type = "u32" }   # PS+0x90, the grapple gate (> 0)'
        'map_flags        = { base = "static:main+0x459a428 -> +0x0 -> +0x38 -> +0x4e0150", off = 0xc10, type = "u8" }   # bit 2 = map open'
        'loading_ptr      = { base = "static:main+0x457a0c0", off = 0x0,   type = "ptr" }'
        'loading_state    = { base = "static:main+0x457a0c0 -> +0x0", off = 0x114, type = "u32" }   # active if != 0xFFFFFFFF'
        'loading_fade_in  = { base = "static:main+0x457a0c0 -> +0x0", off = 0xf8,  type = "u32" }   # f32 bits: active if != 0'
        'loading_fade_out = { base = "static:main+0x457a0c0 -> +0x0", off = 0x100, type = "u32" }'
        'movie_current    = { base = "static:main+0x457a200 -> +0x0", off = 0x70, type = "ptr" }   # probable (confirm live)'
        'io_pause         = { hook = "io_tick", base = "reg:x0", off = 0x58, type = "u32" }   # 0 play, 1 paused, 2 dead, 3 cinematic skip'
    ) -join "`r`n")
    $text = $text -replace '(?m)^\[fields\][^\r\n]*', ("`$0`r`n" + $fields)
    # The level camera hints' engine camera, seen at hook B while the ball is out (logged first).
    $text = $text -replace '(?m)^(camera_write_b\s*=.*?)enabled\s*=\s*false', '$1enabled = true'
    $hintFields = (@(
        'hint_cinematic_cam = { base = "static:main+0x459a428 -> +0x0 -> +0x38 -> +0x4e0168 -> +0x8", off = 0xe8, type = "ptr" }   # engine CCameraManager+0xe8'
        'hint_morph_state   = { base = "static:main+0x459a428 -> +0x0 -> +0x38 -> +0x4e0150 -> +0x18", off = 0x4d4, type = "u32" }   # PL+0x4d4 without hook-A registers'
    ) -join "`r`n")
    $text = $text -replace '(?m)^\[fields\][^\r\n]*', ("`$0`r`n" + $hintFields)
    $text = $text.TrimEnd() + "`r`n`r`n" + @'
[hint_camera]                    # Logging first
hook = "camera_write_b"          # CGameCamera::UpdateCameraState, every engine camera
camera_reg = 0
camera_field = "hint_cinematic_cam"
when = { field = "hint_morph_state", equals = 1 }
'@
    $states = 'flat_screen = { any_of = [ { field = "hud_iowin", is_null = true }, { all_of = [ { not = { field = "loading_ptr", is_null = true } }, { any_of = [ { not = { field = "loading_state", equals = 0xFFFFFFFF } }, { not = { field = "loading_fade_in", equals = 0 } }, { not = { field = "loading_fade_out", equals = 0 } } ] } ] }, { not = { field = "movie_current", is_null = true } }, { field = "io_pause", equals = 1 } ] }   # Not in a session, loading, a movie, a pause menu (the map is io_pause 1)'
    $text = $text -replace '(?m)^\[states\][^\r\n]*', ("`$0`r`n" + $states)
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
}

function Set-StagedRightArm {
    param([Parameter(Mandatory)][string]$Profiles, [Parameter(Mandatory)][string]$Bin, [switch]$IkLog,
          [switch]$LeftArm)   # LeftArm: the left-arm patch + draw patch + left xf (hides the game's left glove; test only)
    $title = '010012101468C000'
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    $hook = (@(
        'build_pose = { module = "main", offset = 0xba5760, expect = "68 26 40 79 60 02 08 8B E1 03 13 AA 70 EC FF 97" }  # CChar::BuildPose after ApplyResults; x19 = CChar (dirty flag here)'
        'pose_solve = { module = "main", offset = 0xba0994, expect = "08 79 18 12 68 06 00 B9 68 52 80 79 68 02 08 8B" }  # CCharPose::Solve after the rel program, before abs; x19 = pose, x20 = CChar'
    ) -join "`r`n")
    $field = (@(
        'arm_right_char = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x18 -> +0x690 -> +0x4f8 -> +0x5c", off = 0x0, type = "u64" }  # **(W+0x5c), W = *(gun+0x4f8)'
        'left_arm_xf    = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x18 -> +0x690 -> +0x520", off = 0x920, type = "mat3x4" }  # the left arm render xf (the game resets it each frame)'
    ) -join "`r`n")
    if ($LeftArm) {
        # The left arm's one-shot cave (EnterFreeLook while not grappling/morphed) beside the mode C patch.
        $patch = "$VIZRRoot\patches\left_arm_on.pchtxt"
        $mod = "$Bin\user\load\$title\VIZR-C\exefs"
        if (-not (Test-Path $mod)) { throw "Set-StagedRightArm: $mod missing (stage Set-StagedModeC first)" }
        $pch = (Get-Content $patch) | ForEach-Object { if ($_ -eq '@disabled') { '@enabled' } else { $_ } }
        [IO.File]::WriteAllLines("$mod\left_arm_on.pchtxt", $pch, (New-Object Text.UTF8Encoding($false)))
        # The gun draws the arm only while grappling or when a facing test passes (drawn at one of
        # three hand positions); NOP that b.le so the arm always draws.
        $vis = @('@nsobid-67c9ca1428222752f951021464a7b3597eb98fec', '# Left arm always drawn: CPlayerGunMP1::Render b.le at main+0xc92c98 -> nop.',
                 '@flag offset_shift 0x100', '@enabled', '00C92C98 1F2003D5', '@stop')
        [IO.File]::WriteAllLines("$mod\left_arm_draw.pchtxt", $vis, (New-Object Text.UTF8Encoding($false)))
    }
    # The game dial override showed no title during the hold; our ring stays until it does (Dev20 keeps it).
    $text = $text -replace '(?m)^\[hooks\][^\r\n]*', ("`$0`r`n" + $hook)
    $text = $text -replace '(?m)^\[fields\][^\r\n]*', ("`$0`r`n" + $field)
    $log = if ($IkLog) { 'ik_log = "frames"' } else { '' }
    $left = if ($LeftArm) { @('left_xf_field = "left_arm_xf"    # the cave turns the arm on; we place it each frame',
                              'left_offset_m = [0.0, 0.0, 0.27] # the arm model origin is L_elbow, the wrist 0.287 ahead',
                              'left_columns = ["forward", "up", "right"]   # its bones run along +X (sideways otherwise)') -join "`r`n" } else { '' }
    $text = $text.TrimEnd() + "`r`n`r`n" + @"
[arms]                           # The right arm's two-bone solve
hook = "build_pose"
char_reg = 19
char_fields = ["arm_right_char"]
pose_off_at = 0x12               # pose = CChar + u16 at +0x12
coords_table_at = 0x3c
node_set_chain = [0x8, 0x10, 0x10]
count_at = 0x0
ids_at = 0x10
node_log = false
ik = true
shoulder_node = 1                # shoulder (bone 0.320)
elbow_node = 2                   # elbow (0.320): the upper-arm model hangs here
gun_node = 3                     # gun_bind: the cannon (counter-rotated)
ik_hook = "pose_solve"           # solve inside CCharPose::Solve (BuildPose writes were overwritten)
ik_pose_reg = 19
ik_char_reg = 20
pose_signs = [1.0, 1.0, -1.0]   # pose space has the barrel along -Z
rot_order = "wxyz"
shoulder_offset_m = [0.24, -0.22, 0.05]   # the right shoulder from the head: right, up, back (yaw only); 0.18 felt like the chest
dirty_at = 0x4
dirty_or = 0x80
$left
$log

"@
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
}

# The game's own visor ring (owned visors only): the HUD's visor dial previews the
# head-gesture choice; the custom ring is not drawn (ring_slots set). Needs Set-StagedGestures first. Profile COPY only.
function Set-StagedDialArmTest {   # work in progress (dial override, arm dump): tests only, never the launcher
    param([Parameter(Mandatory)][string]$Profiles, [switch]$LeftForce)   # LeftForce: breaks the guest (kept for tests)
    $title = '010012101468C000'
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    $hook = @(
        'visor_dial = { module = "main", offset = 0xcace08, expect = "BE 19 00 94 40 03 40 F9 74 76 40 F9 58 CE FE 97" }  # before the visor dial SetSelection call (w1, w2 = slot, s0 = turn)'
        'build_pose = { module = "main", offset = 0xba5760, expect = "68 26 40 79 60 02 08 8B E1 03 13 AA 70 EC FF 97" }  # CChar::BuildPose after ApplyResults; x19 = CChar'
    ) -join "`r`n"
    $fields = @(
        'arm_right_char = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x18 -> +0x690 -> +0x4f8 -> +0x5c", off = 0x0, type = "u64" }  # **(W+0x5c), W = *(gun+0x4f8) (the beam weapon model)'
        'arm_second_char = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x18 -> +0x690 -> +0x4f8 -> +0x128", off = 0x0, type = "u64" }  # fallback: the second model at W+0x110 (CModelData; +0x18 = anim data, +0 = CChar): a guess for the dump'
        'arm_left_char  = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x18 -> +0x690 -> +0x520 -> +0x40", off = 0x0, type = "u64" }  # 37.1: **(arm+0x40)'
        'left_arm_flags = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x18 -> +0x690 -> +0x520", off = 0xa60, type = "u32" }  # 37.2: bit0 active, bit5 hidden'
        'left_arm_xf    = { hook = "camera_write", base = "reg:x20 -> +0x4e0150 -> +0x18 -> +0x690 -> +0x520", off = 0x920, type = "mat3x4" }  # 37.2: the arm render xf'
    ) -join "`r`n"
    $text = $text -replace '(?m)^\[hooks\][^\r\n]*', ("`$0`r`n" + $hook)
    $text = $text -replace '(?m)^\[fields\][^\r\n]*', ("`$0`r`n" + $fields)
    $text = $text.TrimEnd() + "`r`n`r`n" + @'
[arms]                           # diagnostic: log both arm models, force the left arm on
hook = "build_pose"
char_reg = 19
char_fields = ["arm_right_char", "arm_left_char", "arm_second_char"]
pose_off_at = 0x12               # pose = CChar + u16 at +0x12
coords_table_at = 0x3c           # u64 at pose+0x3c (unaligned)
node_set_chain = [0x8, 0x10, 0x10]
count_at = 0x0
ids_at = 0x10
# left_flags_field = "left_arm_flags"
# flags_set = 0x1
# flags_clear = 0x20
# left_xf_field = "left_arm_xf"
# left_offset_m = [0.0, 0.0, 0.27] # the arm model origin is L_elbow, the wrist 0.287 ahead
'@
    if ($LeftForce) {
        $text = $text.TrimEnd() + "`r`n" + (@('left_flags_field = "left_arm_flags"', 'flags_set = 0x1', 'flags_clear = 0x20',
            'left_xf_field = "left_arm_xf"', 'left_offset_m = [0.0, 0.0, 0.27]') -join "`r`n") + "`r`n"
    }
    $text = $text.TrimEnd() + "`r`n`r`n" + @'
[visor_dial]                     # the game's own SetSelection call previews the head-gesture choice
hook = "visor_dial"
choice_regs = [1, 2]             # w1 = w2 = the slot (highlight and title)
float_reg = 0
float_value = 0.5                # s0: how far the dial turns
'@
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
    $layout = "$Profiles\$title\layouts\prime-remastered.toml"
    $l = [IO.File]::ReadAllText($layout)
    $l = $l -replace '(?m)^ring_home = "up"', ('ring_slots = { up = 0, right = 2, down = 3, left = 1 }   # the dial''s slots (0 combat, 1 X-ray, 2 scan, 3 thermal)' + "`r`n" + 'ring_home = "up"')
    [IO.File]::WriteAllText($layout, $l, (New-Object Text.UTF8Encoding($false)))
}

# Hotfix: in menus (title, pause, map) Touch A confirms and Touch B goes back, while gameplay keeps the
# player's map (A = jump, B = map). [map.menu] goes into the staged layout only; builds before the hotfix skip it.
function Set-StagedMenuMap {
    param([Parameter(Mandatory)][string]$Profiles)
    $layout = "$Profiles\010012101468C000\layouts\prime-remastered.toml"
    $text = [IO.File]::ReadAllText($layout)
    if ($text -notmatch '(?m)^\[map\.menu\]') {
        $text = $text.TrimEnd() + "`r`n`r`n[map.menu]   # while the game is in a menu (camera hook silent)`r`nright_a = `"a`"   # confirm`r`nright_b = `"b`"   # back`r`n"
        [IO.File]::WriteAllText($layout, $text, (New-Object Text.UTF8Encoding($false)))
    }
}

# VIZR MH: per-element HUD hooks, widget layout and element roots, on a
# profile COPY (an older build would install these hooks with no handler, and an unmatched hook turns its camera to
# passthrough). -Hud adds a game.toml [hud] block (test values; none = the game's HUD).
function Set-StagedHud {
    param(
        [Parameter(Mandatory)][string]$Profiles,
        [string[]]$Hud = @()   # e.g. 'energy = { offset = [0.1, -0.1] }'
    )
    $title = '010012101468C000'
    $address = Get-ChildItem "$Profiles\$title" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $text = [IO.File]::ReadAllText($address.FullName)
    $hooks = @(
        'hud_draw_enter = { module = "main", offset = 0xcb01d8, expect = "FF 03 03 D1 E9 23 06 6D F9 43 00 F9 FD 7B 07 A9" }   # CSamusHudMP1::Draw entry, x0 = hud'
        'hud_draw_exit = { module = "main", offset = 0xcb0498, expect = "C0 03 5F D6 08 14 41 B9 1F 0D 00 71 88 00 00 54" }    # its only ret'
        'minimap_update_enter = { module = "main", offset = 0xe5c540, expect = "FF C3 04 D1 EF 3B 09 6D ED 33 0A 6D EB 2B 0B 6D" }  # CMiniMapMP1::Update, x0 = minimap'
        'minimap_update_exit = { module = "main", offset = 0xe5caa0, expect = "C0 03 5F D6 00 08 40 B9 C0 03 5F D6 00 30 00 91" }   # its ret'
    ) -join "`r`n"
    $fields = @(
        'hud_state            = { hook = "hud_draw_enter", base = "reg:x0", off = 0x114, type = "u32" }   # 0 combat, 1 x-ray, 2 thermal, 3 scan, 4 ball, 5 none'
        'hud_root_energy      = { hook = "hud_draw_enter", base = "reg:x0 -> +0xb8", off = 0x28, type = "ptr" }   # basewidget_energystuff'
        'hud_root_missiles    = { hook = "hud_draw_enter", base = "reg:x0 -> +0xc8 -> +0x60", off = 0x68, type = "ptr" }   # basewidget_missilestuff'
        'hud_root_threat      = { hook = "hud_draw_enter", base = "reg:x0 -> +0xc0", off = 0x58, type = "ptr" }   # basewidget_threatstuff'
        'hud_root_visor_frame = { hook = "hud_draw_enter", base = "reg:x0 -> +0xd8", off = 0x80, type = "ptr" }   # basewidget_deco (combat only)'
        'hud_root_scan_pivot  = { hook = "hud_draw_enter", base = "reg:x0 -> +0xd8", off = 0x308, type = "ptr" }   # Scan Visor (state 3): basewidget_pivot (frame, freelook brackets, the Energy word; no bar)'
        'hud_root_scan_left   = { hook = "hud_draw_enter", base = "reg:x0 -> +0xd8", off = 0x310, type = "ptr" }   # basewidget_leftside'
        'hud_root_scan_right  = { hook = "hud_draw_enter", base = "reg:x0 -> +0xd8", off = 0x318, type = "ptr" }   # basewidget_rightside'
        'hud_root_scan_gauge  = { hook = "hud_draw_enter", base = "reg:x0 -> +0xd8", off = 0x328, type = "ptr" }   # the scan box (FRME_ScanHudFlat) basewidget_scanguage'
        'hud_root_scan_text   = { hook = "hud_draw_enter", base = "reg:x0 -> +0xd8", off = 0x340, type = "ptr" }   # the scan box basewidget_textgroup'
        'hud_root_boss_energy = { hook = "hud_draw_enter", base = "reg:x0 -> +0x108", off = 0x18, type = "ptr" }   # basewidget_bossenergystuff (visor frame; null in scan/ball)'
        'hud_root_radar       = { hook = "hud_draw_enter", base = "reg:x0 -> +0xf8", off = 0x50, type = "ptr" }   # basewidget_radarstuff'
        'hud_root_visor_menu  = { hook = "hud_draw_enter", base = "reg:x0 -> +0xe8", off = 0x28, type = "ptr" }   # basewidget_visormenu'
        'hud_root_beam_menu   = { hook = "hud_draw_enter", base = "reg:x0 -> +0xf0", off = 0x28, type = "ptr" }   # basewidget_beammenu'
        'hud_root_message     = { hook = "hud_draw_enter", base = "reg:x0", off = 0x488, type = "ptr" }   # basewidget_message'
        'hud_frame_base       = { hook = "hud_draw_enter", base = "reg:x0 -> +0x88", off = 0x28, type = "ptr" }   # FRME_BaseHud root widget (whole HUD)'
        'hud_frame_visor      = { hook = "hud_draw_enter", base = "reg:x0 -> +0xb0", off = 0x28, type = "ptr" }   # the visor frame (FRME_CombatHud ...) root widget'
    ) -join "`r`n"
    $text = $text -replace '(?m)^\[hooks\][^\r\n]*', ("`$0`r`n" + $hooks)
    $text = $text -replace '(?m)^\[fields\][^\r\n]*', ("`$0`r`n" + $fields)
    $text = $text.TrimEnd() + "`r`n`r`n" + @'
[hud_widgets]
enter_hook = "hud_draw_enter"
exit_hook = "hud_draw_exit"
world_xf_offset = 0x38           # CGuiObject O2W, 3x4 row-major
first_child_offset = 0x70
next_sibling_offset = 0x78
alpha_offset = 0x100             # CGuiWidget effective colour alpha (f32)
drawn_flags = [[0xa8, 0x01], [0x104, 0x20], [0x80, 0x01]]   # Visible, contributes, transform valid (the HUD crop test)
alpha_bypass = [0x104, 0x10]     # drawn at alpha 0 too
units_per_screen_height = 12.742 # FRME units per screen height at depth 10 (2 * 10 * tan(32.5 deg))
extent_camera = [65.0, 20.0, 1.778]   # the HUD camera (vertical fov deg, distance to the HUD plane, aspect) for the test log of each element's extent
state_field = "hud_state"
whole_roots = ["hud_frame_base", "hud_frame_visor"]   # the whole-HUD sliders move every widget under these
elements = [                     # order = menu order; [hud] keys in game.toml
  { name = "energy",      root_field = "hud_root_energy" },
  { name = "missiles",    root_field = "hud_root_missiles" },
  { name = "threat",      root_field = "hud_root_threat" },
  { name = "visor_frame", root_field = "hud_root_visor_frame", only_in_state = 0, root_field_in_state = [[3, "hud_root_scan_pivot"], [3, "hud_root_scan_left"], [3, "hud_root_scan_right"], [3, "hud_root_scan_gauge"], [3, "hud_root_scan_text"]] },   # In the Scan Visor the same slider places its own deco and scan box (never +0x300: it holds the energy bar)
  { name = "radar",       root_field = "hud_root_radar", base_offset = [0.35, 0.0] },   # At NDC x -0.73 it is outside both eyes' crops at the headset FOV; 0.25 inward showed it in one eye only, 0.35 aims for both
  { name = "visor_menu",  root_field = "hud_root_visor_menu" },
  { name = "beam_menu",   root_field = "hud_root_beam_menu" },
  { name = "message",     root_field = "hud_root_message" },
  { name = "boss_energy", root_field = "hud_root_boss_energy" },   # Shows while an enemy bar is active
  # The map window's corner points (TOML inline tables must stay on one line); pulled inward like the radar
  { name = "minimap", kind = "corners", enter_hook = "minimap_update_enter", exit_hook = "minimap_update_exit", reg = 0, count_off = 0xe8, data_off = 0xf0, engaged_off = 0xf8, stride = 12, base_offset = [-0.35, 0.0] },
]
'@
    [IO.File]::WriteAllText($address.FullName, $text, (New-Object Text.UTF8Encoding($false)))
    if ($Hud.Count -gt 0) {
        $game = "$Profiles\$title\game.toml"
        $g = [IO.File]::ReadAllText($game).TrimEnd() + "`r`n`r`n[hud]`r`n" + ($Hud -join "`r`n") + "`r`n"
        [IO.File]::WriteAllText($game, $g, (New-Object Text.UTF8Encoding($false)))
    }
}

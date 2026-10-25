# SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# Launch steps for title 010012101468C000 (Metroid Prime Remastered), dot-sourced by launch.ps1.
# Mode C and every feature since are settings on a profile COPY (stage\stage-render.ps1, stage\stage-modec.ps1): the
# installed profile is only read. A step that differs by build takes -ForBuild (the launcher passes the build that runs).
. "$PSScriptRoot\..\stage\stage-render.ps1"
. "$PSScriptRoot\..\stage\stage-modec.ps1"

function Get-GameStagingSummary { 'stereo mode_c (staged profile copy)' }

function Invoke-GameStaging {
    param([Parameter(Mandatory)][string]$Profiles, [Parameter(Mandatory)][string]$Bin, [int]$ForBuild = [int]::MaxValue)
    # The game's own HUD at 100% in both eyes, no flat layers; resizing is in the mod menu's HUD sliders.
    [void](Set-StagedRender -GameToml "$Profiles\010012101468C000\game.toml" -Keys @('stereo = "mode_c"', 'scale = "native"'))
    # The game's own lock-on, as shipped: no scan_no_snap (the camera and body turn
    # to the target again), before Set-StagedModeC
    Remove-Item "$Profiles\010012101468C000\patches\67c9ca14\scan_no_snap.pchtxt" -ErrorAction SilentlyContinue
    # In the Scan Visor only, no camera or body turn toward a scan lock (combat lock-on
    # and the grapple keep the game's turn); before Set-StagedModeC installs it
    Copy-Item -Force "$PSScriptRoot\..\stage\b44\scan_snap_scan_only.pchtxt" "$Profiles\010012101468C000\patches\67c9ca14\"
    # The lock's facing test from the head (the address file's
    # [orbit_head] facing_buffer below); before Set-StagedModeC installs it
    Copy-Item -Force "$PSScriptRoot\..\stage\b45\orbit_facing_head.pchtxt" "$Profiles\010012101468C000\patches\67c9ca14\"
    # The left arm stays shown: the clip end no longer hides it; before
    # Set-StagedModeC installs it
    # (left_arm_on.pchtxt re-shows it after a grapple; left_arm_draw.pchtxt keeps the facing gate open)
    Copy-Item -Force "$PSScriptRoot\..\stage\b49\left_arm_hold.pchtxt" "$Profiles\010012101468C000\patches\67c9ca14\"
    # The scan window's viewport scale stays 1: the world renders full frame in the Scan, X-Ray and
    # Thermal visors (it shrank to a screen-centred rectangle away from the head-placed scan box)
    Copy-Item -Force "$PSScriptRoot\..\stage\b49\scan_viewport_full.pchtxt" "$Profiles\010012101468C000\patches\67c9ca14\"
    [void](Set-StagedModeC -Profiles $Profiles -Bin $Bin)
    # the core's menu labels are game-neutral: Prime's own words
    $labels = @(
        '', '[menu_labels]',
        'true_helmet = "True-helmet visor size (off: the game''s HUD size)"',
        'crouch_gesture = "Crouch to morph into the ball"',
        'hand_forward_m = "Cannon forward / back (m)"',
        'hand_up_m = "Cannon up / down (m)"',
        'hand_scale = "Cannon size"',
        'comfort_fade = "Fade when morphing"',
        'ball_view = "Morph ball in 3D (off: a 2D screen)"',
        'ball_vignette = "Morph ball vignette (tunnel while it rolls; 0 = off)"',
        '"tab.hands_arms" = "Cannon & arms"') -join "`r`n"
    $toml = "$Profiles\010012101468C000\game.toml"
    [IO.File]::WriteAllText($toml, [IO.File]::ReadAllText($toml).TrimEnd() + "`r`n" + $labels + "`r`n", (New-Object Text.UTF8Encoding($false)))
    Set-StagedHud -Profiles $Profiles               # HUD hooks; the HUD placement comes from the mod menu
    Set-StagedMenuMap -Profiles $Profiles           # in menus Touch A confirms and B goes back
    Set-StagedHandAim -Profiles $Profiles                # the cannon and aim on the right hand, snap turn, state-aware left trigger
    Set-StagedGestures -Profiles $Profiles                # head / cannon gestures, the visor ring, crouch = morph ball
    Set-StagedOwnedVisors -Profiles $Profiles                # the visor ring shows only owned visors
    Set-StagedRightArm -Profiles $Profiles -Bin $Bin      # the right arm follows the body (shoulder) and the hand (elbow)
    Set-StagedLeftArmAndBall -Profiles $Profiles -Bin $Bin -BallCamera   # morph fade, the game's visor dial, the left arm, shoulder sliders, the 3D ball
    Set-StagedCutscenes -Profiles $Profiles -Bin $Bin     # cutscenes in 3D, cut fades, no letterbox
    Set-StagedBody -Profiles $Profiles -Bin $Bin     # the body below the view
    Set-StagedLegs -Profiles $Profiles               # the legs
    Set-StagedGrapple -Profiles $Profiles               # the hand grapple
    Set-StagedStance -Profiles $Profiles               # the body's stance keys
    Set-StagedPassthrough -Profiles $Profiles   # passthrough by explicit state; the ball stays 3D
    # Targets are selected from the head: the candidate box's camera
    # register at 0xc7894c points at the head camera (enemy, scan and grapple lists, and their HUD icon queries),
    # and the lock's facing test reads the head's forward (stage\b45\orbit_facing_head.pchtxt). Movement, the body,
    # the camera and the gun keep the body's transform; a lock still turns the body toward the target, as in the game.
    $address = Get-ChildItem "$Profiles\010012101468C000" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $t = [IO.File]::ReadAllText($address.FullName)
    $t = $t -replace '(?m)^(orbit_idle = [^\r\n]*)', ("`$1`r`n" +
        'orbit_box = { module = "main", offset = 0xc7894c, expect = "60 03 40 F9 08 A1 41 BD 88 9F FF 97" }  # UpdateOrbitableObjects after mov x21, x0 (the candidate box camera)')
    if ($t -notmatch '(?m)^orbit_box = ') { throw 'staging: orbit_idle hook not found (orbit_box)' }
    $t = $t.TrimEnd() + "`r`n`r`n" + (@(
        '[orbit_head]   # the candidate box and the lock''s facing test from the head camera',
        'hook = "orbit_box"',
        'reg = 21',
        'buffer = "main+0x102af80"          # the head camera 3x4 (code-segment padding, zero in this build)',
        'facing_buffer = "main+0x102ae40"   # the head''s flat forward for orbit_facing_head.pchtxt, flag at +0x30',
        'facing_target_off = 0x4ec          # the lock target: a grapple point keeps the body''s facing'
    ) -join "`r`n") + "`r`n"
    [IO.File]::WriteAllText($address.FullName, $t, (New-Object Text.UTF8Encoding($false)))
    # A lock's body turn is subtracted from the view and handed back at the release
    # (lock_field: the body turns back to the player while the view stays); the lock ring and the locked scan
    # reticle at the target's depth, the grapple icons and scan indicators facing the head; the targets
    # around each ZL press logged
    $address = Get-ChildItem "$Profiles\010012101468C000" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $t = [IO.File]::ReadAllText($address.FullName)
    $t = $t -replace '(?m)^(facing_target_off = [^\r\n]*)', ("`$1`r`n" + (@(
        'lock_field = "player_orbit_state"  # PL+0x4e0: 1..4 = a scan, combat or grapple lock (the game turns the body)',
        'lock_min = 1',
        'lock_max = 4') -join "`r`n"))
    if ($t -notmatch '(?m)^lock_field = "player_orbit_state"') { throw 'staging: [orbit_head] not found (lock_field)' }
    $t = $t -replace '(?m)^(head_reticle_lrs = [^\r\n]*)', ("`$1`r`n" + (@(
        'head_reticle_target = { reg = 19, off = 0x10, margin = 0.75 }   # CReticleScanTarget: the locked scannable',
        'rotate_only_lrs = ["main+0xcb52cc", "main+0xe62f80"]   # grapple icons, scan indicators: already at the object') -join "`r`n"))
    if ($t -notmatch '(?m)^rotate_only_lrs = ') { throw 'staging: head_reticle_lrs not found (reticle depth)' }
    $t = $t.TrimEnd() + "`r`n`r`n" + (@(
        '[lock_ring]   # CReticleOrbitLock callers of the reticle hook: the ring at the locked target''s depth',
        'lrs = ["main+0xcc2c3c", "main+0xcc2c6c", "main+0xcc2cb8", "main+0xcc2cec", "main+0xcc2d38", "main+0xcc2d6c", "main+0xcc2dac", "main+0xcc2e4c"]',
        'target_off = 0x4ec',
        'distance = 1.25                         # the game''s cursor plane: the nearest the ring comes',
        'orbit_point = { reg = 21, off = 0x1c4 }  # the ring''s own point (GetOrbitPosition) in the ring at x21',
        'surface_margin = 0.75                   # in front of the target''s surface',
        '',
        '[grapple_aim.press_log]   # every ZL press: the lists, the nearest listed target, the visibility read list',
        'lists = [0x520, 0x530, 0x540]',
        'disable_off = 0x1094',
        'read_list_off = 0x16c8',
        'read_count_off = 0xa8',
        'read_data_off = 0xb0',
        'read_stride = 0x1c',
        'read_id_off = 0x0',
        'read_flags_off = 0x18'
    ) -join "`r`n") + "`r`n"
    [IO.File]::WriteAllText($address.FullName, $t, (New-Object Text.UTF8Encoding($false)))
    # The hands are placed before the tick's camera hook; the head camera they are
    # placed from moves (and outside a lock turns) with this tick's game camera and body
    $address = Get-ChildItem "$Profiles\010012101468C000" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $t = [IO.File]::ReadAllText($address.FullName)
    $t = $t.TrimEnd() + "`r`n`r`n" + (@(
        '[camera_now]   # CCameraManagerMP1 at the camera hook (x21); its first-person camera at the hand hook',
        'reg = 21',
        'camera_off = 0x80               # CFirstPersonCameraMP1*',
        'skip_counts = [0x18, 0x08]      # the cinematic camera lists: a cinematic camera is not resolved here',
        'chain = [0x8, 0x20, 0x40]       # CActorMP1::GetTransform: the world 3x4',
        'shake_off = 0x6c                # the camera shake (vec3, in the camera''s axes)',
        'max_error = 0.01                # units: the check against the camera the hook writes from',
        'lock_state_off = 0x4e0          # PL+0x4e0: outside a lock the body''s yaw is final at the hand hook'
    ) -join "`r`n") + "`r`n"
    [IO.File]::WriteAllText($address.FullName, $t, (New-Object Text.UTF8Encoding($false)))
    # The scan window's viewport scale on the Scan Visor's 1 s line
    # (expected 1 with scan_viewport_full, < 1 without); the left hand picks a grapple point within 25 deg (a headset
    # test: the left ray 9-30 deg off the grappled points, 4 of 25 presses under the old 12)
    $address = Get-ChildItem "$Profiles\010012101468C000" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $t = [IO.File]::ReadAllText($address.FullName)
    if ($t -notmatch '(?m)^max_angle_deg = [0-9.]+') { throw 'staging: max_angle_deg not found' }
    $t = $t -replace '(?m)^max_angle_deg = [0-9.]+[^\r\n]*', "max_angle_deg = 25.0            # the left hand's grapple pick (nearest to the ray wins)"
    # The left aim ray turned 15 deg right for the pick (a headset test: all 13
    # hand picks had the point right of the ray, +3.9 to +23.5 deg, mean about +15; up about 0 to +7)
    $t = $t -replace '(?m)^(max_angle_deg = [^\r\n]*)', ("`$1`r`n" + 'left_aim_yaw_offset_deg = -15.0  # the left aim ray''s yaw for the pick (positive = left)')
    if ($t -notmatch '(?m)^left_aim_yaw_offset_deg = ') { throw 'staging: left_aim_yaw_offset_deg not added' }
    $f = (@(
        'scan_viewport_x = { hook = "camera_write", base = "reg:x20 -> +0x4e0150", off = 0xb68, type = "f32" }   # logic+0xb68: min(hud scale, the visor''s viewport scale X)',
        'scan_viewport_y = { hook = "camera_write", base = "reg:x20 -> +0x4e0150", off = 0xb6c, type = "f32" }   # logic+0xb6c: Y'
    ) -join "`r`n")
    $t = $t -replace '(?m)^\[fields\][^\r\n]*', ("`$0`r`n" + $f.Replace('$', '$$'))
    $t = $t.TrimEnd() + "`r`n`r`n[scan_log]   # logged on the Scan Visor's 1 s line`r`nfields = [`"scan_viewport_x`", `"scan_viewport_y`"]`r`n"
    [IO.File]::WriteAllText($address.FullName, $t, (New-Object Text.UTF8Encoding($false)))
    # The left arm is solved only inside its draw windows: its
    # PreRender (which builds the pose the skin keeps; the arm transform is written there too) and its Render entry
    # (or the rain draw) to the return of its draw call. The tick-time solve (AdvanceEffects) keeps the game's pose.
    $address = Get-ChildItem "$Profiles\010012101468C000" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $t = [IO.File]::ReadAllText($address.FullName)
    $hooks = (@(
        'left_arm_rain = { module = "main", offset = 0xc7f23c, expect = "FF 43 02 D1 FD 7B 05 A9 FD 43 01 91 F7 33 00 F9" }  # DrawArmModelWithRain entry (no direct caller)',
        'left_draw_done_a = { module = "main", offset = 0xc7f47c, expect = "77 03 00 36 E0 03 16 AA BC 84 FF 97 E1 03 13 AA" }  # CGrappleArmMP1::Render after bl RenderGun (visors 0/2)',
        'left_draw_done_b = { module = "main", offset = 0xc7f3ec, expect = "B7 04 00 37 3E 00 00 14 20 03 40 F9 9A 86 FF 97" }  # CGrappleArmMP1::Render after bl CModelDataMP1::Render (visors 1/3)',
        'left_arm_prerender = { module = "main", offset = 0xc7f0ec, expect = "FF C3 02 D1 FD 7B 07 A9 FD C3 01 91 F7 43 00 F9" }  # CGrappleArmMP1::PreRender entry; x0 = arm',
        'left_prerender_done = { module = "main", offset = 0xc7f12c, expect = "08 2A 80 52 C8 09 A0 72 41 00 80 52 A8 6A 68 F8" }  # CGrappleArmMP1::PreRender after bl CAnimDataMP1::PreRender',
        'left_rain_done = { module = "main", offset = 0xc7f2f0, expect = "F4 4F 48 A9 F7 33 40 F9 F6 57 47 A9 FD 7B 45 A9" }  # DrawArmModelWithRain after bl RenderGun (one exit, no branches)'
        $(# The device parts from our elbow, captured right after our window solve
        'arm_compose = { module = "main", offset = 0xba09bc, expect = "68 06 40 B9 08 01 18 32 68 06 00 B9 68 52 80 79" }   # after the abs program; x19 = pose')
    ) -join "`r`n")
    $t = $t -replace '(?m)^\[hooks\][^\r\n]*', ("`$0`r`n" + $hooks)
    $t = $t.TrimEnd() + "`r`n`r`n" + (@(
        '[left_window]   # the left arm''s draw window (its Render entry, left_render_hook, always opens it)',
        'open_hooks = ["left_arm_rain"]',
        'close_hooks = ["left_draw_done_a", "left_draw_done_b", "left_prerender_done", "left_rain_done"]',
        'prerender_hook = "left_arm_prerender"',
        'prerender_flags_off = 0xa60     # the arm''s flags: bit0 active, bit5 hidden',
        'prerender_flags_mask = 0x21',
        'prerender_flags_equals = 0x1',
        'ik_window_only = true           # off: the IK in every solve (47''s)',
        'state_off = 0x980               # the arm state (9 = free look, shown; 10 = idle after the clip, hidden)',
        'shown_state = 9',
        'player_offs = [0x598, 0x4d4]    # PL+0x598 grappling, PL+0x4d4 morphed (the left_arm_on cave''s conditions)',
        'device_locals = [0x860, 0x890, 0x8c0, 0x8f0]   # the grapple device parts, then +0x8f0 (the beam''s base reset the clip end skips)',
        'arm_words = [0xa64]             # the clip end''s other skipped write (bl 0xb57760 on +0xa64)',
        'owned_field = "grapple_amount"  # PS+0x90: the Grapple Beam owned (> 0)',
        'device_write = true             # the device on the arm while not grappling',
        'device_skip_player_off = 0x598  # PL+0x598 != 0: grappling, the game places it',
        'device_abs_banks_off = 0x44     # *(u64)*(u64)(pose + 0x44) = the first abs bank (8-byte slots: 8 + dst x 8)',
        $('device_abs_slots = [0x4e8, 0x428, 0x428]   # one entry per written part (+0x860 / +0x890 / +0x8c0); with device_compose_hook below the parts are composed from our elbow, so these slots only set the part count'),
        'device_parent_slot = 0xc8       # L_elbow (dst 0x18), the locators'' parent',
        'device_fallback_local = [[0.0, 0.0, -1.0, 0.360, 0.0, 1.0, 0.0, -0.084, 1.0, 0.0, 0.0, 0.0], [0.0, 0.0, -1.0, 0.264, 0.0, 1.0, 0.0, -0.084, 1.0, 0.0, 0.0, 0.0], [0.0, 0.0, -1.0, 0.264, 0.0, 1.0, 0.0, -0.084, 1.0, 0.0, 0.0, 0.0]]   # grapLocator, gNozLoc1 x2 from the elbow (the CHPR bind pose; inferred, unproven on the rig)',
        $(# On every drawn frame outside a grapple, each part = our window solve's L_elbow x its
        # locator's rel CCoords x its legacy m (the game's GetLegacyTransform), so it follows the drawn arm.
        @('device_rel_ids = [0x0d21, 0x0e61]   # gNozLoc1 (bone 22, bank 1 +0x350), grapLocator (bone 26, bank 1 +0x3a0): logged on the rig only, not written',
          'device_compose_hook = "arm_compose"   # b52-rel-log: the abs slots at the Render entry are not our solve''s; compose from our elbow',
          'device_compose_pose_reg = 19',
          'device_elbow_id = 0x300             # L_elbow in the abs banks (bank 0 +0xc8)',
          'device_part_rel_ids = [0x0dc1, 0x0d21, 0x0d21]   # the parts at +0x860 / +0x890 / +0x8c0: UpperArmVis (GrappleGear), gNozLoc1 twice',
          'device_part_bind_ids = [0x12c0, 0x1080, 0x1080]  # their nodes in the legacy table: the game''s own GetNodeId results (b52-nodeid: gear 0x6212c0 = UpperArmVis +0x4b8, nozzles 0x621080 = gNozLoc1 +0x428)') -join "`r`n"),
        'probe = true                    # rig only: one frame''s solve order a second'
    ) -join "`r`n") + "`r`n"
    [IO.File]::WriteAllText($address.FullName, $t, (New-Object Text.UTF8Encoding($false)))
    # The game picks its own lock-on / grapple candidates; ZL is plain (held) in
    # every visor; the game's simulation step follows the headset's refresh ([tick_rate])
    $address = Get-ChildItem "$Profiles\010012101468C000" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $t = [IO.File]::ReadAllText($address.FullName)
    # The left hand picks the grapple point again (within its angle, from the game's list of points
    # in range); enemies and scan targets keep the game's own candidate (no hand pick for scans).
    $t = $t -replace '(?m)^(\[grapple_aim\][^\r\n]*)', ("`$1`r`n" + 'candidate_writes = true   # the left hand picks the grapple point; the rest is the game''s')
    if ($t -notmatch '(?m)^candidate_writes = true') { throw 'staging: [grapple_aim] not found (candidate_writes)' }
    if ($t -notmatch '(?m)^scan_max_angle_deg = [0-9.]+') { throw 'staging: scan_max_angle_deg not found' }
    $t = $t -replace '(?m)^scan_max_angle_deg = [0-9.]+[^\r\n]*', 'scan_max_angle_deg = 0.0         # no hand pick for scan targets (the game''s own)'
    # The game's own grapple-point list, for the menu's "Grapple pick: anywhere the hand
    # points"; the default pick stays among the points the game's view offers.
    $t = $t.TrimEnd() + "`r`n`r`n" + (@(
        '[grapple_aim.points]            # every loaded grapple point: list = *(*(GL+8)+0x4e0058), L = list+0x1e0'
        'list_ptr_off = 0x4e0058'
        'list_off = 0x1e0               # i32 bucket count, the bucket array at +8'
        'list_data_off = 0x8'
        'bucket_stride = 16             # bucket k = *(array + 16k)'
        'head_goc_off = 0x10            # its first GOC = *(bucket + 0x10)'
        'goc_next_off = 0xd8            # the next GOC'
        'goc_entity_off = 0x40          # the entity'
        'id_off = 0x10                  # entity: u32 id, area, active (bit 0)'
        'area_off = 0x14'
        'active_off = 0x18'
        'active_mask = 1'
        'player_area_off = 0x14         # the player''s area (the player is an entity of the same layout)'
        'min_range = 0.5                # the game''s own nearest grapple distance'
    ) -join "`r`n") + "`r`n"
    # The lock-on input path's log points: ZL held, ZL pressed this tick, the lock done.
    $t = $t -replace '(?m)^(orbit_idle = [^\r\n]*)', ("`$1`r`n" +
        'zl_held = { module = "main", offset = 0xc780ac, expect = "68 E2 44 B9 1F 15 00 71 20 07 00 36" }  # UpdateOrbitInput: w0 = ZL held' + "`r`n" +
        'zl_pressed = { module = "main", offset = 0xc780f4, expect = "20 35 00 36 61 1A 45 B9 E0 03 13 AA" }  # w0 = ZL pressed this tick' + "`r`n" +
        'zl_lockdone = { module = "main", offset = 0xc78154, expect = "68 E2 44 B9 1F 15 00 71 08 32 00 54" }  # after the lock: PL+0x4ec / +0x4e0')
    if ($t -notmatch '(?m)^zl_pressed = ') { throw 'staging: orbit_idle hook not found (zl trace hooks)' }
    $offs = 'player_offs = [0x4d4, 0x554, 0x518, 0x4e0, 0x4ec, 0x598]'
    $trace = @()
    foreach ($p in @(@('zl_held', 'held'), @('zl_pressed', 'pressed'), @('zl_lockdone', 'lock done'))) {
        $trace += '', '[[grapple_aim.zl_trace]]', "hook = `"$($p[0])`"", "label = `"$($p[1])`"", 'reg = 0', 'player_reg = 19', $offs
    }
    $t = $t.TrimEnd() + "`r`n" + ($trace -join "`r`n") + "`r`n"
    $dt = '28 11 91 52 08 91 A7 72'
    # The air damping's per-tick constant: the cave (main+0x102afc0: ldr s1, [pc+8];
    # b 0xcee3ec; .float 3.5*60/rate) is written before the branch to it (0xcee3e8).
    $air = $ForBuild -ge 42
    $airSites = if ($air) { ', 0x102afc0, 0xcee3e8' } else { '' }
    $airNote = if ($air) { ' + the air damping cave and its branch' } else { '' }
    $airOrig = if ($air) { ', "00 00 00 00 00 00 00 00 00 00 00 00", "01 90 21 1E"' } else { '' }
    $airK = @{ 72 = 'AB AA 3A 40'; 80 = '00 00 28 40'; 90 = '55 55 15 40'; 120 = '00 00 E0 3F'; 144 = 'AB AA BA 3F' }
    # The physics world's 64-bit pair of the same float: movz/movk w8 (52/72) become x8 (D2/F2), and the second
    # half repeats them at lsl 32/48 (hw + 2: byte 2 + 0x40). From a dt row "b0 b1 b2 52 b4 b5 b6 72".
    function World64([string]$row) {
        $b = $row -split ' '
        $hi = { param($x) '{0:X2}' -f ([Convert]::ToInt32($x, 16) + 0x40) }
        return (@($b[0], $b[1], $b[2], 'D2', $b[4], $b[5], $b[6], 'F2', $b[0], $b[1], (& $hi $b[2]), 'F2',
                  $b[4], $b[5], (& $hi $b[6]), 'F2') -join ' ')
    }
    $t = $t.TrimEnd() + "`r`n`r`n" + (@(
        '[tick_rate]   # the fixed simulation step (G7 loop): dt x4, the sim tick dt, the accumulator floor, 4 budgets, FPS',
        'module = "main"',
        'match_max = 90   # "Match headset" stays at 90: a fixed tick runs slow when frames dip (a 120 test run: 7% slow)',
        "sites = [0x3c819c, 0x3c81ac, 0x3c8230, 0xfe49c, 0x3c8304, 0x3c834c, 0x3c8628, 0x3c8640, 0x3c86e8, 0x3c8700, 0x3c81bc, 0xc11754$airSites]   # + the physics world step$airNote",
        "original = [`"$dt`", `"$dt`", `"$dt`", `"$dt`", `"3B 11 91 52 1B 91 A7 72`", `"28 11 91 52 08 91 B7 72`", `"48 08 8C 52 A8 8C A7 72`", `"48 08 8C 52 A8 8C A7 72`", `"48 08 8C 52 A8 8C A7 72`", `"48 08 8C 52 A8 8C A7 72`", `"08 4E A8 52`", `"$(World64 $dt)`"$airOrig]",
        '',
        '[tick_rate.rates]'
    ) -join "`r`n") + "`r`n"
    $rows = @(
        @(72, '28 C7 91 52 68 8C A7 72', '3B C7 91 52 7B 8C A7 72', '28 C7 91 52 68 8C B7 72', '88 B1 84 52 E8 87 A7 72', '08 52 A8 52'),
        @(80, 'A8 99 99 52 88 89 A7 72', 'BB 99 99 52 9B 89 A7 72', 'A8 99 99 52 88 89 B7 72', '28 06 81 52 88 85 A7 72', '08 54 A8 52'),
        @(90, '28 6C 81 52 C8 86 A7 72', '3B 6C 81 52 DB 86 A7 72', '28 6C 81 52 C8 86 B7 72', 'C8 5A 9D 52 08 83 A7 72', '88 56 A8 52'),
        @(120, '28 11 91 52 08 81 A7 72', '3B 11 91 52 1B 81 A7 72', '28 11 91 52 08 81 B7 72', '48 08 8C 52 A8 7C A7 72', '08 5E A8 52'),
        @(144, '28 C7 91 52 68 7C A7 72', '3B C7 91 52 7B 7C A7 72', '28 C7 91 52 68 7C B7 72', '88 B1 84 52 E8 77 A7 72', '08 62 A8 52'))
    foreach ($r in $rows) {
        $parts = @($r[1], $r[1], $r[1], $r[1], $r[2], $r[3], $r[4], $r[4], $r[4], $r[4], $r[5], (World64 $r[1]))
        if ($air) { $parts += @("41 00 00 1C 0A 0D F3 17 $($airK[[int]$r[0]])", 'F6 F2 0C 14') }
        $b = $parts | ForEach-Object { "`"$_`"" }
        $t += "$($r[0]) = [$($b -join ', ')]`r`n"
    }
    [IO.File]::WriteAllText($address.FullName, $t, (New-Object Text.UTF8Encoding($false)))
    $layout = "$Profiles\010012101468C000\layouts\prime-remastered.toml"
    $l = [IO.File]::ReadAllText($layout)
    $l = $l -replace '(?ms)^\[when\][^\r\n]*\r?\nleft_trigger = \[[^\]]*\]\r?\n', ''
    if ($l -match '(?m)^left_trigger = \[') { throw 'staging: the [when] left_trigger gate was not removed' }
    [IO.File]::WriteAllText($layout, $l, (New-Object Text.UTF8Encoding($false)))
    # head-tap cycling: every visor's "on" state and the ring's map of them
    $address = Get-ChildItem "$Profiles\010012101468C000" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $t = [IO.File]::ReadAllText($address.FullName)
    $t = $t -replace '(?m)^(scan_visor = \{ field = "visor_target", equals = 2 \}[^\r\n]*)', ("`$1`r`n" +
        'xray_visor = { field = "visor_target", equals = 1 }' + "`r`n" + 'thermal_visor = { field = "visor_target", equals = 3 }')
    if ($t -notmatch '(?m)^thermal_visor = ') { throw 'staging: scan_visor state not found (head-tap states)' }
    [IO.File]::WriteAllText($address.FullName, $t, (New-Object Text.UTF8Encoding($false)))
    $layout = "$Profiles\010012101468C000\layouts\prime-remastered.toml"
    $l = [IO.File]::ReadAllText($layout)
    $l = $l -replace '(?m)^(ring_owned = [^\r\n]*)', ("`$1`r`n" + 'ring_state = { up = "combat_visor", right = "scan_visor", down = "thermal_visor", left = "xray_visor" }')
    if ($l -notmatch '(?m)^ring_state = ') { throw 'staging: ring_owned not found (head-tap ring_state)' }
    [IO.File]::WriteAllText($layout, $l, (New-Object Text.UTF8Encoding($false)))
    # last, after every other step: the only fields a launch-only grant may write (-GrantMorphBall)
    $address = Get-ChildItem "$Profiles\010012101468C000" -Filter '67c9ca14*.toml' | Select-Object -First 1
    $grants = "`r`n[session_grants]`r`nfields = [`"power_morph_amount`", `"power_morph_capacity`"]`r`n"
    [IO.File]::WriteAllText($address.FullName, [IO.File]::ReadAllText($address.FullName).TrimEnd() + "`r`n" + $grants, (New-Object Text.UTF8Encoding($false)))
}

function Show-GameControls {
    Write-Host '  1. Left stick click = recenter the view.  Left stick = move, right stick = turn.'
    Write-Host '  2. Right trigger = shoot (hold = charge), right grip = missile, A = jump, B = map, X = morph ball, Y = swap select (hold), left trigger = lock-on (hold it; in the Scan Visor it scans).'
    Write-Host '  3. Left menu = pause.  Visors and beams: see the hand gestures below (the flick counts when the stick springs back).'
    Write-Host '  In menus (title, pause, map): A = confirm, B = back.'
    Write-Host '  AIM: the cannon is your right hand: aim where you point, the reticle sits where the shot lands. Right stick = 30 deg snap turn (angle in the mod menu). Hold the left trigger to lock on or grapple, as in the game.'
    Write-Host '  VISOR = hold your left hand on your head (buzz, a ring shows), flick the left stick to pick (up Combat, right Scan, left X-Ray, down Thermal); the visor switches when you take the hand away and is remembered. A quick TAP on the head goes to the next visor you own (Combat, Scan, Thermal, X-Ray, then back to Combat). The Scan Visor aims with your head. BEAM = left hand on the right controller (buzz on both) + flick (up Power, right Wave, down Ice, left Plasma). CROUCH (about half a metre) = morph ball, stand up = back out; X still works. Crouch on/off and depth are in the mod menu.'
    Write-Host '  4. Click BOTH sticks together and hold a moment = mod menu (a buzz confirms) (HUD size/place per element, world scale): point with the right hand, right trigger = drag a slider, B or both sticks again = close. Your settings are kept for next time.'
}

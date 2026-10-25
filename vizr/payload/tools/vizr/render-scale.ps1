# SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# VIZR M5: Eden's resolution scale for a headset launch, set before Eden starts (Eden reads it at boot).
# Dot-sourced by launch.ps1 and boot-eden.ps1.
# game.toml [render] scale = "native" (default: the smallest Eden scale whose 1080-line docked frame reaches the
# headset's recommended per-eye height, M5-NOTES section 5) or a number (1, 1.5, 2, 3: an explicit choice, logged).
function Set-EdenRenderScale {
    param(
        [int]$EyeHeight,   # the runtime's recommended per-eye height (0 = unknown: Quest 3's 2208)
        [Parameter(Mandatory)][string]$Profiles,
        [string]$Title = '010012101468C000',
        [Parameter(Mandatory)][string]$Config,  # the build's qt-config.ini
        [string]$Force = ''   # the player's Compatibility tab render scale (1, 1.5, 2, 3); wins over game.toml
    )
    if ($EyeHeight -le 0) { $EyeHeight = 2208 }
    # [render] scale from game.toml (a plain line read; the file is TOML but this key is a scalar).
    $want = 'native'
    $inRender = $false
    foreach ($line in Get-Content "$Profiles\$Title\game.toml" -ErrorAction SilentlyContinue) {
        if ($line -match '^\s*\[(.+)\]') { $inRender = $Matches[1].Trim() -eq 'render'; continue }
        if ($inRender -and $line -match '^\s*scale\s*=\s*"?([^"#\s]+)"?') { $want = $Matches[1] }
    }
    if ($Force) { $want = $Force }
    # Eden's ResolutionSetup enum index per scale (settings_enums.h).
    $setup = @{ '1' = 3; '1.5' = 5; '2' = 6; '3' = 7 }
    if ($want -eq 'native') {
        $scale = @('1', '1.5', '2', '3') | Where-Object { 1080 * [double]$_ -ge 0.95 * $EyeHeight } | Select-Object -First 1
        if (-not $scale) { $scale = '3' }
    } elseif ($setup.ContainsKey($want)) {
        $scale = $want
    } else {
        return "render scale: game.toml [render] scale '$want' is not native/1/1.5/2/3; Eden's setting left as is"
    }
    $text = Get-Content $Config -Raw
    $text = $text -replace '(?m)^resolution_setup\\default=.*$', 'resolution_setup\default=false'
    $text = $text -replace '(?m)^resolution_setup=.*$', "resolution_setup=$($setup[$scale])"
    [IO.File]::WriteAllText($Config, $text, (New-Object Text.UTF8Encoding($false)))  # no BOM: Qt reads the first group name
    return "render scale ${scale}x ($want; per-eye height $EyeHeight, frame $([int](1080 * [double]$scale)) lines)"
}

# The scale cap for this launch, from the last headset session's log. The runtime's recommended size is used unless
# the GPU provably cannot hold it (a GPU can hold 59/s on average yet fall below 58/s for 3+ seconds at a time).
# The first 90 s of 3D are ignored (cold shader-cache stalls are not GPU headroom); 2 or more separate runs of 3+ consecutive 3D seconds below 58 game frames/s in the
# rest = one step below the scale that session ran (3 -> 2 -> 1.5 -> 1, never below 1); a session that ran capped and
# held the rate keeps its cap (no automatic step back up). Returns @{ Cap = <scale or 0 = none>; Why = <text> }.
function Get-RenderScaleCap([Parameter(Mandatory)][string]$Logs) {
    $steps = @(1.0, 1.5, 2.0, 3.0)
    $dirs = Get-ChildItem $Logs -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^\d{4}-\d\d-\d\d-session\d+$' } |
        Sort-Object LastWriteTime -Descending
    foreach ($dir in $dirs) {
        $log = "$($dir.FullName)\eden_log.txt"
        if (-not (Test-Path $log)) { continue }
        $scale = 0.0; $capped = 0.0; $fps = New-Object System.Collections.Generic.List[int]
        foreach ($m in (Select-String -Path $log -Pattern 'render scale (\S+) from runtime per-eye', 'render scale \S+ capped at (\S+) by the launcher', '1s: published=(\d+) .*?projection=(\d+)')) {
            $line = $m.Line
            if ($line -match 'render scale (\S+) from runtime per-eye') { $scale = [double]$Matches[1] }
            elseif ($line -match 'capped at (\S+) by the launcher') { $capped = [double]$Matches[1] }
            elseif ($line -match '1s: published=(\d+) .*?projection=(\d+)' -and [int]$Matches[2] -gt 0) { $fps.Add([int]$Matches[1]) }
        }
        if ($fps.Count -lt 120) { continue }  # under 2 minutes of 3D: not enough to judge the GPU
        $ran = if ($capped -gt 0) { $capped } else { $scale }
        $run = 0; $runs = 0
        $graded = @($fps | Select-Object -Skip 90)   # one line per second of 3D: skip the first 90 s
        foreach ($f in $graded + 99) { if ($f -lt 58) { $run++ } else { if ($run -ge 3) { $runs++ }; $run = 0 } }
        if ($runs -ge 2 -and $ran -gt 1.0) {
            $lower = ($steps | Where-Object { $_ -lt $ran } | Select-Object -Last 1)
            return @{ Cap = $lower; Why = "$($dir.Name) ran ${ran}x with $runs run(s) of 3+ s below 58/s: cap ${lower}x" }
        }
        if ($capped -gt 0) { return @{ Cap = $capped; Why = "$($dir.Name) ran capped at ${capped}x and held 58/s: cap kept" } }
        return @{ Cap = 0; Why = "$($dir.Name) held 58/s at ${ran}x ($runs run(s) of 3+ s below 58/s after the first 90 s): no cap" }
    }
    return @{ Cap = 0; Why = 'no earlier headset session: no cap' }
}

# The recommended per-eye height the last headset session logged (Eden log line "recommended per-eye WxH").
function Get-LastEyeHeight([Parameter(Mandatory)][string]$Logs) {
    # The newest log may be a launcher test without the headset (no per-eye line), so
    # this returned 0 and the launch fell back to Quest 3's 2208 (2x instead of 3x on VD). Use the newest log that
    # HAS the line; session folders whose log was not saved, and test runs without the headset, are skipped.
    # Only real launches (folders named <date>-session<N>) count; a test folder never gives a hint.
    $dirs = Get-ChildItem $Logs -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^\d{4}-\d\d-\d\d-session\d+$' } |
        Sort-Object LastWriteTime -Descending
    foreach ($dir in $dirs) {
        foreach ($name in 'eden_log.txt', 'eden_log.prev.txt') {
            $log = "$($dir.FullName)\$name"
            if (-not (Test-Path $log)) { continue }
            $m = Select-String -Path $log -Pattern 'recommended per-eye (\d+)x(\d+)' | Select-Object -Last 1
            if ($m) { return [int]$m.Matches[0].Groups[2].Value }
        }
    }
    return 0
}

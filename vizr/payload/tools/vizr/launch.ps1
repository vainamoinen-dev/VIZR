# SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# VIZR launcher: start one game in VR on the VIZR Eden build.
# - Checks the system OpenXR runtime (read only; prints it). Virtual Desktop as the runtime needs its Streamer running;
#   any other runtime (SteamVR, Meta/Oculus, ...) is used as it is; no runtime = a plain refusal.
# - Uses the system runtime: never sets XR_RUNTIME_JSON or VIZR_NO_VR, and clears test variables inherited from a shell.
# - The game: -Game, else the first line of <root>\game\GAME.txt (written by the installer). The title id: -Title, else
#   the [0100...] in the file name, else the second line of GAME.txt, else the only profile installed.
# - Per-game launch steps live in tools\vizr\games\<title>.ps1: Invoke-GameStaging (optional: settings applied to a
#   profile COPY in the session folder, never the installed profile) and Show-GameControls.
# - Logs to <logs>\<date>-session<N>\: the Eden log (line 1 proves the build), the profile files used, launch.txt.
param(
    [string]$Root = '',            # the install folder (default: VIZR_ROOT, else two folders above this script)
    [string]$Bin = '',             # Eden's bin folder (default: the only build\<dir>\bin under the root)
    [string]$Game = '',
    [string]$Title = '',
    [string]$Logs = '',            # default: <root>\logs
    [string]$Hints = '',           # where the last per-eye size is read from (default: the logs folder)
    [string]$Session = '',         # a named session folder (default: <date>-<prefix>session<N>)
    [string]$SessionPrefix = '',
    [string]$Label = '',           # what runs, for the first line (e.g. "test launch")
    [int]$ForBuild = 0,            # the build that runs, for staging that differs by build (0 = the current one)
    [string]$Save = '',            # optional: a stored save folder under <root>\game\saves to start from
    [string]$BootDriver = '',      # optional: the key-press helper for a stored save's BOOT.txt route
    [string[]]$EnvAlias = @(),     # optional: extra prefixes that also get the VIZR_* settings below (older builds)
    [string]$SessionWrites = '',   # this launch only: profile field writes "field=value,..." (e.g. a granted item; kept if saved)
    [switch]$DryRun,               # everything up to starting Eden (checks, staging, settings, launch.txt), then stop;
                                   # a missing VR runtime is reported, not fatal
    [switch]$KeepTestEnv,          # automated tests only: keep the caller's test runtime and capture settings
    [string]$SaveBackups = ''      # where the save backup of every start goes (default: <root>\save-backups; '' off for tests)
                                   # (XR_RUNTIME_JSON, VIZR_NO_VR, VIZR_XR_RUNTIME_ALLOW, VIZR_RIG_OUT)
)

$Root = if ($Root) { $Root.TrimEnd('\') } elseif ($env:VIZR_ROOT) { $env:VIZR_ROOT.TrimEnd('\') } else { Split-Path (Split-Path $PSScriptRoot) }
$VIZRRoot = $Root   # the staging scripts read their root from here
function Stop-Launch($why) { Write-Host "`nNOT LAUNCHED: $why" -ForegroundColor Red; exit 1 }

if (-not $Bin) {
    $found = @(Get-ChildItem "$Root\build" -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path "$($_.FullName)\bin\eden.exe" })
    if ($found.Count -ne 1) { Stop-Launch "expected one Eden build under $Root\build, found $($found.Count)" }
    $Bin = "$($found[0].FullName)\bin"
}
$bin = $Bin
# PowerShell names ignore case: the -Game argument and $game are one variable, so read the argument first.
$gameTxt = "$Root\game\GAME.txt"
$gameLines = @(if (Test-Path $gameTxt) { Get-Content $gameTxt -TotalCount 2 | ForEach-Object { $_.Trim() } })
if (-not $Game -and $gameLines.Count -ge 1) { $Game = $gameLines[0] }
if (-not $Game) { Stop-Launch "no game named: run the installer again, or pass -Game <file>" }
$game = $Game
$profiles = "$Root\profiles"
if (-not $Title) {
    if ($game -match '\[([0-9A-Fa-f]{16})\]') { $Title = $Matches[1] }
    elseif ($gameLines.Count -ge 2 -and $gameLines[1] -match '^[0-9A-Fa-f]{16}$') { $Title = $gameLines[1] }
    else {
        $installed = @(Get-ChildItem $profiles -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^[0-9A-Fa-f]{16}$' })
        if ($installed.Count -eq 1) { $Title = $installed[0].Name }
    }
}
if (-not $Title) { Stop-Launch "cannot tell which game $game is (no title id); run the installer again" }
$title = $Title.ToUpper()
if (-not (Test-Path "$profiles\$title\game.toml")) { Stop-Launch "no profile for title $title in $profiles" }
$gameScript = "$PSScriptRoot\games\$title.ps1"
if (Test-Path $gameScript) { . $gameScript }

if (-not $Logs) { $Logs = "$Root\logs" }
if (-not $Hints) { $Hints = $Logs }
$logs = $Logs
$staging = [bool](Get-Command Invoke-GameStaging -ErrorAction SilentlyContinue)

# Session folder (created further down, after the checks).
$date = Get-Date -Format 'yyyy-MM-dd'
if (-not $Session) {
    $n = 1
    while (Test-Path "$logs\$date-$($SessionPrefix)session$n") { $n++ }
    $Session = "$date-$($SessionPrefix)session$n"
}
$out = "$logs\$Session"
$labelText = if ($Label) { "$Label, " } else { '' }
$what = if ($staging) { "$labelText$(Get-GameStagingSummary), profiles $out\profiles-run" }
        else { "$labelText$(Split-Path -Leaf $game), profile $profiles\$title (as it is)" }
Write-Host "Launch: $what" -ForegroundColor Cyan

if (Get-Process -Name 'eden' -ErrorAction SilentlyContinue) { Stop-Launch 'Eden is already running.' }
if (-not (Test-Path "$bin\eden.exe")) { Stop-Launch "no build at $bin" }

# Which OpenXR runtime will Eden use?
$active = (Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime
$vdRunning = [bool](Get-Process -Name 'VirtualDesktop.Streamer' -ErrorAction SilentlyContinue)
Write-Host "OpenXR runtime (system): $active"
Write-Host "Virtual Desktop Streamer running: $vdRunning"
if ($KeepTestEnv -and $env:XR_RUNTIME_JSON) {
    Write-Host "TEST RUNTIME: XR_RUNTIME_JSON=$env:XR_RUNTIME_JSON (the system runtime is not used)" -ForegroundColor Yellow
} elseif ($DryRun -and (-not $active -or -not (Test-Path $active) -or ($active -match 'Virtual Desktop' -and -not $vdRunning))) {
    Write-Host 'DRY RUN: the VR runtime check would stop a real launch here; continuing.' -ForegroundColor Yellow
} elseif (-not $active -or -not (Test-Path $active)) {
    Stop-Launch 'no VR runtime is set up on this PC (no active OpenXR runtime). Install and start Virtual Desktop, SteamVR or the Oculus/Meta app, then try again.'
}
if (-not $DryRun -and -not ($KeepTestEnv -and $env:XR_RUNTIME_JSON) -and $active -match 'Virtual Desktop' -and -not $vdRunning) { Stop-Launch 'Virtual Desktop Streamer is not running. Start it and connect the headset first.' }
if (-not (Test-Path -LiteralPath $game)) { Stop-Launch "the game file is not there: $game" }
# An installed copy loads every NSP in its game\ folder (Eden's game list below). The profiles are written for the base
# game's program: an update replaces it, no profile matches, and the VR layer would not load.
if (Test-Path $gameTxt) {
    $gameFull = (Resolve-Path -LiteralPath $game).Path
    $updates = @(Get-ChildItem -LiteralPath "$Root\game" -File -Filter *.nsp -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -ne $gameFull })
    if ($updates) {
        Stop-Launch "VIZR supports the base game (v1.0.0) without updates; remove the update from $Root\game ($($updates.Name -join ', '))"
    }
}

# Never inherit test settings: they would redirect or refuse the runtime.
$testVars = if ($KeepTestEnv) { @('VIZR_RIG_WRITES', 'VIZR_RIG_CAM_BACK', 'VIZR_LAG_PROBE') }
            else { @('XR_RUNTIME_JSON', 'VIZR_RIG_OUT', 'VIZR_RIG_WRITES', 'VIZR_RIG_CAM_BACK', 'VIZR_LAG_PROBE', 'VIZR_XR_RUNTIME_ALLOW') }
foreach ($v in $testVars) { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
if ($SessionWrites) { $env:VIZR_SESSION_WRITES = $SessionWrites } else { Remove-Item Env:VIZR_SESSION_WRITES -ErrorAction SilentlyContinue }
if ($env:VIZR_NO_VR) { Write-Host "TEST MODE: VIZR_NO_VR=$env:VIZR_NO_VR is set, so Eden will refuse the runtime." -ForegroundColor Yellow }

# Every session keeps its Eden log. The log is copied when Eden exits while this window is open; if the window was
# closed first, the next launch recovers it here from the build the last session ran (named in its launch.txt),
# before this launch starts Eden and overwrites it.
$last = Get-ChildItem $logs -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^\d{4}-\d\d-\d\d-([0-9a-z]+-)?session\d+$' } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($last -and -not (Test-Path "$($last.FullName)\eden_log.txt")) {
    $ran = Get-Content "$($last.FullName)\launch.txt" -ErrorAction SilentlyContinue | Where-Object { $_ -like 'eden *' } | Select-Object -First 1
    $started = Get-Content "$($last.FullName)\launch.txt" -ErrorAction SilentlyContinue | Where-Object { $_ -like 'launched *' } | Select-Object -First 1
    if ($ran -and $started) {
        $lastLog = Join-Path (Split-Path $ran.Substring(5)) 'user\log\eden_log.txt'
        $since = [datetime]::Parse($started.Substring(9))
        if ((Test-Path $lastLog) -and (Get-Item $lastLog).LastWriteTime -ge $since) {
            Copy-Item $lastLog "$($last.FullName)\eden_log.txt"
            "eden_log.txt recovered by the next launch ($(Get-Date -Format s))" | Add-Content "$($last.FullName)\launch.txt"
            Write-Host "Saved the last session's Eden log to $($last.Name) (its window was closed before Eden)."
        }
    }
}

# Session folder.
New-Item -ItemType Directory -Force "$out\profile\layouts", "$out\profile\patches" | Out-Null

# The game's launch settings go onto a profile COPY in the session folder; the installed profile is only read.
if ($staging) {
    $staged = "$out\profiles-run"
    Copy-Item -Recurse $profiles $staged
    Invoke-GameStaging -Profiles $staged -Bin $bin -ForBuild $(if ($ForBuild -gt 0) { $ForBuild } else { [int]::MaxValue })
    $profiles = $staged
}
# The player's settings live in the INSTALLED profile folder, so placements survive every session. A named test session
# (-Session) keeps its own in the session folder: it never reads or writes the player's.
$userToml = if ($PSBoundParameters.ContainsKey('Session')) { "$out\user.toml" } else { "$Root\profiles\$title\user.toml" }
$env:VIZR_USER_TOML = $userToml
$env:VIZR_PROFILES = $profiles
# Eden's portable config names its folders absolutely; point them at this binary's user dir (an install moved
# anywhere keeps working). Where they already point there, this rewrites the same values.
$qt = "$bin\user\config\qt-config.ini"
if (Test-Path $qt) {
    $u = ($bin -replace '\\', '/') + '/user'
    $text = [IO.File]::ReadAllText($qt)
    $text = [regex]::Replace($text, '(?m)^(nand|sdmc|load|dump|tas)_directory=.*$', { param($m) "$($m.Groups[1].Value)_directory=$u/$($m.Groups[1].Value)" })
    $text = [regex]::Replace($text, '(?m)^Screenshots\\screenshot_path=.*$', "Screenshots\screenshot_path=$u/screenshots")
    # An installed copy (game\GAME.txt) also lists its game\ folder, so an update NSP there is loaded.
    if ((Test-Path $gameTxt) -and $text -notmatch '(?m)^Paths\\gamedirs\\4\\path=' -and $text -match '(?m)^Paths\\gamedirs\\size=3\s*$') {
        $gameDir = ("$Root\game" -replace '\\', '/')
        $text = [regex]::Replace($text, '(?m)^Paths\\gamedirs\\size=3\s*$', 'Paths\gamedirs\size=4')
        $add = "Paths\gamedirs\4\path=$gameDir`r`nPaths\gamedirs\4\deep_scan=false`r`nPaths\gamedirs\4\expanded=true"
        $text = [regex]::Replace($text, '(?m)^(Paths\\gamedirs\\3\\expanded=.*)$', { param($m) $m.Groups[1].Value + "`r`n" + $add })
    }
    [IO.File]::WriteAllText($qt, $text, (New-Object Text.UTF8Encoding($false)))
}

# Every start: a copy of the game saves (bin\user\nand\user\save) in <root>\save-backups\<session>, the newest 10 kept.
# Outside the logs folder, so an uninstall keeps them with the saves. Automated tests (-KeepTestEnv) make none.
$backupLine = 'no save backup (automated test)'
if (-not $KeepTestEnv) {
    $backupRoot = if ($SaveBackups) { $SaveBackups.TrimEnd('\') } else { "$Root\save-backups" }
    $saves = "$bin\user\nand\user\save"
    if (-not (Test-Path -LiteralPath $saves)) {
        $backupLine = 'no saves yet: nothing to back up'
    } elseif ($DryRun) {
        $backupLine = "would back up $saves to $backupRoot\$Session (dry run)"
    } else {
        New-Item -ItemType Directory -Force "$backupRoot\$Session" | Out-Null
        & robocopy $saves "$backupRoot\$Session" /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
        if ($LASTEXITCODE -ge 8) { Stop-Launch "the save backup to $backupRoot\$Session failed (robocopy $LASTEXITCODE); your saves are untouched" }
        $kept = @(Get-ChildItem -Directory -LiteralPath $backupRoot | Sort-Object LastWriteTime -Descending)
        $kept | Select-Object -Skip 10 | ForEach-Object { Remove-Item -Recurse -Force -LiteralPath $_.FullName }
        $files = @(Get-ChildItem -Recurse -File -LiteralPath "$backupRoot\$Session").Count
        $backupLine = "$files files backed up to $backupRoot\$Session (the newest 10 backups are kept)"
    }
}
Write-Host "Save backup: $backupLine"
# Optional: start from a stored save (the slot's current files are backed up to the session folder first).
$saveLine = 'no stored save; save slot left as is'
if ($Save) {
    # Stored saves are a development feature: the public install has no save-fixture.ps1.
    if (-not (Test-Path "$PSScriptRoot\save-fixture.ps1")) { Stop-Launch "-Save needs tools\vizr\save-fixture.ps1, which this install does not include" }
    if ($DryRun) {
        $saveLine = "would install the stored save '$Save' (dry run: the save slot is left as is)"
    } else {
        . "$PSScriptRoot\save-fixture.ps1"
        $saveLine = Install-SaveFixture -Name $Save -BackupDir $out -UserDir "$bin\user" -Title $title
    }
}
Write-Host "Save: $saveLine"
# The player's Compatibility settings (user.toml [compat]; 0 or missing = automatic).
$compat = @{ render_scale = 0.0; shaders = 0.0; gpu_accuracy = 0.0 }
if (Test-Path $userToml) {
    $inCompat = $false
    foreach ($line in [IO.File]::ReadAllLines($userToml)) {
        if ($line -match '^\s*\[(.+)\]') { $inCompat = $Matches[1].Trim() -eq 'compat'; continue }
        if ($inCompat -and $line -match '^\s*(render_scale|shaders|gpu_accuracy)\s*=\s*([0-9.]+)') { $compat[$Matches[1]] = [double]$Matches[2] }
        if ($inCompat -and $line -match '^\s*game_fps\s*=\s*([0-9.]+)') { $gameFps = [double]$Matches[1] }
    }
}
# The Game frame rate setting as the player left it (the core reads the same key; missing = its default, 60).
$gameFpsLine = if ($null -eq $gameFps) { 'not set (the default: 60)' }
               elseif ($gameFps -lt 0.5) { '0 (match the headset, up to the profile''s cap)' } else { "$gameFps" }
# Eden's resolution scale for the headset's per-eye size (the last session's logged size; Quest 3 if none).
. "$PSScriptRoot\render-scale.ps1"
$forced = if ($compat.render_scale -ge 1.0) { "$($compat.render_scale)" } else { '' }
$scaleLine = Set-EdenRenderScale -EyeHeight (Get-LastEyeHeight $Hints) -Profiles $profiles -Title $title -Config $qt -Force $forced
# Shader compile and GPU accuracy (Eden settings, read at boot): the player's choice, or "automatic" = Eden's own default
# (\default=true). Written every launch, so going back to automatic undoes an earlier choice (the round trip).
$compatLine = @()
$text = [IO.File]::ReadAllText($qt)
if ($compat.shaders -ge 1) {
    $async = if ($compat.shaders -eq 1) { 'true' } else { 'false' }
    $text = $text -replace '(?m)^use_asynchronous_shaders\\default=.*$', 'use_asynchronous_shaders\default=false'
    $text = $text -replace '(?m)^use_asynchronous_shaders=.*$', "use_asynchronous_shaders=$async"
    $compatLine += "shaders $(if ($async -eq 'true') { 'smooth (async)' } else { 'exact' })"
} else {
    $text = $text -replace '(?m)^use_asynchronous_shaders\\default=.*$', 'use_asynchronous_shaders\default=true'
}
if ($compat.gpu_accuracy -ge 1) {
    $acc = if ($compat.gpu_accuracy -eq 1) { 0 } else { 1 }   # Eden GpuAccuracy: 0 Low (our "Normal"), 1 High
    $text = $text -replace '(?m)^gpu_accuracy\\default=.*$', 'gpu_accuracy\default=false'
    $text = $text -replace '(?m)^gpu_accuracy=.*$', "gpu_accuracy=$acc"
    $compatLine += "gpu accuracy $(if ($acc -eq 0) { 'normal' } else { 'high' })"
} else {
    $text = $text -replace '(?m)^gpu_accuracy\\default=.*$', 'gpu_accuracy\default=true'
}
[IO.File]::WriteAllText($qt, $text, (New-Object Text.UTF8Encoding($false)))
$compatText = if ($compatLine.Count) { $compatLine -join ', ' } else { 'automatic' }
Write-Host "Compatibility: $compatText"
Write-Host "Render: $scaleLine"
# With scale "native" Eden itself sets the scale from the runtime's per-eye size at XR start; the line above is only
# the pre-boot default. Eden's own line goes into launch.txt after the session (render-final).
if ($scaleLine -like '*(native;*') { $env:VIZR_RENDER_SCALE = 'native' } else { Remove-Item Env:VIZR_RENDER_SCALE -ErrorAction SilentlyContinue }
# A cap on Eden's native pick after a session that could not hold the game's frame rate.
# The player's Compatibility tab (user.toml [compat]) wins over the automatic choices.
if ($compat.render_scale -ge 1.0) {
    Remove-Item Env:VIZR_RENDER_SCALE_MAX -ErrorAction SilentlyContinue
    $capLine = "none (user override: render scale $($compat.render_scale)x)"
} else {
    $cap = Get-RenderScaleCap $Hints
    if ($cap.Cap -gt 0) { $env:VIZR_RENDER_SCALE_MAX = "$($cap.Cap)" } else { Remove-Item Env:VIZR_RENDER_SCALE_MAX -ErrorAction SilentlyContinue }
    $capLine = $cap.Why
}
Write-Host "Render cap: $capLine"
foreach ($prefix in $EnvAlias) {
    foreach ($key in 'USER_TOML', 'PROFILES', 'RENDER_SCALE', 'RENDER_SCALE_MAX') {
        $value = [Environment]::GetEnvironmentVariable("VIZR_$key")
        [Environment]::SetEnvironmentVariable("$($prefix)_$key", $value)   # an empty value removes it
    }
}
Copy-Item "$profiles\$title\*.toml" "$out\profile\" -Force
Copy-Item "$profiles\$title\layouts\*" "$out\profile\layouts\" -Force -ErrorAction SilentlyContinue
Copy-Item "$profiles\$title\patches\*" "$out\profile\patches\" -Recurse -Force -ErrorAction SilentlyContinue

# vr_output must be ON (Eden setting; default ON).
$vr = Select-String -Path $qt -Pattern '^vr_output=(.*)$' -ErrorAction SilentlyContinue | ForEach-Object { $_.Matches[0].Groups[1].Value }
if ($vr -and $vr -ne 'true') { Stop-Launch "Eden's VR headset output setting is off (vr_output=$vr). Turn it on in Settings > Graphics." }

@(
    "launch $what",
    "launched $(Get-Date -Format s)",
    "runtime $active (VD Streamer running: $vdRunning)",
    "eden $bin\eden.exe",
    "profiles $profiles (fov mode headset, Touch layout from game.toml [input])",
    "VIZR_NO_VR=$env:VIZR_NO_VR",
    "save backup $backupLine",
    "save $saveLine",
    "render $scaleLine",
    "render-cap $capLine",
    "compat $compatText",
    "game-fps $gameFpsLine (the tick in use: the eden_log 'game tick set to' lines, copied below after the session)",
    "session grant $(if ($SessionWrites) { "$SessionWrites (this launch only; kept if the game is saved)" } else { 'none' })",
    "patches $((@(Get-ChildItem "$bin\user\load\*\*\exefs\*.pchtxt" -ErrorAction SilentlyContinue | ForEach-Object Name | Sort-Object)) -join ', ')"
) | Set-Content "$out\launch.txt"

Write-Host ''
Write-Host 'WHAT TO PRESS (Quest Touch buttons; other controllers: README.md, "Controllers", maps them)' -ForegroundColor Cyan
if (Get-Command Show-GameControls -ErrorAction SilentlyContinue) { Show-GameControls }
else { Write-Host '  Left stick click = recenter the view. The game''s own buttons follow the profile''s Touch layout.' }
Write-Host ''
Write-Host "Logs: $out"

if ($DryRun) { "dry-run: Eden not started" | Add-Content "$out\launch.txt"; Write-Host "DRY RUN: stopped before starting Eden. Logs: $out"; exit 0 }
$log = "$bin\user\log\eden_log.txt"
$p = Start-Process -FilePath "$bin\eden.exe" -ArgumentList '-g', "`"$game`"" -PassThru
# The session keeper (hidden, tied to Eden, not to this window): gpu.csv while Eden runs, then Eden's log and the
# closing lines of launch.txt, even when this window is closed first.
$keeper = Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', "`"$PSScriptRoot\session-keeper.ps1`"", '-Out', "`"$out`"", '-EdenLog', "`"$log`"", '-EdenPid', $p.Id
"session keeper $(if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) { 'with the gpu sampler (nvidia-smi 1 Hz -> gpu.csv)' } else { 'without a gpu sampler (nvidia-smi not found)' })" |
    Add-Content "$out\launch.txt"
Write-Host 'Eden is running. Close Eden when you are done; the log is saved either way.'
# A stored save with a BOOT.txt route: press through the title menus to the save point.
if ($Save -and $BootDriver -and (Test-Path "$Root\game\saves\$Save\BOOT.txt")) {
    Write-Host 'Pressing through the menus to the save point. Leave the controllers alone until the game starts.' -ForegroundColor Yellow
    . $BootDriver
    # Confirm presses until the gameplay camera shows up, no fixed waits (boot-until-gameplay.ps1).
    . "$PSScriptRoot\boot-until-gameplay.ps1"
    try {
        $route = Invoke-BootUntilGameplay -EdenLog $log -Process $p
        $route | ForEach-Object { Write-Host $_ } ; $route | Add-Content "$out\launch.txt"
    } catch { "boot route failed: $_" | Add-Content "$out\launch.txt" }
}
$p.WaitForExit()
[void]$keeper.WaitForExit(60000)   # the keeper saves the log and the closing lines
$line1 = Get-Content "$out\eden_log.txt" -TotalCount 1 -ErrorAction SilentlyContinue
Write-Host "Saved. Build: $line1"

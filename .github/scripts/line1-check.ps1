# SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# The built eden.exe names the commit it was built from: start it once with no game, read line 1 of its log, stop it.
# Line 1 must be "VIZR fork <branch> <commit>" with -Expect as the commit. Eden opens its normal window: its Qt is
# linked statically with the Windows platform plugin only, so Qt's offscreen platform is not there (asking for it stops
# Qt with a message box before Eden logs anything).
#   line1-check.ps1 -Bin <build>\bin -Expect <40-character commit>      exit 0 = PASS
param(
    [Parameter(Mandatory)][string]$Bin,
    [Parameter(Mandatory)][string]$Expect,
    [int]$TimeoutSeconds = 60
)
$ErrorActionPreference = 'Stop'
if ($Expect -notmatch '^[0-9a-f]{40}$') { throw "line1-check: -Expect is not a full commit: $Expect" }
$exe = Join-Path $Bin 'eden.exe'
if (-not (Test-Path $exe)) { throw "line1-check: no eden.exe in $Bin" }
# A user folder beside eden.exe makes Eden portable: its log goes to <bin>\user\log, nothing to the profile folders.
$user = Join-Path $Bin 'user'
New-Item -ItemType Directory -Force $user | Out-Null
$log = Join-Path $user 'log\eden_log.txt'
Remove-Item $log -ErrorAction SilentlyContinue
Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -PassThru -WorkingDirectory $Bin
$line1 = $null
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline -and -not $line1) {
    Start-Sleep -Milliseconds 500
    if (Test-Path $log) {
        $first = Get-Content $log -TotalCount 1 -ErrorAction SilentlyContinue
        if ($first) { $line1 = [string]$first }
    }
    if ($p.HasExited -and -not $line1) { break }
}
if (-not $line1) {
    # What Eden was doing: its processes (the startup check runs a child copy), their window titles (a message box
    # names itself here), and what it wrote under its user folder.
    Write-Host "line1-check: FAIL, no log line within $TimeoutSeconds s (exited: $($p.HasExited), exit code $(if ($p.HasExited) { $p.ExitCode }))"
    Get-CimInstance Win32_Process -Filter "Name = 'eden.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
        $w = (Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue).MainWindowTitle
        Write-Host "line1-check:   eden.exe pid $($_.ProcessId) parent $($_.ParentProcessId) window '$w'"
    }
    Get-ChildItem -Recurse -Force $user -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "line1-check:   $($_.FullName.Substring($user.Length)) $($_.Length)" }
}
if (-not $p.HasExited) { & taskkill.exe /T /F /PID $p.Id | Out-Null }
if (-not $line1) { exit 1 }
Write-Host "line1-check: line 1: $line1"
if ($line1 -match "VIZR fork \S+ $Expect\b") { Write-Host "line1-check: PASS, line 1 names $Expect"; exit 0 }
Write-Host "line1-check: FAIL, line 1 does not name $Expect"
exit 1

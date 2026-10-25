# SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# The installer on a clean machine (the release runner): install silently into a new temporary folder with stand-in
# files for the keys and the game (they only pass the installer's own format checks; nothing is decrypted or run),
# check what it wrote, uninstall silently, and check that it is gone again: the install folder's files, the desktop
# and Start menu shortcuts, the per-user uninstall entry, and nothing else new in the user's folders.
#   installer-selftest.ps1 -Setup <setup.exe> -Iss <installer\setup.iss>      exit 0 = PASS
param(
    [Parameter(Mandatory)][string]$Setup,
    [Parameter(Mandatory)][string]$Iss
)
$ErrorActionPreference = 'Stop'
$iss = [IO.File]::ReadAllText((Resolve-Path $Iss))
$appName = [regex]::Match($iss, '(?m)^#define AppName "([^"]+)"').Groups[1].Value
$appId = [regex]::Match($iss, '(?m)^AppId=\{(\{[0-9A-Fa-f-]{36}\})').Groups[1].Value
$titles = [regex]::Match($iss, '(?m)^#define TitleIds "([^"]*)"').Groups[1].Value
if (-not $appName -or -not $appId -or -not $titles) { throw 'installer-selftest: AppName, AppId or TitleIds not found in the script' }
$uninstKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$($appId)_is1"
$desktopLnk = Join-Path ([Environment]::GetFolderPath('Desktop')) "$appName.lnk"
$startLnk = Join-Path ([Environment]::GetFolderPath('Programs')) "$appName.lnk"
$fails = @()
function Check([bool]$Ok, [string]$What) {
    Write-Host ("installer-selftest: {0} {1}" -f $(if ($Ok) { 'PASS' } else { 'FAIL' }), $What)
    if (-not $Ok) { $script:fails += $What }
}
# What lives outside the install folder before the test (the temporary folder, where setup writes its log, excepted).
function Get-Outside {
    $roots = @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('CommonDesktopDirectory'),
               [Environment]::GetFolderPath('Programs'), [Environment]::GetFolderPath('LocalApplicationData'),
               [Environment]::GetFolderPath('ApplicationData'))
    $items = foreach ($r in $roots) { Get-ChildItem -Force -LiteralPath $r -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName } }
    $items += Get-ChildItem 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }
    $items += Get-ChildItem 'HKCU:\Software' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }
    return @($items | Sort-Object -Unique)
}

$work = Join-Path ([IO.Path]::GetTempPath()) "vizr-selftest-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
$app = Join-Path $work 'app'
New-Item -ItemType Directory -Force $work | Out-Null
# Stand-ins: a keys file with the two names the installer looks for, a file with an NSP's magic and a released title id.
$keys = Join-Path $work 'prod.keys'
[IO.File]::WriteAllText($keys, "header_key = $('0' * 64)`nkey_area_key_application_00 = $('0' * 32)`n")
$game = Join-Path $work 'game.nsp'
$title = $titles.Split(';')[0]
[IO.File]::WriteAllBytes($game, [Text.Encoding]::ASCII.GetBytes('PFS0' + ("`0" * 12) + "$title.nca" + ("`0" * 4096)))

$before = Get-Outside
Check (-not (Test-Path $uninstKey)) 'no uninstall entry before the test (a clean runner)'
$p = Start-Process -FilePath $Setup -Wait -PassThru -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
    "/DIR=`"$app`"", "/KEYS=`"$keys`"", "/GAME=`"$game`"", "/LOG=`"$work\setup.log`"")
Check ($p.ExitCode -eq 0) "setup exit code 0 (got $($p.ExitCode))"
Check (Test-Path (Join-Path $app 'build\eden\bin\eden.exe')) 'eden.exe installed'
Check (Test-Path (Join-Path $app 'FORK-SOURCE.md')) 'FORK-SOURCE.md installed'
Check (Test-Path $desktopLnk) "desktop shortcut made ($desktopLnk)"
Check (Test-Path $startLnk) "Start menu shortcut made ($startLnk)"
Check (Test-Path $uninstKey) 'uninstall entry written'
$uninst = if (Test-Path $uninstKey) { (Get-ItemProperty $uninstKey).UninstallString.Trim('"') } else { '' }

if ($uninst -and (Test-Path $uninst)) {
    $u = Start-Process -FilePath $uninst -Wait -PassThru -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    # The uninstaller runs from a copy of itself in the temporary folder: wait for its entry and files to go.
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline -and ((Test-Path $uninstKey) -or (Test-Path $uninst))) { Start-Sleep -Milliseconds 500 }
    Check ($u.ExitCode -eq 0) "uninstall exit code 0 (got $($u.ExitCode))"
} else {
    Check $false 'uninstaller found from the uninstall entry'
}
Check (-not (Test-Path $uninstKey)) 'uninstall entry removed'
Check (-not (Test-Path $desktopLnk)) 'desktop shortcut removed'
Check (-not (Test-Path $startLnk)) 'Start menu shortcut removed'
$left = @(Get-ChildItem -Recurse -File -Force -LiteralPath $app -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName.Substring($app.Length + 1) })
Check ($left.Count -eq 0) "no file left in the install folder ($($left.Count): $($left -join ', '))"
$new = @(Get-Outside | Where-Object { $before -notcontains $_ })
Check ($new.Count -eq 0) "nothing new outside the install folder ($($new.Count): $($new -join ', '))"

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
if ($fails.Count) { Write-Host "installer-selftest: FAIL ($($fails.Count))"; exit 1 }
Write-Host 'installer-selftest: PASS'
exit 0

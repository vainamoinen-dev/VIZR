# SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# VIZR rig, network gate (build 51), static half: the built eden.exe carries none of Eden's own request hosts and paths
# (the built-in news, the release and update requests, the amiibo data download), in 8-bit or UTF-16LE text
# (QStringLiteral), and the build folder has no room server. Links in the About and help texts (HTML hrefs the user's
# browser opens) are not requests and are not counted. The build's CMake options are recorded with the result.
#   net-static-check.ps1 -Bin <build>\bin      exit 0 = PASS
# Build 50's eden.exe is the positive control: it holds the news, release and update strings.
param([Parameter(Mandatory)][string]$Bin)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $Bin 'eden.exe'
$bytes = [IO.File]::ReadAllBytes($exe)
$texts = @{
    'ascii' = [Text.Encoding]::ASCII.GetString($bytes)
    'utf16' = [Text.Encoding]::Unicode.GetString($bytes)                    # even offsets
    'utf16+1' = [Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1)   # odd offsets
}
$forbidden = @(
    'Downloading image: https://eden-emu.dev',   # the news images (builtin_news.cpp DownloadImage)
    '^https?://git.eden-emu.dev',                 # the news entries' link host (a literal string, matched literally)
    '/api/v1/repos/',                             # the release list request path
    'stable.eden-emu.dev',                        # the update hosts
    'nightly.eden-emu.dev',
    'amiiboapi.com',                              # amiibo data download
    'raw.githubusercontent.com'
)
$fail = $false
$out = @()
foreach ($s in $forbidden) {
    $hits = @($texts.Keys | Sort-Object | ForEach-Object { $n = ([regex]::Matches($texts[$_], [regex]::Escape($s))).Count; if ($n) { "$_ x$n" } })
    $out += ("{0,-44} {1}" -f "'$s'", $(if ($hits) { "FOUND ($($hits -join ', '))" } else { 'absent' }))
    if ($hits) { $fail = $true }
}
# A bare request host (a NUL-terminated string), not an href inside HTML text.
foreach ($h in 'https://eden-emu.dev', 'git.eden-emu.dev', 'https://git.eden-emu.dev') {
    $n = ([regex]::Matches($texts['ascii'], '\x00' + [regex]::Escape($h) + '\x00')).Count +
         ([regex]::Matches($texts['utf16'] + $texts['utf16+1'], '\x00' + [regex]::Escape($h) + '\x00')).Count
    $out += ("{0,-44} {1}" -f "bare '$h'", $(if ($n) { "FOUND x$n" } else { 'absent' }))
    if ($n) { $fail = $true }
}
$room = @(Get-ChildItem $Bin -Filter 'eden-room*' -ErrorAction SilentlyContinue)
$out += "eden-room in the bin folder: $(if ($room) { 'PRESENT' } else { 'absent' })"
if ($room) { $fail = $true }
$cache = Join-Path (Split-Path $Bin) 'CMakeCache.txt'
if (Test-Path $cache) {
    $out += 'CMake options: ' + ((Select-String -Path $cache -Pattern '^(VIZR_OFFLINE|YUZU_ROOM|YUZU_ROOM_STANDALONE|ENABLE_WEB_SERVICE|ENABLE_UPDATE_CHECKER|USE_DISCORD_PRESENCE):' |
        ForEach-Object { $_.Line -replace ':[A-Z]+=', '=' }) -join ', ')
} else { $out += "CMake options: no CMakeCache.txt beside $Bin" }
$out += "RESULT $(if ($fail) { 'FAIL' } else { 'PASS' })"
$out | ForEach-Object { Write-Host "net-static: $_" }
exit $(if ($fail) { 1 } else { 0 })

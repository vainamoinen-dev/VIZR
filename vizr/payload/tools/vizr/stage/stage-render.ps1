# SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# VIZR: replace the [render] section of a staged game.toml (a profile COPY, never the live one). Appending a
# second [render] breaks TOML ("cannot redefine table"), which disables the whole profile.
# -Flat: also take the live file's commented flat_layers block uncommented into the new section.
function Set-StagedRender {
    param(
        [Parameter(Mandatory)][string]$GameToml,
        [Parameter(Mandatory)][string[]]$Keys,   # e.g. 'stereo = "afr"', 'scale = "2"'
        [switch]$Flat
    )
    $lines = Get-Content $GameToml
    $out = New-Object System.Collections.Generic.List[string]
    $flatLines = New-Object System.Collections.Generic.List[string]   # ($flat would be the -Flat switch)
    $inRender = $false
    $inFlat = $false
    foreach ($line in $lines) {
        if ($line -match '^\s*\[([^\]]+)\]\s*(#.*)?$') { $inRender = $Matches[1].Trim() -eq 'render' }
        if ($inRender) {
            if ($line -match '^#\s*flat_layers\s*=\s*\[') { $inFlat = $true }
            if ($inFlat) {
                # Only the list brackets and one-line entries with a real hash: the placeholder entry
                # ("0x????...", a multi-line name) is not valid TOML uncommented).
                $plain = $line -replace '^#\s?', ''
                if ($plain -match '^\s*flat_layers\s*=\s*\[' -or $plain -match '^\s*\]' -or
                    $plain -match '^\s*\{\s*shader_hash\s*=\s*"0x[0-9a-fA-F]{16}".*\},?\s*$') {
                    $flatLines.Add($plain)
                }
                if ($line -match '^#\s*\]') { $inFlat = $false }
            }
            continue
        }
        $out.Add($line)
    }
    $out.Add('')
    $out.Add('[render]')
    foreach ($k in $Keys) { $out.Add($k) }
    if ($Flat) { foreach ($f in $flatLines) { $out.Add($f) } }
    [IO.File]::WriteAllLines($GameToml, $out, (New-Object Text.UTF8Encoding($false)))
    return $flatLines.Count
}

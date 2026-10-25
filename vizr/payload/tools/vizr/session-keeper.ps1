# SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# VIZR session keeper: started hidden by launch.ps1 once Eden runs, and tied to Eden's process, not to the launcher
# window (a player may close that window first; the log then went unsaved). While Eden runs it writes the GPU once a
# second to gpu.csv (load, the video encoder's load: a headset stream shares the GPU, power, clocks, temperature,
# memory) and the CPU to cpu.csv (the whole CPU, its busiest core, Eden's share and Eden's three busiest threads by
# name); when Eden exits it saves Eden's log into the session folder and adds the session's closing lines to
# launch.txt, among them the game's hitch rate per simulation rate.
#   session-keeper.ps1 -Out <session folder> -EdenLog <bin>\user\log\eden_log.txt -EdenPid <pid>
param(
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$EdenLog,
    [Parameter(Mandatory)][int]$EdenPid
)
$ErrorActionPreference = 'Continue'
$eden = Get-Process -Id $EdenPid -ErrorAction SilentlyContinue
# nvidia-smi once a second, appended (its own -l -f mode writes nothing until it exits cleanly).
$smi = (Get-Command nvidia-smi -ErrorAction SilentlyContinue).Source
$query = '--query-gpu=timestamp,utilization.gpu,utilization.encoder,power.draw,clocks.gr,clocks.mem,temperature.gpu,memory.used'
if ($smi) { & $smi $query --format=csv | Select-Object -First 1 | Set-Content "$Out\gpu.csv" }
# Thread names (Eden names its threads): GetThreadDescription.
Add-Type -ErrorAction SilentlyContinue -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class VizrThreads {
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr h, out IntPtr text);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
    public static string Name(int id) {
        IntPtr h = OpenThread(0x1000, false, (uint)id); // THREAD_QUERY_LIMITED_INFORMATION
        if (h == IntPtr.Zero) return "";
        try {
            IntPtr text;
            if (GetThreadDescription(h, out text) < 0 || text == IntPtr.Zero) return "";
            string s = Marshal.PtrToStringUni(text);
            LocalFree(text);
            return s ?? "";
        } finally { CloseHandle(h); }
    }
}
'@
Set-Content "$Out\cpu.csv" 'time,cpu_total_pct,cpu_busiest_core_pct,eden_pct_of_one_core,eden_top_threads_pct_of_one_core'
$lastEden = $null; $lastThreads = @{}; $names = @{}; $lastAt = Get-Date
while ($eden -and -not $eden.HasExited) {
    if ($smi) { & $smi $query --format=csv,noheader | Add-Content "$Out\gpu.csv" }
    try {
        $now = Get-Date
        $span = ($now - $lastAt).TotalSeconds; $lastAt = $now
        $cpu = Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -ErrorAction Stop
        $total = ($cpu | Where-Object Name -eq '_Total').PercentProcessorTime
        $busiest = ($cpu | Where-Object Name -ne '_Total' | Measure-Object PercentProcessorTime -Maximum).Maximum
        $eden.Refresh()
        $edenPct = if ($lastEden -and $span -gt 0) { [math]::Round(($eden.TotalProcessorTime - $lastEden).TotalSeconds / $span * 100) } else { '' }
        $lastEden = $eden.TotalProcessorTime
        $threads = @{}
        foreach ($t in $eden.Threads) { try { $threads[$t.Id] = $t.TotalProcessorTime } catch { } }
        $top = ''
        if ($lastThreads.Count -and $span -gt 0) {
            $top = ($threads.Keys | Where-Object { $lastThreads.ContainsKey($_) } | ForEach-Object {
                [pscustomobject]@{ Id = $_; Pct = ($threads[$_] - $lastThreads[$_]).TotalSeconds / $span * 100 } } |
                Sort-Object Pct -Descending | Select-Object -First 3 | ForEach-Object {
                    if (-not $names.ContainsKey($_.Id)) { $names[$_.Id] = [VizrThreads]::Name($_.Id) }
                    $n = if ($names[$_.Id]) { $names[$_.Id] } else { "tid$($_.Id)" }
                    "$($n -replace '[,;]', ' '):$([math]::Round($_.Pct))" }) -join ';'
        }
        $lastThreads = $threads
        Add-Content "$Out\cpu.csv" "$($now.ToString('HH:mm:ss')),$total,$busiest,$edenPct,$top"
    } catch { }
    Start-Sleep -Milliseconds ([math]::Max(100, 1000 - ((Get-Date) - $lastAt).TotalMilliseconds))
}
Start-Sleep -Seconds 2   # Eden's last log lines
Copy-Item $EdenLog "$Out\eden_log.txt" -ErrorAction SilentlyContinue
$lines = @()
$lines += Select-String -Path "$Out\eden_log.txt" -Pattern 'game tick set to .*' -ErrorAction SilentlyContinue |
    ForEach-Object { "tick $($_.Matches[0].Value)" }
# The game's hitches (frames over twice the median, from the once-a-second frame lines) per simulation rate.
$tick = '1/60'; $per = @{}
foreach ($m in (Select-String -Path "$Out\eden_log.txt" -Pattern 'game tick set to (1/\d+)|1s game frames: \d+ at .*over 2x p50: (\d+)' -ErrorAction SilentlyContinue)) {
    if ($m.Matches[0].Groups[1].Success) { $tick = $m.Matches[0].Groups[1].Value; continue }
    if (-not $per.ContainsKey($tick)) { $per[$tick] = @(0, 0) }
    $per[$tick] = @(($per[$tick][0] + 1), ($per[$tick][1] + [int]$m.Matches[0].Groups[2].Value))
}
foreach ($k in $per.Keys) {
    $s = $per[$k][0]; $h = $per[$k][1]
    $lines += "hitches tick $k`: $h in $s s ($([math]::Round($h * 60.0 / [math]::Max($s, 1), 1)) per minute)"
}
$line1 = Get-Content "$Out\eden_log.txt" -TotalCount 1 -ErrorAction SilentlyContinue
$lines += "line1 $line1"
$runtimeScale = Select-String -Path "$Out\eden_log.txt" -Pattern 'render scale \S+ from runtime per-eye \d+' -ErrorAction SilentlyContinue |
    Select-Object -First 1
$lines += "render-final $(if ($runtimeScale) { $runtimeScale.Matches[0].Value } else { 'not set by Eden (see the render line)' })"
$lines += "saved $(Get-Date -Format s) (session keeper: Eden exited)"
$lines | Add-Content "$Out\launch.txt"

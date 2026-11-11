<#
.SYNOPSIS
    In-run A/B of a run-time setting (docs/research/performance.md 33): ONE game session per scene, the arms alternate inside it (A B A B ...) and
    each arm is measured in a tagged perf window (GPU time of every render pass + the interval's `perf:` CPU line + the `native: stages` line of
    exactly that window); summary per arm (mean / sd over the windows) by tools/perf_ab.py, the stage states checked per window.

.DESCRIPTION
    Schedule (overlay [debug] commands, src/overlay/overlay.cpp): anchored at the world of the scene (`load2+N` = N presents after the savegame's
    loading screen for the hold scene, `freeze+N` = after rvkcompare froze the game time at a spot), for k = 0 .. 2 * Pairs - 1:
        <anchor>+(Lead + k * (Settle + Window + 4)):          set <Key> <A or B>
        <anchor>+(Lead + k * (Settle + Window + 4) + Settle): perf window <Window> <A|B>_<pair>
    then `quit` (the process ends itself after the last window's GPU summary: no idle hold). Setting k = 0 to A as well makes every arm's window
    start Settle frames after a toggle. Restart-only settings (Flag::Restart in src/core/settings.cpp, or -Restart) fall back to one session per
    arm and pair with a single window each (the old way, shorter holds).

    Scenes: 'hold' = rvk_perfsave at -QuickSave (the hero stands at the savegame's place); 'point' = rvk_pointshadow (the point-light atlas renders;
    anchor load1, give -Lead the scenario's 30 s); any other name = an rvkcompare spot (time frozen).

.EXAMPLE
    .\scripts\perf-ab.ps1 -Key native.vertex_pull -SettingsFile B:\DevDrive\rvk-work\scratch\uberfix\all_settings.txt -Setting 'debug.ubershader=true' -PlayerDefaults
    .\scripts\perf-ab.ps1 -Key native.owned_trees -Scenes jungle -Pairs 4 -GameDir B:\DevDrive\rvk-work\copies\risen-test-vk-iter -Instance 58 -LockName rvk-iter
#>
param(
    [Parameter(Mandatory)][string]$Key,
    [string]$A = 'false',
    [string]$B = 'true',
    [string[]]$Scenes = @('hold', 'jungle'),
    [int]$Pairs = 3,
    # frames after a toggle before the arm's window starts (pipelines of the new arm built, caches warm); frames of each window
    [int]$Settle = 240,
    [int]$Window = 240,
    # frames after the anchor before the first toggle (streaming after the load / the spot's arrival); -Restart sessions wait -RestartLead
    # (between launches the first seconds vary most: 2026-11-10, performance.md 34)
    [int]$Lead = 600,
    [int]$RestartLead = 1800,
    # player settings of every arm (+ lines of -SettingsFile: key=value per line); the arms only change -Key
    [string[]]$Setting = @(),
    [string]$SettingsFile = '',
    [string[]]$Environment = @(),
    [ValidateSet('wrapper', 'full')][string]$Mode = 'full',
    [ValidateSet('vulkan', 'd3d12')][string]$Api = 'vulkan',
    [string]$Exe = 'RisenTest.exe',
    [switch]$PlayerDefaults,
    [string]$QuickSave = 'C:\Users\johnh\AppData\Local\Risen\SaveGames\Risen_Save_0005.save',
    # the default copy + lock = timing runs (CLAUDE.md: perf timing uses the shared lock); -LockName for a private copy
    [string]$GameDir = 'B:\DevDrive\rvk-work\copies\risen-test-vk',
    [int]$Instance = 0,
    [string]$LockName = '',
    [string]$Who = 'perf-ab',
    [switch]$Restart,
    # a window of an arm that claims the key on may show its stage armed but idle (nothing of its kind in view)
    [switch]$AllowIdle,
    [switch]$NoLock,
    # overlay command template of the toggle instead of `set <Key> <value>` (e.g. 'debug pull {0}'); stage states are not asserted then
    [string]$CommandTemplate = '',
    [string]$Out = '',
    [int]$Seconds = 300
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\rvk-work.ps1"
. "$PSScriptRoot\lib\stage-state.ps1"
$scriptDir = $PSScriptRoot
$repo = Split-Path $PSScriptRoot -Parent
if ($Out -eq '') { $Out = Join-Path $RvkScratch ("perf-ab\{0}_{1}" -f ($Key -replace '[^\w]', '_'), (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force $Out | Out-Null
$GameDir = Resolve-RvkGameDir $GameDir

$base = @() + $Setting
if ($SettingsFile -ne '') { $base += Get-Content $SettingsFile | Where-Object { $_ -match '\S' -and $_ -notmatch "^\s*#" } }
$base = @($base | Where-Object { $_ -notlike "$Key=*" })
$base += @('vulkan.vsync=off', 'vulkan.max_fps=0')

# a restart-only setting cannot be toggled in a session
if (-not $Restart) {
    $src = Get-Content (Join-Path $repo 'src\core\settings.cpp') -Raw
    $at = $src.IndexOf(".key = `"$Key`"")
    if ($at -lt 0) { Write-Warning "perf-ab: $Key not found in src\core\settings.cpp (assuming a run-time setting)" }
    else {
        $next = $src.IndexOf('reg.add(', $at)
        $block = if ($next -gt $at) { $src.Substring($at, $next - $at) } else { $src.Substring($at) }
        if ($block -match 'Flag::Restart') { $Restart = $true; Write-Host "perf-ab: $Key is a restart setting: one session per arm and pair" -ForegroundColor Yellow }
    }
}

function Invoke-Session([string]$Scene, [string]$Tag, [string[]]$Settings, [string[]]$Commands) {
    $h = @{ GameDir = $GameDir; Mode = $Mode; WaitForResult = $true; Seconds = $Seconds; Setting = $Settings; Perf = $true; Vsync = 'off'; Exe = $Exe; Commands = $Commands }
    if ($Instance -ne 0) { $h.Instance = $Instance }
    if ($Api -ne 'vulkan') { $h.Api = $Api }
    if ($Environment.Count -gt 0) { $h.Environment = $Environment }
    if ($PlayerDefaults) { $h.PlayerDefaults = $true }
    if ($Scene -eq 'hold') {
        $h.Mods = 'rvk_perfsave'; $h.Scenario = 'rvk_perfsave'; $h.QuickSave = $QuickSave
        $h.Param = @('hold=240')   # (ended by the schedule's quit)
        $h.Setting += @('lod.view=2', 'lod.trees=2', 'lod.meshes=2')
    } elseif ($Scene -eq 'point') {
        # rvk_pointshadow (frame-ownership.md 14.29): a light with CastShadows at night = the point-light atlas renders; spawned ~30 s after the new game's
        # loading screen (anchor load1): give -Lead room for that (e.g. -Lead 9000 at ~150 fps); the hold is ended by the schedule's quit
        $h.Mods = 'rvk_pointshadow'; $h.Scenario = 'rvk_pointshadow'
        $h.Param = @('hold=900')
    } else {
        $h.Mods = 'rvkcompare'; $h.Scenario = 'rvkcompare'
        $h.Param = @("tag=pab", "spots=$Scene", 'burst=1', 'freeze_wait=100000', 'start_wait=10')
    }
    $dir = Join-Path $Out $Tag
    New-Item -ItemType Directory -Force $dir | Out-Null
    $log = Join-Path $GameDir 'bin\rvk\rvk.log'
    $block = {
        Remove-Item (Join-Path $GameDir 'localappdata\Local\Risen\SaveGames\AutoSave.save') -ErrorAction SilentlyContinue
        Push-Location $repo
        try { & "$scriptDir\run.ps1" @h *> (Join-Path $dir 'run.txt') } finally { Pop-Location }
        # (the `quit` command terminates the game: wait until its process is gone and d3d9.dll is free again, or the next session's deploy fails)
        for ($i = 0; $i -lt 60; $i++) {
            $alive = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$GameDir\*" }
            $free = $true
            try { [IO.File]::Open((Join-Path $GameDir 'bin\d3d9.dll'), 'Open', 'ReadWrite', 'None').Dispose() } catch { $free = $false }
            if (-not $alive -and $free) { break }
            Start-Sleep -Milliseconds 500
        }
        Copy-Item $log (Join-Path $dir 'rvk.log') -Force -ErrorAction SilentlyContinue
    }   # (with-test-lock runs it in a child scope of this function: $h / $dir / $log resolve here)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    if ($NoLock) { & $block }
    else {
        $lockArgs = @{ Who = $Who; Run = $block }
        if ($LockName -ne '') { $lockArgs.Name = $LockName }
        & "$scriptDir\with-test-lock.ps1" @lockArgs
    }
    $sw.Stop()
    Write-Host ("perf-ab: session {0} took {1:N0} s" -f $Tag, $sw.Elapsed.TotalSeconds)
    return (Join-Path $dir 'rvk.log')
}

$total = [Diagnostics.Stopwatch]::StartNew()
$logs = @()
foreach ($scene in $Scenes) {
    $anchor = if ($scene -eq 'hold') { 'load2' } elseif ($scene -eq 'point') { 'load1' } else { 'freeze' }
    if (-not $Restart) {
        $cmds = @()
        $t = $Lead
        for ($k = 0; $k -lt 2 * $Pairs; $k++) {
            $arm = if ($k % 2 -eq 0) { 'A' } else { 'B' }
            $value = if ($arm -eq 'A') { $A } else { $B }
            $cmds += "${anchor}+${t}: " + $(if ($CommandTemplate -ne '') { $CommandTemplate -f $value } else { "set $Key $value" })
            $cmds += "${anchor}+$($t + $Settle): perf window $Window ${arm}_$([math]::Floor($k / 2) + 1)"
            # (+ 4: the next toggle comes after the window's end report: a toggle in the present that closes the window would show in its stage line)
            $t += $Settle + $Window + 4
        }
        $cmds += "${anchor}+$($t + 60): quit"
        $logs += , @($scene, (Invoke-Session $scene "$scene" ($base + $(if ($CommandTemplate -ne "") { @() } else { "$Key=$A" })) $cmds))
    } else {
        for ($p = 1; $p -le $Pairs; $p++) {
            foreach ($arm in 'A', 'B') {
                $value = if ($arm -eq 'A') { $A } else { $B }
                $cmds = @("${anchor}+$($RestartLead + $Settle): perf window $Window ${arm}_$p", "${anchor}+$($RestartLead + $Settle + $Window + 60): quit")
                $logs += , @($scene, (Invoke-Session $scene "${scene}_${arm}$p" ($base + "$Key=$value") $cmds))
            }
        }
    }
}
$total.Stop()

# stage states per window (scripts/lib/stage-state.ps1): a window of an arm must show the arm's state of the key's stage
$failures = @()
foreach ($l in $logs) {
    if (-not (Test-Path $l[1])) { $failures += "no rvk.log for $($l[0])"; continue }
    $wins = Get-PerfWindowStages $l[1]
    if ($wins.Count -eq 0) { $failures += "$($l[0]): no perf window in $($l[1]) (did the schedule's anchor fire? see 'overlay: [debug] command' lines)" }
    foreach ($w in $wins) {
        $value = if ($w.Tag -like 'A_*') { $A } else { $B }
        if ($CommandTemplate -ne '') { continue }
        try { Assert-WindowStages -Window $w -Settings @("$Key=$value") -Arm "$($l[0]) $($w.Tag)" -Quiet -AllowIdle:$AllowIdle }
        catch { $failures += $_.Exception.Message }
    }
}
$argsPy = @((Join-Path $repo 'tools\perf_ab.py'), '--key', $Key, '--json', (Join-Path $Out 'summary.json'))
foreach ($l in $logs) { $argsPy += "$($l[0])=$($l[1])" }
python @argsPy | Tee-Object (Join-Path $Out 'summary.txt')
Write-Host ("perf-ab: {0} session(s), {1:N0} s of wall time; out: {2}" -f $logs.Count, $total.Elapsed.TotalSeconds, $Out)
if ($failures.Count -gt 0) {
    Write-Host ("STAGE STATE CHECK FAILED (the numbers above do not compare what the arms claim; a stage armed but idle after a run-time toggle = use -Restart):`n  " + ($failures -join "`n  ")) -ForegroundColor Red
    exit 1
}
Write-Host 'stage states: every window matches its arm' -ForegroundColor Green

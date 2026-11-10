# Owned-stage state assertions for A/B runs (docs/research/frame-ownership.md 7.47).
#
# 7.46: `-Commands 'N: set native.owned_trees ...'` never switched the trees on (the setting was not wired to applyStaticOps) and no A/B checked that it did, so
# several "picture A/B" verdicts compared a stage with itself. Every 5 s rvk.log now holds one line per window:
#   native: stages (5 s): owned_static=active(...) owned_trees=inactive(setting on, code off) ... ubershader=inactive(setting off)
#   native: own post state (5 s): active (setting 1, code 1)
# state = active(<what it did>) | inactive(setting off) | inactive(setting on, code off) | inactive(armed, nothing owned in the window) | INCONSISTENT(...)
# An A/B script calls Assert-ArmStages after each arm: a stage the arm claims on must be `active`, one it claims off `inactive`, and no stage may be INCONSISTENT.
#   . "$PSScriptRoot\lib\stage-state.ps1"
#   Assert-ArmStages -Log <rvk.log> -Settings @('native.owned_trees=true') -Arm 'enh'   # throws on a failed assertion

# the setting key -> the stage name in the `native: stages` line
$script:StageOfSetting = [ordered]@{
    'native.owned_static'          = 'owned_static'
    'native.owned_persistent'      = 'owned_persistent'
    'native.owned_trees'           = 'owned_trees'
    'native.owned_shadows'         = 'owned_shadows'
    'native.owned_actors'          = 'owned_actors'
    'native.owned_vegetation'      = 'owned_vegetation'
    'native.owned_vegetation_ops'  = 'owned_vegetation_ops'
    'native.clusters'              = 'clusters'
    'native.cluster_cull'          = 'cluster_cull'
    'debug.ubershader'             = 'ubershader'
    'native.depth_z'               = 'depth_z'
    'native.vertex_pull'           = 'vertex_pull'
    'native.own_foliage'           = 'own_foliage'
    'native.bone_motion'           = 'bone_motion'
}

# the stage windows of a log: an array of hashtables name -> state string (the first window of a run is a partial one: callers skip it)
function Get-StageWindows([string]$Log) {
    $windows = @()
    foreach ($line in (Select-String -Path $Log -Pattern 'native: stages \(5 s\):' -SimpleMatch:$false)) {
        $text = $line.Line -replace '^.*native: stages \(5 s\):\s*', ''
        $w = [ordered]@{}
        # name=state where a state may hold parentheses with spaces: split at the next ' name=' boundary
        foreach ($m in [regex]::Matches($text, '(\w+)=((?:active|inactive|INCONSISTENT)\((?:[^()]|\([^()]*\))*\))')) { $w[$m.Groups[1].Value] = $m.Groups[2].Value }
        $windows += , $w
    }
    return , $windows
}

# the stage states of each perf window of a log (scripts/perf-ab.ps1, `perf window <frames> <tag>`, docs/research/performance.md 33): the HAL closes its
# stats interval right before a window and right after it, so the `native: stages (5 s)` line between `perf: window <tag> closing` and `perf: window <tag> end`
# covers exactly the window's frames. Returns objects { Tag, Stages (name -> state; empty when the mode has no stage line), Line }.
function Get-PerfWindowStages([string]$Log) {
    $out = @()
    $tag = $null
    $stages = $null
    foreach ($line in [System.IO.File]::ReadLines($Log)) {
        if ($line -match 'perf: window (\S+) closing at frame') { $tag = $Matches[1]; $stages = [ordered]@{}; continue }
        if ($null -eq $tag) { continue }
        if ($line -match 'native: stages \(5 s\):\s*(.*)$') {
            foreach ($m in [regex]::Matches($Matches[1], '(\w+)=((?:active|inactive|INCONSISTENT)\((?:[^()]|\([^()]*\))*\))')) { $stages[$m.Groups[1].Value] = $m.Groups[2].Value }
            continue
        }
        if ($line -match "perf: window $([regex]::Escape($tag)) end at frame") {
            $out += [pscustomobject]@{ Tag = $tag; Stages = $stages }
            $tag = $null
        }
    }
    return , $out
}

# Asserts one perf window's stage states against the arm's 'key=value' settings (the rules of Assert-ArmStages for a single window: a key claimed on must
# be active or armed, one claimed off inactive, no stage INCONSISTENT). Settings without a stage name are not checked. Throws on a failure.
function Assert-WindowStages($Window, [string[]]$Settings, [string]$Arm = '', [switch]$Quiet, [switch]$AllowIdle) {
    $failures = @()
    foreach ($k in $Window.Stages.Keys) { if ($Window.Stages[$k] -like 'INCONSISTENT*') { $failures += "stage $k is $($Window.Stages[$k])" } }
    foreach ($s in $Settings) {
        $kv = $s -split '=', 2
        if ($kv.Count -ne 2 -or -not $script:StageOfSetting.Contains($kv[0])) { continue }
        $stage = $script:StageOfSetting[$kv[0]]
        if ($Window.Stages.Count -eq 0) { $failures += "no 'native: stages' line inside window $($Window.Tag) (Full mode reports it)"; continue }
        $state = if ($Window.Stages.Contains($stage)) { $Window.Stages[$stage] } else { '(missing)' }
        $want = $kv[1].ToLowerInvariant() -in @('true', '1', 'on', 'compare')
        # (stricter than Assert-ArmStages: a window lies inside one arm after its settle frames, so an arm claimed on must have ACTED in it;
        # `inactive(armed, nothing owned in the window)` = the toggle armed the code but nothing ran through it, e.g. native.vertex_pull switched on
        # at run time: no record is rebuilt pulled; -AllowIdle accepts it for stages that legitimately have nothing in view)
        if ($want -and $state -notlike 'active*' -and -not ($AllowIdle -and $state -like 'inactive(armed*')) { $failures += "window $($Window.Tag) claims $($kv[0])=on but $stage=$state" }
        if (-not $want -and $state -like 'active*') { $failures += "window $($Window.Tag) claims $($kv[0])=off but $stage=$state" }
    }
    if ($failures.Count -gt 0) {
        $msg = "STAGE STATE ASSERTION FAILED ($Arm): " + ($failures -join '; ')
        if (-not $Quiet) { Write-Host $msg -ForegroundColor Red }
        throw $msg
    }
    if (-not $Quiet) { Write-Host "stage states of window '$($Window.Tag)' match its claims" -ForegroundColor Green }
}

function Get-OwnPostWindows([string]$Log) {
    $out = @()
    foreach ($line in (Select-String -Path $Log -Pattern 'native: own post state \(5 s\): (\w+) \(setting (\d+), code (\d+)\)(.*)')) {
        $m = $line.Matches[0]
        $out += [pscustomobject]@{ State = $m.Groups[1].Value; Setting = [int]$m.Groups[2].Value; Code = [int]$m.Groups[3].Value; Inconsistent = ($m.Groups[4].Value -match 'INCONSISTENT') }
    }
    return , $out
}

# Asserts the arm's stage states from its rvk.log. `Settings` = the 'key=value' list the arm ran with (start-time -Setting values and, with -AfterToggles, the
# values the run's -Commands left). Skips the first window (partial) and any window before the world is up (all stages idle at loading: only consistent states checked).
function Assert-ArmStages([string]$Log, [string[]]$Settings, [string]$Arm = "", [switch]$Quiet) {
    $failures = @()
    if (-not (Test-Path $Log)) { throw "Assert-ArmStages: no log $Log" }
    $windows = Get-StageWindows $Log
    if ($windows.Count -eq 0) {
        if ($Settings | Where-Object { $_ -match '^(native\.|debug\.ubershader)' }) { $failures += 'no `native: stages (5 s)` line in the log (the run never reached a stage report)' }
    }
    else {
        $considered = if ($windows.Count -gt 1) { $windows[1..($windows.Count - 1)] } else { $windows }
        foreach ($w in $considered) {
            foreach ($k in $w.Keys) { if ($w[$k] -like 'INCONSISTENT*') { $failures += "stage $k is $($w[$k])" } }
        }
        $last = $considered[-1]
        foreach ($s in $Settings) {
            $kv = $s -split '=', 2
            if ($kv.Count -ne 2 -or -not $script:StageOfSetting.Contains($kv[0])) { continue }
            $stage = $script:StageOfSetting[$kv[0]]
            $want = $kv[1].ToLowerInvariant() -in @('true', '1', 'on', 'compare')
            $anyActive = $false; $everActive = $false
            foreach ($w in $considered) { if ($w.Contains($stage) -and $w[$stage] -like 'active*') { $anyActive = $true } }
            $lastState = if ($last.Contains($stage)) { $last[$stage] } else { '(missing)' }
            if ($want) {
                # on = it acted in some window of the run and is armed at the end (a spot with nothing of its kind in view makes the last window 'armed, nothing owned')
                if (-not $anyActive) { $failures += "arm claims $($kv[0])=on but no window says $stage active (last: $lastState)" }
                elseif ($lastState -notlike 'active*' -and $lastState -notlike 'inactive(armed*') { $failures += "arm claims $($kv[0])=on but the last window says $stage=$lastState" }
            }
            else {
                if ($anyActive) { $failures += "arm claims $($kv[0])=off but a window says $stage active" }
            }
        }
    }
    $post = Get-OwnPostWindows $Log
    foreach ($p in $post) { if ($p.Inconsistent) { $failures += "own post: setting $($p.Setting) but code $($p.Code)" } }
    foreach ($s in $Settings) {
        if ($s -match '^native\.own_post=(\w+)$' -and $post.Count -gt 0) {
            $wantActive = $Matches[1] -notin @('off', '0', 'false')
            if (($post[-1].State -eq 'active') -ne $wantActive) { $failures += "arm claims native.own_post=$($Matches[1]) but the last state is $($post[-1].State)" }
        }
    }
    if ($failures.Count -gt 0) {
        $msg = "STAGE STATE ASSERTION FAILED ($Arm): " + ($failures -join '; ')
        Write-Host $msg -ForegroundColor Red
        throw $msg
    }
    if (-not $Quiet) { Write-Host "stage states of arm '$Arm' match its claims ($($windows.Count) windows, settings: $($Settings -join ' '))" -ForegroundColor Green }
}

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

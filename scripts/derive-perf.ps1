<#
.SYNOPSIS
    Cost of materials.derive_from_specular (docs/research/material-pack.md 6): N interleaved runs of mods/rvkcompare at one spot, derive off vs on, with a
    pack folder that matches nothing (so the variants exist in both arms and only the derived words differ), -Perf + -PerfFrame over a span of frames.
    Prints per arm: GPU main submission ms, Solid pass ms, the pack lookup counters, and the uncapped fps / CPU lines of the 5 s perf windows.
    Default lock + copy for timing numbers (RisenTest.exe, vsync off):
    .\scripts\with-test-lock.ps1 -Name rvk-derive -Run { .\scripts\derive-perf.ps1 -GameDir B:\DevDrive\rvk-work\copies\risen-test-vk-derive -Instance 8 -Api d3d12 -Mode full }
#>
param(
    [string]$Spot = 'harbour_docks',
    [int]$Runs = 3,
    [int]$PerfFrame = 4300,
    [int]$Span = 24,
    [string]$GameDir = 'B:\DevDrive\rvk-work\copies\risen-test-vk',
    [int]$Instance = 0,
    [string]$Api = '',
    [string]$Mode = 'wrapper',
    [string[]]$Setting = @('materials.quality=medium')
)
. "$PSScriptRoot\lib\rvk-work.ps1"; $GameDir = Resolve-RvkGameDir $GameDir
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $repo "out\matpack\dperf\$stamp"
New-Item -ItemType Directory -Force $out | Out-Null
$none = Join-Path $out 'nomatch-pack'
New-Item -ItemType Directory -Force $none | Out-Null
Set-Content (Join-Path $none 'pack.toml') "[material.`"No_Such_Material_*`"]`nroughness = 0.2`n"
$rows = @()
for ($i = 1; $i -le $Runs; ++$i) {
    foreach ($arm in 'off', 'on') {
        $a = @{ GameDir = $GameDir; Instance = $Instance; Mode = $Mode; Exe = 'RisenTest.exe'; Vsync = 'off'; Perf = $true; PerfFrame = $PerfFrame
                Mods = @('rvkcompare'); Scenario = 'rvkcompare'; WaitForResult = $true; Seconds = 900
                Param = @("spots=$Spot", 'freeze_wait=4800')
                Setting = @($Setting + "materials.derive_from_specular=$(if ($arm -eq 'on') { 'true' } else { 'false' })")
                Environment = @("RVK_PERF_FRAMES=$Span", 'RVK_MATPACK=' + ($none -replace '\\', '/')) }
        if ($Api) { $a.Api = $Api }
        & (Join-Path $repo 'scripts\run.ps1') @a | Select-String 'result:' | Out-Host
        $log = Join-Path $out "$arm$i.log"
        Copy-Item (Join-Path $GameDir 'bin\rvk\rvk.log') $log -Force
        $ms = Select-String -Path $log -Pattern 'perf: GPU passes of frame \d+ \(one submission: ([0-9.]+) ms main' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value }
        $solid = Select-String -Path $log -Pattern 'perf:\s+([0-9.]+) ms\s+Frame \d+ > Post > Main #1 > Solid' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value }
        $derived = Select-String -Path $log -Pattern '(\d+) draws looked up, \d+ matched.*?(\d+) derived' | Select-Object -Last 1
        $rows += [pscustomobject]@{ run = $i; arm = $arm; frames = $ms.Count
                                    gpuMain = if ($ms.Count) { [math]::Round(($ms | Measure-Object -Average).Average, 3) } else { $null }
                                    solid = if ($solid.Count) { [math]::Round(($solid | Measure-Object -Average).Average, 3) } else { $null }
                                    lookups = if ($derived) { $derived.Matches[0].Groups[1].Value } else { '' }; derived = if ($derived) { $derived.Matches[0].Groups[2].Value } else { '' } }
    }
}
$rows | Format-Table | Out-String | Tee-Object (Join-Path $out 'summary.txt')
'arm averages:'
$rows | Group-Object arm | ForEach-Object { '{0}: gpuMain {1:N3} ms, solid {2:N3} ms' -f $_.Name, ($_.Group.gpuMain | Measure-Object -Average).Average, ($_.Group.solid | Measure-Object -Average).Average }

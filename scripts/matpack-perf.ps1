<#
.SYNOPSIS
    GPU / CPU cost of the material pack (docs/research/material-pack.md 7): N interleaved runs of mods/rvkcompare at one spot, pack off
    (materials.pack=false) vs. on (the sample pack folder in the copy's bin\rvk\materialpack), -Perf + -PerfFrame over a span of frames.
    Run under the default lock and the default copy for timing numbers:
    .\scripts\with-test-lock.ps1 -Name rvk-pathtrace -Run { .\scripts\with-test-lock.ps1 -Run { .\scripts\matpack-perf.ps1 } }
#>
param(
    [string]$Spot = 'harbour_docks',
    [int]$Runs = 3,
    [int]$PerfFrame = 4300,
    [int]$Span = 24,
    [string]$GameDir = 'B:\DevDrive\risen-test-vk',
    [int]$Instance = 0,
    [string]$Api = '',
    [string]$Mode = 'wrapper'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $repo "out\matpack\perf\$stamp"
New-Item -ItemType Directory -Force $out | Out-Null
$packDir = Join-Path $GameDir 'bin\rvk\materialpack'
New-Item -ItemType Directory -Force $packDir | Out-Null
Copy-Item (Join-Path $repo 'package\materialpack_sample\*') $packDir -Force
$rows = @()
for ($i = 1; $i -le $Runs; ++$i) {
    foreach ($arm in 'off', 'on') {
        $a = @{ GameDir = $GameDir; Instance = $Instance; Mode = $Mode; Exe = 'RisenTest.exe'; Vsync = 'off'; Perf = $true; PerfFrame = $PerfFrame
                Mods = @('rvkcompare'); Scenario = 'rvkcompare'; WaitForResult = $true; Seconds = 900
                Param = @("spots=$Spot", 'freeze_wait=4800'); Setting = @("materials.pack=$(if ($arm -eq 'on') { 'true' } else { 'false' })")
                Environment = @("RVK_PERF_FRAMES=$Span") }
        if ($Api) { $a.Api = $Api }
        & (Join-Path $repo 'scripts\run.ps1') @a | Select-String 'result:' | Out-Host
        $log = Join-Path $out "$arm$i.log"
        Copy-Item (Join-Path $GameDir 'bin\rvk\rvk.log') $log -Force
        # GPU: the "main" ms of each logged frame's one submission
        $ms = Select-String -Path $log -Pattern 'perf: GPU passes of frame \d+ \(one submission: ([0-9.]+) ms main' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value }
        $solid = Select-String -Path $log -Pattern 'perf:\s+([0-9.]+) ms\s+Frame \d+ > Post > Main #1 > Solid' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value }
        $rows += [pscustomobject]@{ run = $i; arm = $arm; frames = $ms.Count; gpuMain = if ($ms.Count) { [math]::Round(($ms | Measure-Object -Average).Average, 3) } else { $null }
                                    solid = if ($solid.Count) { [math]::Round(($solid | Measure-Object -Average).Average, 3) } else { $null } }
    }
}
$rows | Format-Table | Out-String | Tee-Object (Join-Path $out 'summary.txt')

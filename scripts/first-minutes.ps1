<#
.SYNOPSIS
    The first minutes of a session in one cache state (docs/research/frame-ownership.md 14.53): a new game + a walk (mods/rvk_rtwalk, load=0) in a private test copy,
    with the shader caches of the chosen state, Full or Wrapper, and a noise record of the machine (other agents' builds / games).

.DESCRIPTION
    -State cold      bin\rvk\cache emptied (a player's first start)
    -State updated   the module caches (cache\hlsl = the DXC disk cache of every generated and embedded shader, cache\spirv = the old build's SPIR-V cache) emptied, the
                     pipeline key list kept: an update that changed a shader header (every module is new, the keys of the last session still name what to build)
    -State warm      nothing touched (-State keep = the same)
    -DropKeys        also forget the pipeline keys (what a real update did to the builds before WS12: the shader hashes carried the translator version + header hash)

    Output: <Out>\<Tag>_rvk.log, <Tag>_cpu.csv (total / own / others' CPU, other games, compiler processes every 4 s), <Tag>_out.txt.
    Analyse with `python tools/firstminutes.py <Out>\<Tag>_rvk.log [--rows]`: loading screens, frame intervals per 5 s window after the world appeared (all windows and
    the "clean" ones: no other agent's load), the engine thread's CPU per frame, the variant queue, the owned share. The machine is shared: judge Full against a Wrapper
    run of the same state taken back to back, and trust the clean windows.

.EXAMPLE
    .\scripts\first-minutes.ps1 -Tag n_cold_full -State cold -Mode full
    .\scripts\first-minutes.ps1 -Tag n_cold_wrap -State cold -Mode wrapper
#>
param(
    [Parameter(Mandatory)][string]$Tag,
    [ValidateSet('cold', 'updated', 'warm', 'keep')][string]$State = 'warm',
    [ValidateSet('full', 'wrapper')][string]$Mode = 'full',
    [int]$WalkSeconds = 150,
    [string[]]$Setting = @(),
    [string[]]$Environment = @(),
    [string]$Copy = 'risen-test-vk-first',
    [int]$Instance = 41,
    [string]$Config = 'RelWithDebInfo',
    [string]$Out = '',
    [switch]$DropKeys,
    [switch]$CpuProfile,
    [switch]$NoOwned
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\rvk-work.ps1"
if (-not $Out) { $Out = Join-Path $RvkScratch 'first-minutes' }
New-Item -ItemType Directory -Force $Out | Out-Null
$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo
$gd = Join-Path $RvkCopies $Copy
if (-not (Test-Path (Join-Path $gd 'bin\Risen.exe'))) { & "$PSScriptRoot\new-test-copy.ps1" -GameDir $gd }
$cache = Join-Path $gd 'bin\rvk\cache'
switch ($State) {
    'cold' { if (Test-Path $cache) { Remove-Item $cache -Recurse -Force } }
    'updated' { foreach ($d in 'spirv', 'hlsl') { if (Test-Path "$cache\$d") { Remove-Item "$cache\$d" -Recurse -Force } } }
}
if ($DropKeys) { foreach ($f in 'pipelines.keys', 'pipelines.bin') { Remove-Item "$cache\$f" -ErrorAction SilentlyContinue } }
$a = @{ GameDir = $gd; Mode = $Mode; WaitForResult = $true; Seconds = ($WalkSeconds + 400); Perf = $true; GracefulExit = $true; PlayerDefaults = $true; Instance = $Instance; Config = $Config }
if ($Setting.Count -gt 0) { $a.Setting = $Setting }
if ($Environment.Count -gt 0) { $a.Environment = $Environment }
if ($CpuProfile) { $a.CpuProfile = $true }
if ($NoOwned) { $a.NoOwned = $true }
$a.Mods = 'rvk_rtwalk'; $a.Scenario = 'rvk_rtwalk'; $a.Param = @('load=0', "seconds=$WalkSeconds")
$sb = {
    $t0 = Get-Date
    $stop = Join-Path $Out "${Tag}_cpu.stop"
    Remove-Item $stop -ErrorAction SilentlyContinue
    $null = Start-Process pwsh -ArgumentList '-NoProfile', '-File', "$PSScriptRoot\lib\noise-sampler.ps1", '-Csv', (Join-Path $Out "${Tag}_cpu.csv"), '-Stop', $stop, '-Match', $Copy -WindowStyle Hidden -PassThru
    try { & "$PSScriptRoot\run.ps1" @a } finally { New-Item $stop -ItemType File -Force | Out-Null }
    "EXITCODE=$LASTEXITCODE"
    "WALL=$([int]((Get-Date) - $t0).TotalSeconds)s"
    Copy-Item (Join-Path $gd 'bin\rvk\rvk.log') (Join-Path $Out "${Tag}_rvk.log") -Force
}
# the default lock (perf timing): another agent's perf run is not running beside this one
& "$PSScriptRoot\with-test-lock.ps1" -Who "first-minutes $Tag" -Run $sb *> (Join-Path $Out "${Tag}_out.txt")
Get-Content (Join-Path $Out "${Tag}_out.txt") | Select-Object -Last 4

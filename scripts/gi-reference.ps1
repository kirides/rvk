<#
.SYNOPSIS
    How far is the DDGI GI from ground truth? Renders the rvkcompare spots with the GI off, with the GI on (DDGI + screen probes)
    and with the reference path tracer (gi.reference, 1000+ samples per pixel), then compares them (tools/gi_reference.py):
    side by side pictures, error heat maps, numbers and a ranked list of the places the probes miss
    (docs/research/path-tracing.md).

.DESCRIPTION
    Three runs of mods/rvkcompare (pictures <tag>_<spot>.png), the third also writes the reference's planes (bin\rvk\gi_ref\*.npy).
    Results in out\gi-reference\<stamp>\: off\ ddgi\ ref\ (the game's pictures), gi_ref\ (dumps), meta.json (RVKCMP lines),
    analysis\ (sbs_<spot>.png, err_*, crop_*, triptych_*, summary.md, results.json). -RunDir re-analyses an earlier run.

.EXAMPLE
    # in a private copy / instance with its own lock (correctness runs; timing runs use the default lock and copy)
    .\scripts\with-test-lock.ps1 -Name rvk-pathtrace -Who pathtrace -Run { .\scripts\gi-reference.ps1 -GameDir B:\DevDrive\risen-test-vk-pt -Instance 6 }
    .\scripts\gi-reference.ps1 -Spots pub_interior,jungle -Spp 8 -Max 2048
    .\scripts\gi-reference.ps1 -RunDir out\gi-reference\20261104-120000
#>
param(
    [string]$Spots = 'pub_interior,monastery_hall,jungle,lake_jungle,harbour_night,crystal_cave,lava_cave,shadow_inland_a,beach_sun',
    # the GI settings of the "DDGI" and the reference run (the player defaults at quality high, replace mode, the world's lights)
    [string[]]$Setting = @('gi.quality=high', 'gi.lightmaps=replace', 'gi.lights=world'),
    # reference samples per frame / in total / bounces
    [int]$Spp = 4,
    [int]$Max = 1024,
    [int]$Bounces = 6,
    # frames between the freeze and the picture: the reference needs Max / Spp frames after the probes have converged
    [int]$FreezeWait = 0,
    [string]$WindowSize = '1280x720',
    [ValidateSet('vulkan', 'd3d12')][string]$Api = 'vulkan',
    [ValidateSet('wrapper', 'full')][string]$Mode = 'wrapper',
    [string]$GameDir = 'B:\DevDrive\risen-test-vk',
    [string]$Config = 'RelWithDebInfo',
    [int]$Instance = 0,
    [switch]$SkipOff,
    [switch]$SkipDdgi,
    [switch]$Validation,
    [string]$RunDir = '',
    # quick re-measure after a change to the probes: take the converged reference planes of an earlier run (out/gi-reference/<stamp>), run with a few samples only (-Spp 8 -Max 8 -SkipOff -SkipDdgi)
    [string]$RefRun = '',
    # environment variables for the game ('RVK_SHADER_DIR=...' edits shaders without a rebuild)
    [string[]]$Environment = @()
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if ($RunDir) {
    $out = (Resolve-Path $RunDir).Path
} else {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $out = Join-Path $repo "out\gi-reference\$stamp"
    New-Item -ItemType Directory -Force $out | Out-Null
    $shots = Join-Path $GameDir 'bin\spass\screenshots'
    $spassLog = Join-Path $GameDir 'bin\spass\spass.log'
    $giRef = Join-Path $GameDir 'bin\rvk\gi_ref'
    if ($FreezeWait -le 0) { $FreezeWait = [int](700 + 2.0 * $Max / [Math]::Max($Spp, 1)) }
    $spotParam = 'spots=' + ($Spots -replace ',', ';')

    function Invoke-Run([string]$tag, [string[]]$settings) {
        if (Test-Path $shots) { Get-ChildItem $shots -Filter "${tag}_*" | Remove-Item -Force }
        $runArgs = @{ Api = $Api; Config = $Config; Mode = $Mode; GameDir = $GameDir; Instance = $Instance; Mods = @('rvkcompare'); Scenario = 'rvkcompare'
                      WaitForResult = $true; Seconds = 3600; WindowSize = $WindowSize
                      Param = @("tag=$tag", $spotParam, 'burst=1', "freeze_wait=$FreezeWait"); Setting = $settings
                      RvkFilter = 'enh: gi.reference|enh: gi: (on|off)|\[error\]' }
        if ($Validation) { $runArgs.Validation = $true }
        if ($Environment.Count -gt 0) { $runArgs.Environment = $Environment }
        & (Join-Path $PSScriptRoot 'run.ps1') @runArgs | Out-Host
        $dir = Join-Path $out $tag
        New-Item -ItemType Directory -Force $dir | Out-Null
        Get-ChildItem $shots -Filter "${tag}_*.png" -ErrorAction SilentlyContinue | Move-Item -Destination $dir -Force
        Copy-Item (Join-Path $GameDir 'bin\rvk\rvk.log') (Join-Path $dir 'rvk.log') -Force -ErrorAction SilentlyContinue
        Copy-Item $spassLog (Join-Path $dir 'spass.log') -Force -ErrorAction SilentlyContinue
    }

    if (-not $SkipOff) { Invoke-Run 'off' @('gi.enabled=false') }
    if (-not $SkipDdgi) { Invoke-Run 'ddgi' (@('gi.enabled=true') + $Setting) }
    if (Test-Path $giRef) { Remove-Item -Recurse -Force $giRef }
    Invoke-Run 'ref' (@('gi.enabled=true', 'gi.reference=reference', "gi.reference_spp=$Spp", "gi.reference_max=$Max", "gi.reference_bounces=$Bounces") + $Setting)
    if (Test-Path $giRef) { Copy-Item -Recurse $giRef (Join-Path $out 'gi_ref') }
    $metaLines = @(Get-Content (Join-Path $out 'ref\spass.log') | Where-Object { $_ -match 'RVKCMP (\{.*\})$' } | ForEach-Object { $Matches[1] })
    Set-Content -Path (Join-Path $out 'meta.json') -Value ('[' + ($metaLines -join ',') + ']') -Encoding utf8
}
$extra = @()
if ($RefRun) { $extra = @('--ref-run', (($RefRun -split ',' | ForEach-Object { (Resolve-Path $_).Path }) -join ',')) }
python -I (Join-Path $repo 'tools\gi_reference.py') analyze $out @extra
Write-Host "results: $out\analysis (summary.md)"

<#
.SYNOPSIS
    Engine water vs enhanced water at the rvkcompare water spots: two Vulkan runs of mods/rvkcompare (water.enabled false /
    true), side-by-side pictures (left engine, right enhanced) in out\water\<stamp>\ (docs/research/water.md 8).

.EXAMPLE
    .\scripts\water-compare.ps1
    .\scripts\water-compare.ps1 -Spots beach_sun,swamp_pond -Setting 'water.quality=high' -Perf
#>
param(
    [string]$Spots = 'beach_sun,beach_rain,harbour_docks,lake_jungle,waterfall_jungle,swamp_pond,swamp_camp',
    # extra player settings of the enhanced run (water.enabled=true is always set)
    [string[]]$Setting = @(),
    [string]$GameDir = 'B:\DevDrive\risen-test-vk',
    [string[]]$Param = @(),
    [switch]$Validation,
    [switch]$Perf,
    [switch]$SkipEngine,
    # reuse the engine pictures of an earlier run (out\water\<stamp>)
    [string]$EngineDir = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $repo "out\water\$stamp"
New-Item -ItemType Directory -Force $out | Out-Null
$shots = Join-Path $GameDir 'bin\spass\screenshots'
$spotParam = 'spots=' + ($Spots -replace ',', ';')

function Invoke-Run([string]$tag, [string[]]$settings) {
    if (Test-Path $shots) { Get-ChildItem $shots -Filter "${tag}_*" | Remove-Item -Force }
    $runArgs = @{ Mode = 'vulkan'; GameDir = $GameDir; Mods = @('rvkcompare'); Scenario = 'rvkcompare'; WaitForResult = $true; Seconds = 1200
                  Param = @("tag=$tag", $spotParam, 'burst=1') + $Param; Setting = $settings; RvkFilter = 'enh: water|\[error\]' }
    if ($Validation) { $runArgs.Validation = $true }
    if ($Perf) { $runArgs.Perf = $true }
    & (Join-Path $PSScriptRoot 'run.ps1') @runArgs | Out-Host
    $dir = Join-Path $out $tag
    New-Item -ItemType Directory -Force $dir | Out-Null
    Get-ChildItem $shots -Filter "${tag}_*.png" -ErrorAction SilentlyContinue | Move-Item -Destination $dir -Force
    Copy-Item (Join-Path $GameDir 'bin\rvk\rvk.log') (Join-Path $dir 'rvk.log') -Force -ErrorAction SilentlyContinue
}

if ($EngineDir) { Copy-Item -Recurse (Join-Path $EngineDir 'eng') (Join-Path $out 'eng') }
elseif (-not $SkipEngine) { Invoke-Run 'eng' @('water.enabled=false') }
Invoke-Run 'enh' (@('water.enabled=true') + $Setting)

# side by side (left: engine, right: enhanced)
$py = @'
import sys, os
from PIL import Image
out = sys.argv[1]
eng, enh = os.path.join(out, 'eng'), os.path.join(out, 'enh')
if not os.path.isdir(enh):
    sys.exit(0)
for f in sorted(os.listdir(enh)):
    if not f.endswith('.png'):
        continue
    spot = f[len('enh_'):]
    b = Image.open(os.path.join(enh, f)).convert('RGB')
    pe = os.path.join(eng, 'eng_' + spot)
    a = Image.open(pe).convert('RGB') if os.path.exists(pe) else Image.new('RGB', b.size)
    w, h = b.size
    crop = (0, 113, w, 787) if h == 900 else (0, 0, w, h)
    a, b = a.crop(crop), b.crop(crop)
    s = Image.new('RGB', (a.width + b.width, a.height))
    s.paste(a, (0, 0))
    s.paste(b, (a.width, 0))
    s.save(os.path.join(out, 'sbs_' + spot))
    print('sbs_' + spot)
'@
$pyFile = Join-Path $out 'sbs.py'
Set-Content -Path $pyFile -Value $py -Encoding utf8
python -I $pyFile $out
Write-Host "side by side: $out"

<#
.SYNOPSIS
    Engine shadows vs enhanced shadows at rvkcompare spots: two Vulkan runs of mods/rvkcompare (shadows.enabled false /
    true), side-by-side pictures (left engine, right enhanced) in out\shadows\<stamp>\ (docs/research/shadows.md).

.EXAMPLE
    .\scripts\shadow-compare.ps1
    .\scripts\shadow-compare.ps1 -Spots shadow_inland_c,beach_sun -Setting 'shadows.debug=cascades'
    .\scripts\shadow-compare.ps1 -EngineDir out\shadows\20261022-170000 -Setting 'shadows.filter=medium'
#>
param(
    [string]$Spots = 'shadow_inland_a,shadow_inland_b,shadow_inland_top,shadow_inland_high,shadow_inland_far,beach_sun',
    # extra player settings of the enhanced run (shadows.enabled=true is always set)
    [string[]]$Setting = @(),
    # extra player settings of the engine run too (e.g. the shadow map size / range, which apply to both)
    [string[]]$EngineSetting = @(),
    [string]$GameDir = 'B:\DevDrive\risen-test-vk',
    # risen-spass instance id (run.ps1 -Instance): needed to run beside another Risen
    [int]$Instance = 0,
    [string[]]$Param = @(),
    [switch]$Validation,
    [switch]$Perf,
    [switch]$SkipEngine,
    # reuse the engine pictures of an earlier run (out\shadows\<stamp>)
    [string]$EngineDir = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $repo "out\shadows\$stamp"
New-Item -ItemType Directory -Force $out | Out-Null
$shots = Join-Path $GameDir 'bin\spass\screenshots'
$spotParam = 'spots=' + ($Spots -replace ',', ';')

function Invoke-Run([string]$tag, [string[]]$settings) {
    if (Test-Path $shots) { Get-ChildItem $shots -Filter "${tag}_*" | Remove-Item -Force }
    $runArgs = @{ Mode = 'vulkan'; GameDir = $GameDir; Instance = $Instance; Mods = @('rvkcompare'); Scenario = 'rvkcompare'; WaitForResult = $true; Seconds = 1200
                  Param = @("tag=$tag", $spotParam, 'burst=1') + $Param; Setting = $settings; RvkFilter = 'enh: shadows|\[error\]|VUID' }
    if ($Validation) { $runArgs.Validation = $true }
    if ($Perf) { $runArgs.Perf = $true }
    & (Join-Path $PSScriptRoot 'run.ps1') @runArgs | Out-Host
    $dir = Join-Path $out $tag
    New-Item -ItemType Directory -Force $dir | Out-Null
    Get-ChildItem $shots -Filter "${tag}_*.png" -ErrorAction SilentlyContinue | Move-Item -Destination $dir -Force
    Copy-Item (Join-Path $GameDir 'bin\rvk\rvk.log') (Join-Path $dir 'rvk.log') -Force -ErrorAction SilentlyContinue
}

if ($EngineDir) { Copy-Item -Recurse (Join-Path $EngineDir 'eng') (Join-Path $out 'eng') }
elseif (-not $SkipEngine) { Invoke-Run 'eng' (@('shadows.enabled=false') + $EngineSetting) }
Invoke-Run 'enh' (@('shadows.enabled=true') + $EngineSetting + $Setting)

# side by side (left: engine, right: enhanced), letterbox cropped
$py = @'
import sys, os, re
from PIL import Image
out = sys.argv[1]
eng, enh = os.path.join(out, 'eng'), os.path.join(out, 'enh')
if not os.path.isdir(enh):
    sys.exit(0)
for f in sorted(os.listdir(enh)):
    m = re.match(r'enh_(.+?)(_b\d+)?\.png$', f)
    if not m:
        continue
    spot = m.group(1)
    b = Image.open(os.path.join(enh, f)).convert('RGB')
    pe = os.path.join(eng, 'eng_' + spot + (m.group(2) or '') + '.png')
    a = Image.open(pe).convert('RGB') if os.path.exists(pe) else Image.new('RGB', b.size)
    w, h = b.size
    crop = (0, 113, w, 787) if h == 900 else (0, 0, w, h)
    a, b = a.crop(crop), b.crop(crop)
    s = Image.new('RGB', (a.width + b.width, a.height))
    s.paste(a, (0, 0))
    s.paste(b, (a.width, 0))
    s.save(os.path.join(out, 'sbs_' + spot + '.png'))
    print('sbs_' + spot + '.png')
'@
$pyFile = Join-Path $out 'sbs.py'
Set-Content -Path $pyFile -Value $py -Encoding utf8
python -I $pyFile $out
Write-Host "side by side: $out"

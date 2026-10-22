<#
.SYNOPSIS
    Engine water vs enhanced water at the rvkcompare water spots: two Vulkan runs of mods/rvkcompare (water.enabled false /
    true), side-by-side pictures (left engine, right enhanced) in out\water\<stamp>\ (docs/research/water.md 8).

.EXAMPLE
    .\scripts\water-compare.ps1 -EngineSetting 'water.enabled=true','rt.reflections=off' -Setting 'rt.reflections=medium'
    .\scripts\water-compare.ps1
    .\scripts\water-compare.ps1 -Spots beach_sun,swamp_pond -Setting 'water.quality=high' -Perf
#>
param(
    [string]$Spots = 'beach_sun,beach_rain,harbour_docks,lake_jungle,waterfall_jungle,swamp_pond,swamp_camp,crystal_cave',
    # extra player settings of the enhanced run (water.enabled=true is always set)
    [string[]]$Setting = @(),
    # the left run's player settings instead of the engine's water (water.enabled=false): e.g. SSR vs ray-traced reflections
    # -EngineSetting 'water.enabled=true','rt.reflections=off' -Setting 'rt.reflections=medium'
    [string[]]$EngineSetting = @(),
    [string]$GameDir = 'B:\DevDrive\risen-test-vk',
    # risen-spass instance id (run.ps1 -Instance): needed to run beside another Risen
    [int]$Instance = 0,
    [string[]]$Param = @(),
    [switch]$Validation,
    [switch]$Perf,
    [switch]$SkipEngine,
    # reuse the engine pictures of an earlier run (out\water\<stamp>)
    [string]$EngineDir = '',
    # a third run with water.debug=normals: the water mask of each spot for the luma / hue numbers (tools/water_stats.py)
    [switch]$Mask,
    # reuse the masks of an earlier run (out\water\<stamp>)
    [string]$MaskDir = '',
    # a motion sequence of the enhanced run: -Burst 8 -Interval 6 = 8 pictures 6 frames apart per spot (motion_<spot>.png:
    # a contact sheet, plus the single frames in enh\); the side by side uses the first
    [int]$Burst = 1,
    [int]$Interval = 10
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $repo "out\water\$stamp"
New-Item -ItemType Directory -Force $out | Out-Null
$shots = Join-Path $GameDir 'bin\spass\screenshots'
$spotParam = 'spots=' + ($Spots -replace ',', ';')

function Invoke-Run([string]$tag, [string[]]$settings, [int]$burstCount = 1) {
    if (Test-Path $shots) { Get-ChildItem $shots -Filter "${tag}_*" | Remove-Item -Force }
    $runArgs = @{ Mode = 'vulkan'; GameDir = $GameDir; Instance = $Instance; Mods = @('rvkcompare'); Scenario = 'rvkcompare'; WaitForResult = $true; Seconds = 1200
                  Param = @("tag=$tag", $spotParam, "burst=$burstCount", "interval=$Interval") + $Param; Setting = $settings; RvkFilter = 'enh: water: (first|wet|material .* none)|\[error\]' }
    if ($Validation) { $runArgs.Validation = $true }
    if ($Perf) { $runArgs.Perf = $true }
    & (Join-Path $PSScriptRoot 'run.ps1') @runArgs | Out-Host
    $dir = Join-Path $out $tag
    New-Item -ItemType Directory -Force $dir | Out-Null
    Get-ChildItem $shots -Filter "${tag}_*.png" -ErrorAction SilentlyContinue | Move-Item -Destination $dir -Force
    Copy-Item (Join-Path $GameDir 'bin\rvk\rvk.log') (Join-Path $dir 'rvk.log') -Force -ErrorAction SilentlyContinue
}

if ($EngineDir) { Copy-Item -Recurse (Join-Path $EngineDir 'eng') (Join-Path $out 'eng') }
elseif (-not $SkipEngine) { Invoke-Run 'eng' $(if ($EngineSetting.Count -gt 0) { $EngineSetting } else { @('water.enabled=false') }) }
Invoke-Run 'enh' (@('water.enabled=true') + $Setting) $Burst
if ($MaskDir) { Copy-Item -Recurse (Join-Path $MaskDir 'msk') (Join-Path $out 'msk') }
elseif ($Mask) { Invoke-Run 'msk' @('water.enabled=true', 'water.debug=normals') }

# side by side (left: engine, right: enhanced)
$py = @'
import sys, os
from PIL import Image
out = sys.argv[1]
eng, enh = os.path.join(out, 'eng'), os.path.join(out, 'enh')
if not os.path.isdir(enh):
    sys.exit(0)
import re
bursts = {}
for f in sorted(os.listdir(enh)):
    m = re.match(r'enh_(.+)_b(\d+)\.png$', f)
    if m:
        bursts.setdefault(m.group(1), []).append((int(m.group(2)), f))
for spot, frames in bursts.items():
    frames.sort()
    ims = [Image.open(os.path.join(enh, f)).convert('RGB') for _, f in frames]
    w, h = ims[0].size
    crop = (0, 113, w, 787) if h == 900 else (0, 0, w, h)
    ims = [i.crop(crop) for i in ims]
    cols = 2
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * ims[0].width // 2, rows * ims[0].height // 2))
    for k, i in enumerate(ims):
        sheet.paste(i.resize((i.width // 2, i.height // 2)), ((k % cols) * i.width // 2, (k // cols) * i.height // 2))
    sheet.save(os.path.join(out, 'motion_' + spot + '.png'))
    print('motion_' + spot + '.png')
    first = frames[0][1]
    os.replace(os.path.join(enh, first), os.path.join(enh, 'enh_' + spot + '.png'))
for f in sorted(os.listdir(enh)):
    if not f.endswith('.png') or re.search(r'_b\d+\.png$', f):
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
# luma / hue / saturation of the water, engine vs enhanced (over the masks of the debug run when there are some)
python -I (Join-Path $repo 'tools\water_stats.py') $out
Write-Host "side by side: $out"

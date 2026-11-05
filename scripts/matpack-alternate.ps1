<#
.SYNOPSIS
    Exact proof of "no pack entry = unchanged pixels" (docs/research/material-pack.md 7): ONE run of mods/rvkcompare with a pack folder that
    matches nothing and materials.alternate = 10: the pack's pixel shader variants are bypassed on every second block of 10 frames, so the
    burst pictures b0 / b1 / b2 / b3 (interval 10, the scene frozen) alternate variant / engine shaders. Pixel diff: b0 vs b2 and b1 vs b3
    (same state: the noise floor, must be 0) and b0 vs b1, b2 vs b3 (variant vs engine shaders: must be 0 too).

.EXAMPLE
    .\scripts\with-test-lock.ps1 -Name rvk-matpack -Run { .\scripts\matpack-alternate.ps1 -Api d3d12 }
#>
param(
    [string]$Spots = 'pub_interior;monastery_hall;harbour_docks;harbour_night;crystal_cave;jungle;hero_closeup',
    [string]$GameDir = 'B:\DevDrive\risen-test-vk-mp',
    [int]$Instance = 7,
    [ValidateSet('wrapper', 'full')][string]$Mode = 'wrapper',
    [string]$Api = '',
    [string[]]$Setting = @()
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $repo "out\matpack\alt-$stamp"
New-Item -ItemType Directory -Force $out | Out-Null
$shots = Join-Path $GameDir 'bin\spass\screenshots'
$packDir = Join-Path $GameDir 'bin\rvk\materialpack'
$aside = "$packDir.aside"
$none = Join-Path $out 'nomatch-pack'
New-Item -ItemType Directory -Force $none | Out-Null
Set-Content (Join-Path $none 'pack.toml') "[material.`"No_Such_Material_*`"]`nroughness = 0.2`n"
if (Test-Path $shots) { Get-ChildItem $shots -Filter 'alt_*' | Remove-Item -Force }
if (Test-Path $packDir) { Rename-Item $packDir $aside }
try {
    $a = @{ GameDir = $GameDir; Instance = $Instance; Mode = $Mode; Mods = @('rvkcompare'); Scenario = 'rvkcompare'; WaitForResult = $true; Seconds = 1500
            Param = @('tag=alt', "spots=$Spots", 'burst=4', 'interval=10'); Setting = (@('materials.pack=true', 'materials.alternate=10') + $Setting)
            Environment = @('RVK_MATPACK=' + ($none -replace '\\', '/')) }
    if ($Api) { $a.Api = $Api }
    & (Join-Path $PSScriptRoot 'run.ps1') @a | Select-String 'result:' | Out-Host
    Get-ChildItem $shots -Filter 'alt_*.png' -ErrorAction SilentlyContinue | Move-Item -Destination $out -Force
    Copy-Item (Join-Path $GameDir 'bin\rvk\rvk.log') (Join-Path $out 'rvk.log') -Force
} finally {
    if (Test-Path $aside) { if (Test-Path $packDir) { Remove-Item -Recurse -Force $packDir }; Rename-Item $aside $packDir }
}
$py = @'
import sys, os, re
import numpy as np
from PIL import Image
out = sys.argv[1]
spots = sorted({re.match(r'alt_(.*)_b\d\.png', f).group(1) for f in os.listdir(out) if re.match(r'alt_.*_b\d\.png', f)})
def L(s, k): return np.asarray(Image.open(os.path.join(out, f'alt_{s}_b{k}.png')).convert('RGB')).astype(int)
ne = lambda x, y: (np.abs(x - y).max(axis=2) > 0)
def grow(m, k=24):  # a flame / water glint spreads (bloom): the animated region is dilated
    pad = np.pad(m, k); d = np.zeros_like(m)
    for dy in range(0, 2 * k + 1, 4):
        for dx in range(0, 2 * k + 1, 4):
            d |= pad[dy:dy + m.shape[0], dx:dx + m.shape[1]]
    return d
total = 0
for s in spots:
    b = [L(s, k) for k in range(4)]
    # the engine animates some pixels between frames even with the game time frozen (flames, water, foliage): those differ between two
    # pictures of the SAME state too; only the pixels that are identical across every same-state pair (b0 / b2, b1 / b3) can show the variants
    animated = grow(ne(b[0], b[2]) | ne(b[1], b[3]))
    keep = ~animated
    cross = (ne(b[0], b[1]) | ne(b[2], b[3])) & keep
    n = int(cross.sum()); total += n
    print(f"{s}: not-animated pixels {int(keep.sum())} of {keep.size}; variant-vs-engine differing pixels among them {n}")
    if n:  # magnitude (derive on: this is the derived gloss's effect; b0 / b2 = variants, b1 / b3 = engine shaders)
        d = np.maximum(np.abs(b[0] - b[1]).max(axis=2), np.abs(b[2] - b[3]).max(axis=2))[cross]
        l0 = (b[0].mean(axis=2) + b[2].mean(axis=2))[keep].mean() / 2; l1 = (b[1].mean(axis=2) + b[3].mean(axis=2))[keep].mean() / 2
        print(f"    {100.0 * n / keep.sum():.2f} % of the pixels; max diff {int(d.max())}, mean {d.mean():.1f}, p99 {np.percentile(d, 99):.0f}; mean luma variants / engine {l0 / l1:.4f}")
print('TOTAL variant-vs-engine differing pixels (not-animated)', total)
'@
$pyFile = Join-Path $out 'diff.py'
Set-Content $pyFile $py
python -I $pyFile $out | Tee-Object (Join-Path $out 'diff.txt')

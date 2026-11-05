<#
.SYNOPSIS
    Proves "no pack entry = unchanged pixels" (docs/research/material-pack.md 7): two Vulkan runs of mods/rvkcompare, pack off
    (materials.pack=false) vs. pack ON with a pack folder that matches nothing; then a pixel diff of the captures.

.EXAMPLE
    .\scripts\with-test-lock.ps1 -Name rvk-matpack -Who matpack -Run { .\scripts\matpack-nomatch.ps1 -GameDir B:\DevDrive\rvk-work\copies\risen-test-vk-mp -Instance 7 }
#>
param(
    [string]$Spots = 'pub_interior;monastery_hall;harbour_docks;harbour_night;crystal_cave',
    [string]$GameDir = 'B:\DevDrive\rvk-work\copies\risen-test-vk-mp',
    [int]$Instance = 7,
    [ValidateSet('wrapper', 'full')][string]$Mode = 'wrapper',
    [string]$Api = '',
    [string[]]$Setting = @()
)
. "$PSScriptRoot\lib\rvk-work.ps1"; $GameDir = Resolve-RvkGameDir $GameDir
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $repo "out\matpack\$stamp"
New-Item -ItemType Directory -Force $out | Out-Null
$shots = Join-Path $GameDir 'bin\spass\screenshots'
$packDir = Join-Path $GameDir 'bin\rvk\materialpack'
$aside = "$packDir.aside"
$none = Join-Path $out 'nomatch-pack'
New-Item -ItemType Directory -Force $none | Out-Null
# a pack that matches nothing: one wildcard rule and one map for a name no material has
Set-Content (Join-Path $none 'pack.toml') "[material.`"No_Such_Material_*`"]`nroughness = 0.2`n"
[System.IO.File]::WriteAllBytes((Join-Path $none 'No_Such_Material_01_r.png'), [Convert]::FromBase64String('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAAAAAA6fptVAAAACklEQVR4nGNgAAAAAgABSK+kcQAAAABJRU5ErkJggg=='))
if (Test-Path $packDir) { Rename-Item $packDir $aside }
try {
    foreach ($arm in @(@{ tag = 'off'; pack = 'false' }, @{ tag = 'off2'; pack = 'false' }, @{ tag = 'nomatch'; pack = 'true' })) {
        if (Test-Path $shots) { Get-ChildItem $shots -Filter "$($arm.tag)_*" | Remove-Item -Force }
        $a = @{ GameDir = $GameDir; Instance = $Instance; Mode = $Mode; Mods = @('rvkcompare'); Scenario = 'rvkcompare'; WaitForResult = $true; Seconds = 1200
                Param = @("tag=$($arm.tag)", "spots=$Spots"); Setting = (@("materials.pack=$($arm.pack)") + $Setting)
                Environment = @("RVK_MATPACK=" + ($none -replace "\\", "/")) }
        if ($Api) { $a.Api = $Api }
        & (Join-Path $PSScriptRoot 'run.ps1') @a | Select-String 'result:' | Out-Host
        $dir = Join-Path $out $arm.tag
        New-Item -ItemType Directory -Force $dir | Out-Null
        Get-ChildItem $shots -Filter "$($arm.tag)_*.png" -ErrorAction SilentlyContinue | Move-Item -Destination $dir -Force
        Copy-Item (Join-Path $GameDir 'bin\rvk\rvk.log') (Join-Path $dir 'rvk.log') -Force
    }
} finally {
    if (Test-Path $aside) { if (Test-Path $packDir) { Remove-Item -Recurse -Force $packDir }; Rename-Item $aside $packDir }
}
$py = @'
import sys, os
import numpy as np
from PIL import Image
out = sys.argv[1]
def load(arm, name):
    return np.asarray(Image.open(os.path.join(out, arm, name)).convert('RGB')).astype(int)
for other in ('off2', 'nomatch'):
    bad = 0; files = 0; worst = 0
    for f in sorted(os.listdir(os.path.join(out, 'off'))):
        if not f.endswith('.png'): continue
        g = f.replace('off_', other + '_', 1)
        if not os.path.exists(os.path.join(out, other, g)): continue
        dd = np.abs(load('off', f) - load(other, g)); n = int((dd.max(axis=2) > 0).sum()); files += 1
        print(f"off vs {other}: {f[4:]}: differing pixels {n}, max {int(dd.max())}, mean {dd.mean():.4f}")
        bad += n; worst = max(worst, int(dd.max()))
    print(f'off vs {other}: files {files}, total differing pixels {bad}, worst channel diff {worst}')
'@
$pyFile = Join-Path $out 'diff.py'
Set-Content $pyFile $py
python -I $pyFile $out | Tee-Object (Join-Path $out 'diff.txt')

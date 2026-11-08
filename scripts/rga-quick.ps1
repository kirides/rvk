# Quick register check of the ubershader's variants on gfx1035 (frame-ownership.md 14.7): builds the sample modules from the working-tree shaders
# (RVK_SHADER_DIR, no rebuild) and runs RGA on them.
#   .\scripts\rga-quick.ps1 -Name try1 [-Stride 20] [-Float] [-Variants gi_cl,gbuf_cl]
param([Parameter(Mandatory)] [string] $Name, [int] $Stride = 20, [switch] $Float, [string] $Variants = 'plain,scene2,gi_cl,gbuf_cl')
$ErrorActionPreference = 'Stop'
$S = 'B:\DevDrive\rvk-work\scratch\s2'
$env:RVK_SHADER_DIR = (Resolve-Path "$PSScriptRoot\..\shaders").Path
$d = "$S\q-$Name"
Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
$half = if ($Float) { @() } else { @('--half') }
& "$PSScriptRoot\..\build\RelWithDebInfo\rvk_shadertool.exe" dxil-check uber-spv $d --variants $Variants @half --stride $Stride "$PSScriptRoot\..\out\matsrc\census_town.jsonl" "$PSScriptRoot\..\out\matsrc\census_jungle.jsonl" | Select-Object -Last 3
& "$PSScriptRoot\rga-survey.ps1" -SpirvDir $d -Out "$S\rga_q-$Name.csv" -Jobs 14 -Work "B:\DevDrive\rvk-work\scratch\rga-work-q-$Name" | Out-Null
python "$PSScriptRoot\..\tools\rga_summary.py" "$S\rga_q-$Name.csv"

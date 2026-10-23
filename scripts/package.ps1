<#
.SYNOPSIS
    Packs the player zip: unpacked onto <Risen>\bin it gives d3d9.dll + rvk\config.toml + rvk\README.txt.
.EXAMPLE
    .\scripts\package.ps1                                   # build\RelWithDebInfo\d3d9.dll -> out\package\risen-enhanced-graphics.zip
    .\scripts\package.ps1 -BinDir build-release\RelWithDebInfo -Out dist   # CI
#>
param(
    [string]$BinDir = 'build\RelWithDebInfo',
    [string]$Out = 'out\package',
    [string]$Name = 'risen-enhanced-graphics.zip'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    $dll = Join-Path $BinDir 'd3d9.dll'
    if (-not (Test-Path $dll)) { throw "$dll not built" }

    $stage = Join-Path ([IO.Path]::GetTempPath()) "rvk-package-$PID"
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force "$stage\rvk" | Out-Null
    Copy-Item $dll $stage
    Copy-Item package\rvk\config.toml "$stage\rvk"
    # FSR 3.1 (upscale.mode): AMD's signed FidelityFX DLL
    Copy-Item third_party\amd_fidelityfx\presigned_dll\amd_fidelityfx_vk.dll "$stage\rvk"

    # README.txt + the build it belongs to (bug reports), CRLF for Notepad
    $sha = git rev-parse --short HEAD
    $date = git log -1 --format=%cs
    $readme = (Get-Content package\rvk\README.txt) + "  Build: $sha ($date)"
    [IO.File]::WriteAllText("$stage\rvk\README.txt", ($readme -join "`r`n") + "`r`n")

    New-Item -ItemType Directory -Force $Out | Out-Null
    $zip = Join-Path (Resolve-Path $Out) $Name
    Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
    Remove-Item $stage -Recurse -Force
    Get-Item $zip | Format-Table Name, Length
} finally {
    Pop-Location
}

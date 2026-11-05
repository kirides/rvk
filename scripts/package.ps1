<#
.SYNOPSIS
    Packs the player zip: unpacked onto <Risen>\bin it gives d3d9.dll + rvk\config.toml + rvk\README.txt + AMD's FidelityFX DLLs.
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
    # FSR (upscale.mode, framegen.mode): AMD's signed FSR SDK 2.4.0 DLLs (loader + upscaler: FSR 3.1 and FSR 4; frame generation) and the licence notice their
    # licence asks to be reproduced with the software (the part before the list of source files)
    foreach ($f in 'amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll', 'amd_fidelityfx_framegeneration_vk.dll') {
        Copy-Item "third_party\amd_fidelityfx\signedbin\$f" "$stage\rvk"
    }
    # the runtime HLSL compiler (shader/dxc) and its licence (LLVM Apache-2.0 with LLVM exceptions: a binary redistribution reproduces it)
    Copy-Item (Join-Path $BinDir 'dxcompiler.dll') "$stage\rvk"
    Copy-Item (Join-Path $BinDir 'copyright') "$stage\rvk\dxc_LICENSE.txt"
    # the DirectX 12 Agility SDK runtime (rhi/d3d12 activates it from rvk\d3d12 when the DX12 backend is chosen; the system's d3d12.dll otherwise): D3D12Core.dll
    # is redistributable (Microsoft DirectX licence, shipped as D3D12_LICENSE.txt); the debug layer d3d12SDKLayers.dll is a developer tool and is not shipped
    $agility = Join-Path $BinDir 'd3d12'
    if (Test-Path (Join-Path $agility 'D3D12Core.dll')) {
        New-Item -ItemType Directory -Force "$stage\rvk\d3d12" | Out-Null
        Copy-Item (Join-Path $agility 'D3D12Core.dll') "$stage\rvk\d3d12"
        Copy-Item (Join-Path $agility 'copyright') "$stage\rvk\d3d12\D3D12_LICENSE.txt"
    } else {
        Write-Warning "$agility\D3D12Core.dll missing: the package has no Agility SDK"
    }
    $lic = @(Get-Content third_party\amd_fidelityfx\LICENSE.md)
    $end = ($lic | Select-String -Pattern '^These source files' | Select-Object -First 1).LineNumber - 1
    if ($end -lt 10) { throw 'amd_fidelityfx\LICENSE.md: end of the notice not found' }
    function Write-Notice($file, $dlls) {
        $notice = @("AMD FSR SDK 2.4.0 ($dlls): https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK", '') + $lic[0..($end - 1)]
        [IO.File]::WriteAllText($file, ($notice -join "`r`n") + "`r`n")
    }
    Write-Notice "$stage\rvk\amd_fidelityfx_LICENSE.txt" 'amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll, amd_fidelityfx_framegeneration_vk.dll'

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

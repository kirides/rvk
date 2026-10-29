<#
.SYNOPSIS
    Packs the player zip: unpacked onto <Risen>\bin it gives d3d9.dll + rvk\config.toml + rvk\README.txt, and the optional
    frame generation zip (rvk\amd_fidelityfx_framegeneration_vk.dll, unpacked onto <Risen>\bin as well).
.EXAMPLE
    .\scripts\package.ps1                                   # build\RelWithDebInfo\d3d9.dll -> out\package\risen-enhanced-graphics.zip
    .\scripts\package.ps1 -BinDir build-release\RelWithDebInfo -Out dist   # CI
#>
param(
    [string]$BinDir = 'build\RelWithDebInfo',
    [string]$Out = 'out\package',
    [string]$Name = 'risen-enhanced-graphics.zip',
    [string]$FramegenName = 'risen-enhanced-graphics-framegen.zip'
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
    # FSR (upscale.mode): AMD's signed FSR SDK 2.4.0 DLLs (loader + upscaler: FSR 3.1 and FSR 4) and the licence notice their
    # licence asks to be reproduced with the software (the part before the list of source files)
    foreach ($f in 'amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll') {
        Copy-Item "third_party\amd_fidelityfx\signedbin\$f" "$stage\rvk"
    }
    $lic = @(Get-Content third_party\amd_fidelityfx\LICENSE.md)
    $end = ($lic | Select-String -Pattern '^These source files' | Select-Object -First 1).LineNumber - 1
    if ($end -lt 10) { throw 'amd_fidelityfx\LICENSE.md: end of the notice not found' }
    function Write-Notice($file, $dlls) {
        $notice = @("AMD FSR SDK 2.4.0 ($dlls): https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK", '') + $lic[0..($end - 1)]
        [IO.File]::WriteAllText($file, ($notice -join "`r`n") + "`r`n")
    }
    Write-Notice "$stage\rvk\amd_fidelityfx_LICENSE.txt" 'amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll'

    # README.txt + the build it belongs to (bug reports), CRLF for Notepad
    $sha = git rev-parse --short HEAD
    $date = git log -1 --format=%cs
    $readme = (Get-Content package\rvk\README.txt) + "  Build: $sha ($date)"
    [IO.File]::WriteAllText("$stage\rvk\README.txt", ($readme -join "`r`n") + "`r`n")

    New-Item -ItemType Directory -Force $Out | Out-Null
    $zip = Join-Path (Resolve-Path $Out) $Name
    Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
    Remove-Item $stage -Recurse -Force

    # Frame generation (framegen.mode): the 37 MB effect DLL is a separate, optional download so the main zip stays small. Unpacked onto
    # <Risen>\bin as well it adds rvk\amd_fidelityfx_framegeneration_vk.dll (the loader finds it next to itself) and its licence notice.
    $fgStage = Join-Path ([IO.Path]::GetTempPath()) "rvk-package-fg-$PID"
    Remove-Item $fgStage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force "$fgStage\rvk" | Out-Null
    Copy-Item third_party\amd_fidelityfx\signedbin\amd_fidelityfx_framegeneration_vk.dll "$fgStage\rvk"
    Write-Notice "$fgStage\rvk\amd_fidelityfx_framegeneration_LICENSE.txt" 'amd_fidelityfx_framegeneration_vk.dll'
    $fgZip = Join-Path (Resolve-Path $Out) $FramegenName
    Compress-Archive -Path "$fgStage\*" -DestinationPath $fgZip -Force
    Remove-Item $fgStage -Recurse -Force
    Get-Item $zip, $fgZip | Format-Table Name, Length
} finally {
    Pop-Location
}

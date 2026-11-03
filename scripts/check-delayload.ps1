<#
.SYNOPSIS
    Proves the Vulkan-only path never needs the DX12 runtime: d3d9.dll must import d3d12.dll / dxgi.dll / d3dcompiler_47.dll
    only as *delay-load* dependencies (the D3D12 backend, `vulkan.api = d3d12`, is the only user), so the DLL loads and runs
    on a machine path without them (docs/research/dx12-game-port.md, WP9). Exit code 1 when one of them is a normal import.
.EXAMPLE
    .\scripts\check-delayload.ps1                                   # build\RelWithDebInfo\d3d9.dll
    .\scripts\check-delayload.ps1 -Dll build-release\RelWithDebInfo\d3d9.dll   # CI
#>
param([string]$Dll = 'build\RelWithDebInfo\d3d9.dll')
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Dll)) { throw "$Dll not found" }

# dumpbin: on PATH (a developer prompt) or through vswhere (CI runners, VS installs)
$dumpbin = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue).Source
if (-not $dumpbin) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath
        if ($vs) {
            $dumpbin = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1 -ExpandProperty FullName
        }
    }
}
if (-not $dumpbin) { throw 'dumpbin.exe not found (needs a Visual Studio install)' }

$text = & $dumpbin -dependents $Dll
$section = ''
$normal = @()
$delay = @()
foreach ($line in $text) {
    if ($line -match 'Image has the following dependencies') { $section = 'normal'; continue }
    if ($line -match 'Image has the following delay load dependencies') { $section = 'delay'; continue }
    if ($line -match '^\s+Summary') { $section = ''; continue }
    if ($line -match '^\s+(\S+\.dll)\s*$') {
        if ($section -eq 'normal') { $normal += $Matches[1] } elseif ($section -eq 'delay') { $delay += $Matches[1] }
    }
}
Write-Host ("imports: {0}" -f ($normal -join ', '))
Write-Host ("delay-load imports: {0}" -f ($delay -join ', '))
$must = 'd3d12.dll', 'dxgi.dll', 'd3dcompiler_47.dll'
$bad = $normal | Where-Object { $must -contains $_.ToLowerInvariant() }
if ($bad) {
    Write-Host ("FAIL: {0} imported at load time (must be delay-loaded: the Vulkan path has to start without the DX12 runtime)" -f ($bad -join ', ')) -ForegroundColor Red
    exit 1
}
$missing = $must | Where-Object { $delay -notcontains $_ -and ($delay | ForEach-Object { $_.ToLowerInvariant() }) -notcontains $_ }
if ($missing) { Write-Host ("note: not delay-loaded either (the D3D12 backend is not part of this build?): {0}" -f ($missing -join ', ')) }
Write-Host 'OK: no DX12 runtime import at load time'
exit 0

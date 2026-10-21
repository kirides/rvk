<#
.SYNOPSIS
    Deploys the renderer (build\<Config>\d3d9.dll + rvk\config.toml) into the test copy and runs a risen-spass scenario there.

.DESCRIPTION
    The game is driven by risen-spass's scripts/test-run.ps1 (windowed, muted, watchdog, scenario result); see its help for the
    scenario parameters. After the run the interesting lines of bin\rvk\rvk.log are printed. The exit code is test-run.ps1's
    (0 pass, 1 checks failed, 2 no result).

    -NoRenderer removes d3d9.dll from the copy: a reference run with the game's own D3D9 renderer.

.EXAMPLE
    .\scripts\run.ps1 -Mods testdemo -Scenario testdemo -WaitForResult -Seconds 240
    .\scripts\run.ps1 -Mode trace -Mods testdemo -Scenario testdemo -WaitForResult
#>
param(
    [ValidateSet('passthrough', 'trace', 'vulkan')][string]$Mode = 'passthrough',
    [switch]$NoRenderer,
    [switch]$Validation,
    [ValidateSet('trace', 'debug', 'info', 'warn', 'error')][string]$RvkLogLevel = 'info',
    [string]$Config = 'RelWithDebInfo',
    [string]$GameDir = 'B:\DevDrive\risen-test-vk',
    [string]$SpassRepo = 'B:\DevDrive\git\github\risen-spass',
    # passed to risen-spass's test-run.ps1
    [string[]]$Mods = @('testdemo'),
    [int]$Seconds = 240,
    [string]$Scenario = '',
    [switch]$WaitForResult,
    [string[]]$Param = @(),
    [string]$WindowSize = '1600x900',
    [switch]$KeepRunning,
    [switch]$NoAutostart,
    [string]$Filter = '\[error\]|\[warn\]|\[test\] (FAIL|finished|deadline)',
    # regex of rvk.log lines to print after the run ('' = none, '.' = all)
    [string]$RvkFilter = '\[(warning|error|critical)\]|probe|  \[|missing|InitializeAdmin|ms_pAPI|Direct3DCreate9|hooks|mode ='
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$GameDir = $GameDir.TrimEnd('\')
if ($GameDir -like 'B:\SteamLibrary*') { throw 'refusing to touch the real install' }
if (-not (Test-Path (Join-Path $GameDir 'bin\Risen.exe'))) {
    & (Join-Path $PSScriptRoot 'new-test-copy.ps1') -GameDir $GameDir
}
$bin = Join-Path $GameDir 'bin'
$rvkDir = Join-Path $bin 'rvk'
$rvkLog = Join-Path $rvkDir 'rvk.log'

if ($NoRenderer) {
    foreach ($f in 'd3d9.dll', 'd3d9.pdb') { Remove-Item (Join-Path $bin $f) -Force -ErrorAction SilentlyContinue }
    Write-Host 'reference run: the game''s own D3D9 renderer (no d3d9.dll proxy)'
} else {
    $dll = Join-Path $repo "build\$Config\d3d9.dll"
    if (-not (Test-Path $dll)) { throw "$dll not built (cmake --build build --config $Config)" }
    Copy-Item $dll $bin -Force
    $pdb = [IO.Path]::ChangeExtension($dll, '.pdb')
    if (Test-Path $pdb) { Copy-Item $pdb $bin -Force }
    New-Item -ItemType Directory -Force $rvkDir | Out-Null
    $cfg = @(
        '# written by scripts/run.ps1',
        '[renderer]', "mode = `"$Mode`"",
        '[log]', "level = `"$RvkLogLevel`"",
        '[vulkan]', "validation = $($Validation.IsPresent.ToString().ToLower())"
    )
    [IO.File]::WriteAllLines((Join-Path $rvkDir 'config.toml'), $cfg)
    Remove-Item $rvkLog -Force -ErrorAction SilentlyContinue
    Write-Host "deployed $dll (mode $Mode) to $bin"
}

$spassArgs = @{
    GameDir = $GameDir; Mods = $Mods; Seconds = $Seconds; Param = $Param; WindowSize = $WindowSize; Filter = $Filter
}
if ($Scenario) { $spassArgs.Scenario = $Scenario }
if ($WaitForResult) { $spassArgs.WaitForResult = $true }
if ($KeepRunning) { $spassArgs.KeepRunning = $true }
if ($NoAutostart) { $spassArgs.NoAutostart = $true }

& (Join-Path $SpassRepo 'scripts\test-run.ps1') @spassArgs
$code = $LASTEXITCODE

if (-not $NoRenderer -and $RvkFilter) {
    Write-Host "---- $rvkLog"
    if (Test-Path $rvkLog) {
        Get-Content $rvkLog | Select-String -Pattern $RvkFilter | ForEach-Object { $_.Line }
    } else {
        Write-Host '(no rvk.log: the proxy was not loaded?)'
    }
}
exit $code

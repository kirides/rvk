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
    # [vulkan] max_fps: present-rate cap, 0 = the monitor's refresh rate with vsync
    [double]$MaxFps = 0,
    # frame numbers whose back buffer the Vulkan renderer saves to bin/rvk/screenshots/frame_<n>.png, e.g. 300,900
    [string]$Screenshots = '',
    # eCApplication::Draw calls at which the engine's own snapshot is taken (BMP in <copy>\localappdata\...\Risen\ScreenShots)
    # together with a self-test of the rvk_screenshot API on the same frame (bin/rvk/screenshots/api_draw<n>.*); any mode
    [string]$Snapshots = '',
    # log the first N draws of the Vulkan renderer in detail
    [int]$LogDraws = 0,
    # log the HAL calls of this frame of the Vulkan renderer
    [int]$LogFrame = 0,
    # the Vulkan renderer refuses eCAPITexture::SaveTextureToFile; with this switch it dumps the image to bin/rvk/dumps/*.png
    [switch]$DumpSavedTextures,
    [ValidateSet('trace', 'debug', 'info', 'warn', 'error')][string]$RvkLogLevel = 'info',
    [string]$Config = 'RelWithDebInfo',
    [string]$GameDir = 'B:\DevDrive\risen-test-vk',
    # risen-spass instance id (own single-instance mutex, window offset): needed to run beside another Risen
    [int]$Instance = 0,
    [string]$SpassRepo ='B:\DevDrive\git\github\risen-spass',
    # passed to risen-spass's test-run.ps1
    [string[]]$Mods = @('testdemo'),
    # directories searched for -Mods before risen-spass's own mods (default: this repo's mods)
    [string[]]$ModPath = @(),
    [int]$Seconds = 240,
    [string]$Scenario = '',
    [switch]$WaitForResult,
    [string[]]$Param = @(),
    [string]$WindowSize = '1600x900',
    [switch]$KeepRunning,
    [switch]$NoAutostart,
    # let the game take the foreground / move the cursor (default: risen-spass background mode)
    [switch]$AllowFocus,
    [string]$Filter = '\[error\]|\[warn\]|\[test\] (FAIL|finished|deadline)',
    # regex of rvk.log lines to print after the run ('' = none, '.' = all)
    [string]$RvkFilter = '\[(warning|error|critical)\]|probe|  \[|missing|InitializeAdmin|ms_pAPI|Direct3DCreate9|hooks|mode =|selftest|snapshot'
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
        '[vulkan]', "validation = $($Validation.IsPresent.ToString().ToLower())", "max_fps = $([string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:0.0##}', $MaxFps))",
        '[debug]', "screenshot_frames = [$Screenshots]", "log_draws = $LogDraws", "log_frame = $LogFrame", "snapshot_frames = [$Snapshots]",
        "dump_saved_textures = $($DumpSavedTextures.IsPresent.ToString().ToLower())"
    )
    Remove-Item (Join-Path $rvkDir 'screenshots') -Recurse -Force -ErrorAction SilentlyContinue
    [IO.File]::WriteAllLines((Join-Path $rvkDir 'config.toml'), $cfg)
    Remove-Item $rvkLog -Force -ErrorAction SilentlyContinue
    Write-Host "deployed $dll (mode $Mode) to $bin"
}

if (-not $ModPath) { $ModPath = @(Join-Path $repo 'mods') }
$spassArgs = @{
    ModPath = $ModPath
    GameDir = $GameDir; Instance = $Instance; Mods = $Mods; Seconds = $Seconds; Param = $Param; WindowSize = $WindowSize; Filter = $Filter
}
if ($Scenario) { $spassArgs.Scenario = $Scenario }
if ($WaitForResult) { $spassArgs.WaitForResult = $true }
if ($KeepRunning) { $spassArgs.KeepRunning = $true }
if ($NoAutostart) { $spassArgs.NoAutostart = $true }
if ($AllowFocus) { $spassArgs.AllowFocus = $true }

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

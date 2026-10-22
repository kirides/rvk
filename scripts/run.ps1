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
    # [vulkan] sync_validation: the validation layer's synchronization validation too (implies -Validation; slow)
    [switch]$SyncValidation,
    # [vulkan] max_fps: present-rate cap, 0 = the monitor's refresh rate with vsync
    [double]$MaxFps = 0,
    # [vulkan] vsync: 'engine' (the game's option), 'on', 'off'
    [ValidateSet('engine', 'on', 'off')][string]$Vsync = 'engine',
    # [debug] perf: GPU timestamps + CPU timings ("perf:" lines in rvk.log every 5 s)
    [switch]$Perf,
    # [debug] perf_frame: log the GPU time of every render pass of this frame (implies -Perf)
    [int]$PerfFrame = 0,
    # [debug] rt_selftest: BLAS + TLAS + a ray query compute shader at device creation ("rt selftest" in rvk.log)
    [switch]$RtSelfTest,
    # [debug] present_stalls (any mode): sample the presenting thread's stack while a present lasts longer than this many ms;
    # "diag:" lines in rvk.log every 5 s name the modules / functions the time is spent in (docs/research/fps-limit.md)
    [double]$PresentStalls = 0,
    # [debug] environment: 'NAME=VALUE' variables set by the proxy in DllMain (run.ps1's own environment does not reach the
    # game), e.g. -Environment 'VK_LOADER_LAYERS_DISABLE=~implicit~' or 'DISABLE_VULKAN_OBS_CAPTURE=1'
    [string[]]$Environment = @(),
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
    # player settings (bin/rvk/settings.toml, docs/research/overlay.md): every run starts from the defaults (the file is
    # deleted) unless -KeepSettings; -Setting 'vulkan.max_fps=60','vulkan.vsync=off' writes a fresh file with these values
    [string[]]$Setting = @(),
    [switch]$KeepSettings,
    # the in-game overlay: -NoOverlay = [overlay] enabled = false; -OverlayInScreenshots draws it into the back buffer so
    # -Screenshots / rvk_screenshot capture it; -Commands '450: overlay open','600: set vulkan.max_fps 30' ([debug] commands)
    [switch]$NoOverlay,
    [switch]$OverlayInScreenshots,
    [string[]]$Commands = @(),
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
    # borderless fullscreen test (docs/research/window-mode.md): ConfigUser.xml <Window FullScreen="true"> (the engine's fullscreen
    # request; Vulkan mode makes it a borderless window over the monitor at the monitor's resolution). Keep such runs short.
    [switch]$Fullscreen,
    # the engine's debug keys (docs/research/debug-mode.md): <Engine><Debug TestMode="true"> in the copy's ConfigUser.xml; every
    # other run removes the attribute again (also one the game wrote itself after the cheat code "minsky")
    [switch]$TestMode,
    [switch]$KeepRunning,
    [switch]$NoAutostart,
    # let the game take the foreground / move the cursor (default: risen-spass background mode)
    [switch]$AllowFocus,
    # start a renamed copy of Risen.exe in the test copy (risen-spass test-run.ps1 -Exe; created when missing), e.g.
    # -Exe RisenTest.exe: escapes per-executable driver profiles such as an AMD Radeon Chill cap (docs/research/fps-limit.md)
    [string]$Exe = 'Risen.exe',
    [string]$Filter = '\[error\]|\[warn\]|\[test\] (FAIL|finished|deadline)',
    # regex of rvk.log lines to print after the run ('' = none, '.' = all)
    [string]$RvkFilter = '\[(warning|error|critical)\]|probe|  \[|missing|InitializeAdmin|ms_pAPI|Direct3DCreate9|hooks|mode =|selftest|snapshot|overlay|settings|caps:|ray tracing|diag:|environment:'
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
        '[vulkan]', "validation = $($Validation.IsPresent.ToString().ToLower())", "sync_validation = $($SyncValidation.IsPresent.ToString().ToLower())", "max_fps = $([string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:0.0##}', $MaxFps))",
        "vsync = $(@{ engine = '"engine"'; on = 'true'; off = 'false' }[$Vsync])",
        '[debug]', "screenshot_frames = [$Screenshots]", "log_draws = $LogDraws", "log_frame = $LogFrame", "snapshot_frames = [$Snapshots]",
        "dump_saved_textures = $($DumpSavedTextures.IsPresent.ToString().ToLower())",
        "perf = $($Perf.IsPresent.ToString().ToLower())", "perf_frame = $PerfFrame",
        "rt_selftest = $($RtSelfTest.IsPresent.ToString().ToLower())",
        "present_stalls = $([string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:0.0##}', $PresentStalls))",
        "environment = [$(($Environment | ForEach-Object { '"' + ($_ -replace '"', '') + '"' }) -join ', ')]",
        "overlay_in_screenshots = $($OverlayInScreenshots.IsPresent.ToString().ToLower())",
        "commands = [$(($Commands | ForEach-Object { '"' + ($_ -replace '"', '') + '"' }) -join ', ')]",
        '[overlay]', "enabled = $((-not $NoOverlay.IsPresent).ToString().ToLower())"
    )
    $settingsFile = Join-Path $rvkDir 'settings.toml'
    if (-not $KeepSettings) { Remove-Item $settingsFile -Force -ErrorAction SilentlyContinue }
    if ($Setting) {
        # "table.key=value" -> [table] key = value (strings quoted unless number / bool)
        $tables = [ordered]@{}
        foreach ($kv in $Setting) {
            $k, $v = $kv -split '=', 2
            $dot = $k.LastIndexOf('.')
            $t = $k.Substring(0, $dot); $name = $k.Substring($dot + 1)
            if ($v -notmatch '^(true|false|-?[0-9.]+)$') { $v = '"' + $v + '"' }
            if (-not $tables.Contains($t)) { $tables[$t] = @() }
            $tables[$t] += "$name = $v"
        }
        $lines = @('# written by scripts/run.ps1 -Setting')
        foreach ($t in $tables.Keys) { $lines += "[$t]"; $lines += $tables[$t] }
        [IO.File]::WriteAllLines($settingsFile, $lines)
    }
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
if ($Exe -ne 'Risen.exe') { $spassArgs.Exe = $Exe }
# Fullscreen request: <Window FullScreen="true"> in the copy's ConfigUser.xml (the key players have from ConfigDefault.xml;
# test-run.ps1 keeps it, it only rewrites Right / Bottom). Every other run removes the key, so the copy's ConfigLocal.xml
# (FullScreen="false") decides again and a fullscreen run never outlives itself.
$cfgUser = Join-Path $GameDir 'localappdata\Local\Risen\ConfigUser.xml'
if (Test-Path $cfgUser) {
    $xml = [IO.File]::ReadAllText($cfgUser)
    $new = [regex]::Replace($xml, '(?s)(<Window\b[^>]*?)\s+FullScreen="[^"]*"', '$1')
    if ($Fullscreen) { $new = [regex]::Replace($new, '<Window\b', '<Window FullScreen="true"', 1) }
    # TestMode: drop every TestMode attribute and Debug elements left empty, then add ours
    $new = [regex]::Replace($new, '(?s)(<Debug\b[^>]*?)\s+TestMode="[^"]*"', '$1')
    $new = [regex]::Replace($new, '(?s)\s*<Debug\s*>\s*</Debug>|\s*<Debug\s*/>', '')
    if ($TestMode) { $new = [regex]::Replace($new, '<Engine>', "<Engine>`n`t`t<Debug TestMode=`"true`">`n`t`t</Debug>", 1) }
    if ($new -ne $xml) { [IO.File]::WriteAllText($cfgUser, $new) }
    Write-Host "ConfigUser.xml: FullScreen $(if ($Fullscreen) { '"true" (borderless fullscreen test)' } else { 'key absent (windowed)' }), TestMode $(if ($TestMode) { '"true" (debug keys)' } else { 'absent' })"
} elseif ($Fullscreen -or $TestMode) {
    throw "-Fullscreen / -TestMode need $cfgUser (created by the first run)"
}

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

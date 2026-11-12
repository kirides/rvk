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
    .\scripts\run.ps1 -Mode full -Mods testdemo -Scenario testdemo -WaitForResult -Validation
#>
param(
    # [renderer] mode: the render modes original (the game's D3D9 forwarded), wrapper (the D3D9 model on Vulkan + the
    # enhancements), full (the native frame takes over every draw); the old names passthrough (= original) and vulkan
    # (= wrapper); trace (developer: D3D9 behind the tracing HAL). original / full lock the player setting renderer.mode for
    # the run; wrapper / vulkan leave it to settings.toml (the baseline: wrapper; -Setting 'renderer.mode=full' or the
    # command 'set renderer.mode full' switch at run time)
    [ValidateSet('original', 'wrapper', 'full', 'passthrough', 'trace', 'vulkan')][string]$Mode = 'original',
    [switch]$NoRenderer,
    # [vulkan] api: the graphics API (vulkan default, d3d12 = the experimental DX12 backend; -Validation turns on its debug layer + GPU-based validation)
    [ValidateSet('vulkan', 'd3d12')][string]$Api = 'vulkan',
    [switch]$Validation,
    # [vulkan] sync_validation: the validation layer's synchronization validation too (implies -Validation; slow)
    [switch]$SyncValidation,
    # [vulkan] pipeline_libraries: VK_EXT_graphics_pipeline_library (off by default; vk/pipeline_library.cpp); DXVK's recipe,
    # -Environment 'RVK_GPL_RECIPE=lto' = the former one
    [switch]$PipelineLibraries,
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
    # [debug] shader_debug_info: SPIR-V with names / source lines / the GLSL text embedded and the optimizer off (RenderDoc shows readable shaders)
    [switch]$ShaderDebug,
    # [debug] present_stalls (any mode): sample the presenting thread's stack while a present lasts longer than this many ms;
    # "diag:" lines in rvk.log every 5 s name the modules / functions the time is spent in (docs/research/fps-limit.md)
    [double]$PresentStalls = 0,
    # test-run.ps1 -StallSeconds: kill the game after this long without a new spass.log line (GPU-based validation runs need hundreds)
    [int]$StallSeconds = 0,
    # [debug] cpu_profile (Vulkan mode): a sampled flat profile of the engine thread between presents every 5 s (`diag: profile`
    # lines: per module, self and inclusive per function; docs/research/performance.md 8)
    [switch]$CpuProfile,
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
    # player settings (bin/rvk/settings.toml, docs/research/overlay.md): every run writes a fresh file (unless -KeepSettings
    # without -Setting) = the test baseline (debug mode on, the enhancements at their pre-2026-10-23 defaults: water and
    # enhanced shadows off = parity, what refcompare and the compare scripts expect) + -Setting 'vulkan.max_fps=60',...;
    # -PlayerDefaults = the shipped player defaults instead of the baseline (debug mode still on)
    [string[]]$Setting = @(),
    [switch]$KeepSettings,
    [switch]$PlayerDefaults,
    # S-9 (frame-ownership.md 14.41): every owned stage is on by default in Full (the baseline and -PlayerDefaults test that); -NoOwned turns them all off
    # (the Full of before S-9: everything composed) for bisecting; one stage off: -Setting 'native.owned_trees=false' (list: scripts/lib/stage-state.ps1)
    [switch]$NoOwned,
    # the in-game overlay: -NoOverlay = [overlay] enabled = false; -OverlayInScreenshots draws it into the back buffer so
    # -Screenshots / rvk_screenshot capture it; -Commands '450: overlay open','600: set vulkan.max_fps 30' ([debug] commands)
    [switch]$NoOverlay,
    [switch]$OverlayInScreenshots,
    [string[]]$Commands = @(),
    [ValidateSet('trace', 'debug', 'info', 'warn', 'error')][string]$RvkLogLevel = 'info',
    [string]$Config = 'RelWithDebInfo',
    [string]$GameDir = 'B:\DevDrive\rvk-work\copies\risen-test-vk',
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
    # the engine's real shutdown: after the run (scenario result / -Seconds) the game's window is closed (WM_CLOSE) instead of the
    # process being killed, so HalDevice / Device::destroy / the backend's teardown run; waits up to 60 s, reports the exit code and
    # new minidumps in bin\rvk\dumps, exit code 3 when the game crashed / hung on the way out
    [switch]$GracefulExit,
    [switch]$NoAutostart,
    # let the game take the foreground / move the cursor (default: risen-spass background mode)
    [switch]$AllowFocus,
    # start a renamed copy of Risen.exe in the test copy (risen-spass test-run.ps1 -Exe; created when missing), e.g.
    # -Exe RisenTest.exe: escapes per-executable driver profiles such as an AMD Radeon Chill cap (docs/research/fps-limit.md)
    [string]$Exe = 'Risen.exe',
    # a savegame copied over the test copy's QuickSave.save before the start (read only: e.g. a player's save; scenario
    # rvk_perfsave quick-loads it)
    [string]$QuickSave = '',
    [string]$Filter = '\[error\]|\[warn\]|\[test\] (FAIL|finished|deadline)',
    # regex of rvk.log lines to print after the run ('' = none, '.' = all)
    [string]$RvkFilter = '\[(warning|error|critical)\]|probe|  \[|missing|InitializeAdmin|ms_pAPI|Direct3DCreate9|hooks|mode =|renderer: mode|selftest|snapshot|overlay|settings|caps:|ray tracing|diag:|environment:'
)
. "$PSScriptRoot\lib\rvk-work.ps1"; $GameDir = Resolve-RvkGameDir $GameDir
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$GameDir = $GameDir.TrimEnd('\')
if ($GameDir -like 'B:\SteamLibrary*') { throw 'refusing to touch the real install' }
if (-not (Test-Path (Join-Path $GameDir 'bin\Risen.exe'))) {
    & (Join-Path $PSScriptRoot 'new-test-copy.ps1') -GameDir $GameDir
}
$bin = Join-Path $GameDir 'bin'
if ($QuickSave) {
    if (-not (Test-Path $QuickSave)) { throw "savegame $QuickSave not found" }
    $saves = Join-Path $GameDir 'localappdata\Local\Risen\SaveGames'
    New-Item -ItemType Directory -Force $saves | Out-Null
    Copy-Item $QuickSave (Join-Path $saves 'QuickSave.save') -Force
    Write-Host "quicksave: $QuickSave -> $saves\QuickSave.save"
}
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
    # FSR (enh/upscale): AMD's signed FSR SDK 2.4.0 DLLs (loader + upscaler), loaded from bin\rvk
    # + the frame generation effect DLL (framegen.mode; shipped as the separate risen-enhanced-graphics-framegen.zip)
    # (+ the same three for the DX12 backend, _dx12: 87 MB, copied only when the size differs)
    foreach ($f in 'amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll', 'amd_fidelityfx_framegeneration_vk.dll', 'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll') {
        $src = Join-Path $repo "third_party\amd_fidelityfx\signedbin\$f"
        $dst = Join-Path $rvkDir $f
        if (-not (Test-Path $dst) -or (Get-Item $dst).Length -ne (Get-Item $src).Length) { Copy-Item $src $rvkDir -Force }
    }
    Remove-Item (Join-Path $rvkDir 'amd_fidelityfx_vk.dll') -Force -ErrorAction SilentlyContinue  # the SDK 1.1.4 DLL
    # the runtime HLSL compiler (shader/dxc): the vcpkg directx-dxc port's dxcompiler.dll, copied next to d3d9.dll by the build
    Copy-Item (Join-Path $repo "build\$Config\dxcompiler.dll") $rvkDir -Force
    # the DX12 backend's Agility SDK (D3D12Core.dll + the debug layer; rhi/d3d12 looks in "d3d12" next to d3d9.dll; the game does not use the backend yet)
    $agility = Join-Path $repo "build\$Config\d3d12"
    if (Test-Path $agility) {
        New-Item -ItemType Directory -Force (Join-Path $rvkDir 'd3d12') | Out-Null
        Copy-Item (Join-Path $agility '*.dll') (Join-Path $rvkDir 'd3d12') -Force
    }
    $cfg = @(
        '# written by scripts/run.ps1',
        '[renderer]', "mode = `"$Mode`"",
        '[log]', "level = `"$RvkLogLevel`"",
        '[vulkan]', "api = `"$Api`"", "validation = $($Validation.IsPresent.ToString().ToLower())", "sync_validation = $($SyncValidation.IsPresent.ToString().ToLower())", "pipeline_libraries = $($PipelineLibraries.IsPresent.ToString().ToLower())", "max_fps = $([string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:0.0##}', $MaxFps))",
        "vsync = $(@{ engine = '"engine"'; on = 'true'; off = 'false' }[$Vsync])",
        '[debug]', "screenshot_frames = [$Screenshots]", "log_draws = $LogDraws", "log_frame = $LogFrame", "snapshot_frames = [$Snapshots]",
        "dump_saved_textures = $($DumpSavedTextures.IsPresent.ToString().ToLower())",
        "perf = $($Perf.IsPresent.ToString().ToLower())", "perf_frame = $PerfFrame",
        "rt_selftest = $($RtSelfTest.IsPresent.ToString().ToLower())",
        "shader_debug_info = $($ShaderDebug.IsPresent.ToString().ToLower())",
        "present_stalls = $([string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:0.0##}', $PresentStalls))",
        "cpu_profile = $($CpuProfile.IsPresent.ToString().ToLower())",
        # (TOML basic strings: a backslash must be escaped, else the whole config.toml fails to parse and the run silently falls back to
        # mode original; Windows paths in -Environment are common)
        "environment = [$(($Environment | ForEach-Object { '"' + (($_ -replace '"', '') -replace '\\', '\\') + '"' }) -join ', ')]",
        "overlay_in_screenshots = $($OverlayInScreenshots.IsPresent.ToString().ToLower())",
        "commands = [$(($Commands | ForEach-Object { '"' + (($_ -replace '"', '') -replace '\\', '\\') + '"' }) -join ', ')]",
        '[overlay]', "enabled = $((-not $NoOverlay.IsPresent).ToString().ToLower())"
    )
    $settingsFile = Join-Path $rvkDir 'settings.toml'
    if (-not $KeepSettings -or $Setting) {
        $values = [ordered]@{ 'overlay.debug_mode' = 'true' }
        if (-not $PlayerDefaults) {
            $values['water.enabled'] = 'false'; $values['water.quality'] = 'medium'; $values['water.ocean_quality'] = 'medium'
            $values['rt.reflections'] = 'medium'
            $values['shadows.enabled'] = 'false'; $values['shadows.filter'] = 'high'; $values['shadows.map_size'] = 'auto'
            $values['lod.trees'] = '1'; $values['lod.meshes'] = '1'; $values['lod.view'] = '1'
            # (S-9, frame-ownership.md 14.41: lighting.clustered / full_lit are no longer pinned off: the owned Full frame needs the table lighting, the baseline tests the
            # player's Full; the old pass = -Setting 'lighting.clustered=false','lighting.full_lit=false', which gates the owned stages off)
        }
        if ($NoOwned) { . (Join-Path $PSScriptRoot 'lib\stage-state.ps1'); foreach ($kv in (Get-OwnedStageSettings -Off)) { $k, $v = $kv -split '=', 2; $values[$k] = $v } }
        foreach ($kv in $Setting) { $k, $v = $kv -split '=', 2; $values[$k] = $v }
        # "table.key=value" -> [table] key = value (strings quoted unless number / bool)
        $tables = [ordered]@{}
        foreach ($k in $values.Keys) {
            $v = $values[$k]
            $dot = $k.LastIndexOf('.')
            $t = $k.Substring(0, $dot); $name = $k.Substring($dot + 1)
            if ($v -notmatch '^(true|false|-?[0-9.]+)$') { $v = '"' + (($v -replace '"', '') -replace '\\', '\\') + '"' }
            if (-not $tables.Contains($t)) { $tables[$t] = @() }
            $tables[$t] += "$name = $v"
        }
        $lines = @('# written by scripts/run.ps1 (test baseline + -Setting)')
        foreach ($t in $tables.Keys) { $lines += "[$t]"; $lines += $tables[$t] }
        [IO.File]::WriteAllLines($settingsFile, $lines)
    }
    Remove-Item (Join-Path $rvkDir 'screenshots') -Recurse -Force -ErrorAction SilentlyContinue
    [IO.File]::WriteAllLines((Join-Path $rvkDir 'config.toml'), $cfg)
    Remove-Item $rvkLog -Force -ErrorAction SilentlyContinue
    Write-Host "deployed $dll (mode $Mode) to $bin"
}

# Instance ids: risen-spass renames the engine's single-instance mutex to GenomeMutex_<id> (id = max(-Instance, 1)). A game started while another
# Risen (any copy) holds the same id quits ~0.2 s after the hooks are active, before Lua: the "game exits at once" of shared GPU sessions (two copies
# run without -Instance both take id 1). Checked here: an explicit -Instance that is taken stops the run with the holders listed; without -Instance a
# free id 2..40 is taken instead (logged).
function Get-RisenInstanceHolders([int]$Id) {
    Get-CimInstance Win32_Process -Filter "Name LIKE 'Risen%'" -ErrorAction SilentlyContinue | ForEach-Object {
        $cfgFile = Join-Path (Split-Path $_.ExecutablePath -Parent) 'spass\config.toml'
        $held = if (Test-Path $cfgFile) { (Select-String -Path $cfgFile -Pattern '^instance = "(\d+)"' | Select-Object -First 1).Matches.Groups[1].Value } else { '?' }
        if ($held -eq "$Id") { "pid $($_.ProcessId) $($_.ExecutablePath)" }
    }
}
function Test-RisenInstanceFree([int]$Id) {
    $m = $null
    if ([System.Threading.Mutex]::TryOpenExisting("GenomeMutex_$Id", [ref]$m)) { $m.Dispose(); return $false }
    return $true
}
$instanceId = [Math]::Max($Instance, 1)
if (-not (Test-RisenInstanceFree $instanceId)) {
    $holders = @(Get-RisenInstanceHolders $instanceId)
    $who = if ($holders.Count -gt 0) { $holders -join '; ' } else { 'a Risen process (not identified)' }
    if ($PSBoundParameters.ContainsKey('Instance')) {
        throw "instance id $instanceId is taken (GenomeMutex_$instanceId held by $who): the game would quit at start-up. Pass a free -Instance or wait."
    }
    $free = 2..40 | Where-Object { Test-RisenInstanceFree $_ } | Select-Object -First 1
    if (-not $free) { throw "instance id $instanceId is taken ($who) and no id 2..40 is free" }
    Write-Warning "instance id $instanceId is taken by $who; this run uses -Instance $free"
    $Instance = $free
}

if (-not $ModPath) { $ModPath = @(Join-Path $repo 'mods') }
$spassArgs = @{
    ModPath = $ModPath
    GameDir = $GameDir; Instance = $Instance; Mods = $Mods; Seconds = $Seconds; Param = $Param; WindowSize = $WindowSize; Filter = $Filter
}
if ($Scenario) { $spassArgs.Scenario = $Scenario }
if ($WaitForResult) { $spassArgs.WaitForResult = $true }
if ($StallSeconds -gt 0) { $spassArgs.StallSeconds = $StallSeconds }
if ($KeepRunning -or $GracefulExit) { $spassArgs.KeepRunning = $true }
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

$dumpDir = Join-Path $GameDir 'bin\rvk\dumps'
$dumpsBefore = @(if (Test-Path $dumpDir) { (Get-ChildItem $dumpDir).Name })
& (Join-Path $SpassRepo 'scripts\test-run.ps1') @spassArgs
$code = $LASTEXITCODE

if ($GracefulExit) {
    $exeName = [IO.Path]::GetFileNameWithoutExtension($Exe)
    $game = @(Get-Process $exeName -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$GameDir\*" })
    if ($game.Count -eq 0) {
        Write-Host "graceful exit: no $Exe of $GameDir alive after the run (it ended by itself or crashed before)"
    } else {
        $p = $game[0]
        $null = $p.Handle  # (opens the process handle now: without it ExitCode is empty after the exit)
        Write-Host "graceful exit: closing the window of pid $($p.Id)"
        $null = $p.CloseMainWindow()
        $closedAt = Get-Date
        if ($p.WaitForExit(60000)) {
            Write-Host ("graceful exit: exited, code 0x{0:X8}" -f $p.ExitCode)
            if ($p.ExitCode -ne 0) {
                # the faulting module from Windows Error Reporting's Application event 1000 (properties: 3 module, 7 offset, 8 process id)
                Start-Sleep -Seconds 2
                $ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $closedAt.AddSeconds(-5) } -ErrorAction SilentlyContinue |
                    Where-Object { $v = [string]$_.Properties[8].Value; $id = if ($v -like '0x*') { [Convert]::ToInt32($v.Substring(2), 16) } else { [int]$v }; $id -eq $p.Id } | Select-Object -First 1
                $fault = if ($ev) { '{0}+0x{1}' -f $ev.Properties[3].Value, ([string]$ev.Properties[7].Value -replace '^(0x)?0*', '') } else { '(no WER event)' }
                Write-Host "graceful exit: crash in $fault"
                # SharedBase.dll+0x7caa9 / Engine.dll+0x8f1bd = the game's own exit crash (GUI bitmaps released by Game.dll's static destructors), now
                # suppressed by engine/exit_guard (docs/research/shutdown.md): if it shows again, the guard failed: counted
                $code = 3
            }
        } else {
            Write-Host 'graceful exit: did NOT exit within 60 s of the close request; killed'
            $p.Kill()
            $null = $p.WaitForExit(30000)  # (the kill is asynchronous: the next run's d3d9.dll copy failed on the still-mapped file)
            $code = 3
        }
    }
    # (rvk_<stamp>_exit_<n>.txt = the ExitProcess caller's stack of RVK_EXIT_TRACE=1: a trace, not a fault)
    $newDumps = @(Get-ChildItem $dumpDir -ErrorAction SilentlyContinue | Where-Object { $dumpsBefore -notcontains $_.Name -and $_.Name -notmatch '_exit_\d+\.txt$' })
    if ($newDumps.Count -gt 0) {
        Write-Host "graceful exit: new dumps: $(($newDumps.Name) -join ', ')"
        $code = 3
    } else {
        Write-Host 'graceful exit: no new dumps'
    }
}

if (-not $NoRenderer -and $RvkFilter) {
    Write-Host "---- $rvkLog"
    if (Test-Path $rvkLog) {
        Get-Content $rvkLog | Select-String -Pattern $RvkFilter | ForEach-Object { $_.Line }
    } else {
        Write-Host '(no rvk.log: the proxy was not loaded?)'
    }
}
exit $code

<#
.SYNOPSIS
    D3D9 reference comparison: runs the rvkcompare spots with the game's own D3D9 renderer and with the Vulkan renderer in a
    dedicated test copy, collects the screenshot bursts and their metadata, and diffs them with tools\refcompare.py.

.DESCRIPTION
    Design and noise handling: docs/research/reference-compare.md (sections 5-8, "How to run"). Steps:
      1. free RAM check (a steamwebhelper.exe above 5 GiB is killed, user instruction); the real install is refused
      2. the dedicated copy (default B:\DevDrive\risen-test-vk-cmp, spass instance 3) is created on first use; its ConfigUser.xml
         graphics block is pinned before every run (same resolution and quality for both renderers)
      3. run A: run.ps1 -NoRenderer (the game's D3D9), tag d3d9 -> out\refcompare\<stamp>\d3d9\
         (optional -Baseline: a second D3D9 run, tag d3d9 -> ...\d3d9_b\: known-noise masks + thresholds from A vs A')
      4. run B: run.ps1 -Mode wrapper (-Mode full: the native frame), tag vk -> ...\vk\ (+ rvk.log)
      5. python tools\refcompare.py compare d3d9 vk -o report [--baseline d3d9_b | --thresholds <reference>\thresholds.json]
    Each run directory gets the PNG bursts (<tag>_<spot>_b<k>.png), meta.json (the RVKCMP lines of spass.log as a JSON list),
    spass.log, test-result.json. Exit code: refcompare.py's (0 all PASS/WARN, 1 any FAIL, 2 any INVALID), 3 when a game run
    produced no pictures.

    -Calibrate: only two D3D9 runs and `refcompare.py calibrate` -> <stamp>\thresholds.json + thresholds_masks\*.png (the
    known-noise masks). Such a directory is a stored reference: later comparisons reuse it with -ReferenceDir <stamp dir>
    (no D3D9 run; its d3d9\ set, and its thresholds.json/masks or d3d9_b\ are used). A stored reference is only valid for
    the same spots.lua, spass build and pinned configuration (refcompare.ps1 writes their hashes to reference.json and warns
    on a mismatch).

.EXAMPLE
    .\scripts\refcompare.ps1                                    # D3D9 + Vulkan, all spots
    .\scripts\refcompare.ps1 -Calibrate                         # two D3D9 runs -> thresholds + masks (a reusable reference)
    .\scripts\refcompare.ps1 -ReferenceDir out\refcompare\20261021-190000   # Vulkan run only, against the stored reference
    .\scripts\refcompare.ps1 -Spots beach_rain,pub_interior -Baseline
#>
param(
    # spot ids (comma-separated) or 'all'
    [string]$Spots = 'all',
    # also a second D3D9 run: known-noise masks and per-spot thresholds from the two reference runs
    [switch]$Baseline,
    # two D3D9 runs -> thresholds.json + masks (a reference set for -ReferenceDir); no Vulkan run
    [switch]$Calibrate,
    # a stored reference (a former output directory with d3d9\ and thresholds.json or d3d9_b\): skips the D3D9 run(s)
    [string]$ReferenceDir = '',
    [string]$GameDir = 'B:\DevDrive\risen-test-vk-cmp',
    [int]$Instance = 3,
    [int]$Seconds = 1500,
    [string]$Config = 'RelWithDebInfo',
    # extra scenario parameters, e.g. 'settle=900','burst=5'
    [string[]]$Param = @(),
    # pinned graphics configuration (ConfigUser.xml)
    [string]$WindowSize = '1600x900',
    [ValidateSet('DoF', 'None', 'Bloom', 'Blur', 'Default')][string]$PostEffect = 'DoF',
    [ValidateSet('Low', 'Medium', 'High')][string]$Quality = 'High',
    [string]$OutRoot = '',
    [switch]$Validation,
    # Vulkan run: present-rate cap (run.ps1 -MaxFps), e.g. the D3D9 run's frame rate when frame-time-driven effects matter
    [double]$MaxFps = 0,
    # Vulkan run: player settings (run.ps1 -Setting), e.g. 'rt.allow=false'
    [string[]]$Setting = @(),
    # Vulkan run: render mode (run.ps1 -Mode): wrapper (default; old name vulkan) or full (the native frame takes over)
    [ValidateSet('wrapper', 'full', 'vulkan')][string]$Mode = 'wrapper',
    [switch]$NoImages
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$GameDir = $GameDir.TrimEnd('\')
if ($GameDir -like 'B:\SteamLibrary*') { throw 'refusing to touch the real install' }
if ($GameDir -eq 'B:\DevDrive\risen-test-vk') { throw 'use a dedicated copy: -NoRenderer deletes d3d9.dll from the copy (another agent''s runs use risen-test-vk)' }
if (-not $OutRoot) { $OutRoot = Join-Path $repo 'out\refcompare' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $OutRoot $stamp
New-Item -ItemType Directory -Force $out | Out-Null
$python = 'python'
$tool = Join-Path $repo 'tools\refcompare.py'
$spotsFile = Join-Path $repo 'mods\rvkcompare\spots.lua'
$baseThresholds = Join-Path $repo 'tests\refcompare\thresholds.toml'

# ---- 1. RAM -----------------------------------------------------------------------------------------------------------
$os = Get-CimInstance Win32_OperatingSystem
$freeGiB = $os.FreePhysicalMemory / 1MB
Get-Process steamwebhelper -ErrorAction SilentlyContinue | Where-Object { $_.WorkingSet64 -gt 5GB } | ForEach-Object {
    Write-Host ("killing steamwebhelper {0} ({1:N1} GiB)" -f $_.Id, ($_.WorkingSet64 / 1GB)); Stop-Process -Id $_.Id -Force
}
Write-Host ("free RAM {0:N1} GiB" -f $freeGiB)
if ($freeGiB -lt 4) { Write-Warning 'less than 4 GiB free: the game may stall' }

# ---- 2. the copy and its configuration ----------------------------------------------------------------------------------
if (-not (Test-Path (Join-Path $GameDir 'bin\Risen.exe'))) {
    & (Join-Path $PSScriptRoot 'new-test-copy.ps1') -GameDir $GameDir
}
$cfgPath = Join-Path $GameDir 'localappdata\Local\Risen\ConfigUser.xml'

function Set-Attr($node, [string]$name, [string]$value) { $node.SetAttribute($name, $value) }
function Get-Node($xml, [string]$path) {
    # creates missing elements along an XPath-like path a/b/c
    $node = $xml.DocumentElement
    foreach ($part in $path.Split('/')) {
        $child = $node.SelectSingleNode($part)
        if (-not $child) { $child = $node.AppendChild($xml.CreateElement($part)) }
        $node = $child
    }
    return $node
}

# writes the pinned graphics block; returns a hash of the pinned values (part of the reference identity)
function Set-PinnedConfig {
    if (-not (Test-Path $cfgPath)) { throw "$cfgPath missing (the copy has no ConfigUser.xml)" }
    $resolved = (Resolve-Path $cfgPath).Path
    if ($resolved -like 'B:\SteamLibrary*' -or $resolved -like '*\data\*') { throw "refusing to write $resolved" }
    [xml]$x = Get-Content -Raw $cfgPath
    $w, $h = $WindowSize.Split('x')
    $pins = [ordered]@{
        'Engine/Render' = @{ ViewDistance = '3' }
        'Engine/Render/Shadows' = @{ Quality = $Quality }
        'Engine/Render/PostProcessing' = @{ DefaultEffect = $PostEffect }
        'Engine/Graphics' = @{ TextureFilterMode = '4' }
        'Engine/Vegetation' = @{ Quality = $Quality }
        'Engine/Effect' = @{ Quality = $Quality }
        'Engine/Cache' = @{ Preset = 'High' }
        'Engine/Window' = @{ Left = '0'; Top = '0'; Right = $w; Bottom = $h; FOV = '60.000000'; Vsync = 'true'; AdapterNumber = '0' }
        'Game' = @{ EnableTutorialSystem = 'false'; UserPageMode = '7'; ShowFocusHighlights = 'true'; ShowSubtitles = 'true' }
    }
    $ident = @()
    foreach ($k in $pins.Keys) {
        $n = Get-Node $x $k
        foreach ($a in ($pins[$k].Keys | Sort-Object)) { Set-Attr $n $a $pins[$k][$a]; $ident += "$k@$a=$($pins[$k][$a])" }
    }
    $x.Save($cfgPath)
    return ($ident -join ';')
}

function Get-Hash([string]$text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))) -replace '-', '').Substring(0, 16)
}
function Get-FileHash16([string]$path) { if (Test-Path $path) { return (Get-FileHash $path -Algorithm SHA256).Hash.Substring(0, 16) } else { return '' } }

# the renderer is snapshotted at the start (another session may rebuild build\<Config> meanwhile): run.ps1 deploys from
# build\refcompare-snapshot (-Config refcompare-snapshot)
$snapConfig = 'refcompare-snapshot'
if (-not $Calibrate) {
    $src = Join-Path $repo "build\$Config"
    if (-not (Test-Path (Join-Path $src 'd3d9.dll'))) { throw "$src\d3d9.dll not built" }
    $snap = Join-Path $repo "build\$snapConfig"
    New-Item -ItemType Directory -Force $snap | Out-Null
    foreach ($f in 'd3d9.dll', 'd3d9.pdb') { if (Test-Path (Join-Path $src $f)) { Copy-Item (Join-Path $src $f) $snap -Force } }
    Write-Host ("renderer snapshot: {0} ({1})" -f (Join-Path $snap 'd3d9.dll'), (Get-Item (Join-Path $src 'd3d9.dll')).LastWriteTime.ToString('s'))
}

# risen-spass is loaded as bin\sentry.dll (the original is sentry_orig.dll)
$spassDll = Join-Path $GameDir 'bin\sentry.dll'

# ---- 3./4. one game run -------------------------------------------------------------------------------------------------
function Invoke-GameRun([string]$tag, [string]$dir, [bool]$d3d9) {
    $pinned = Set-PinnedConfig
    $shots = Join-Path $GameDir 'bin\spass\screenshots'
    if (Test-Path $shots) { Get-ChildItem $shots -Filter "${tag}_*" | Remove-Item -Force }
    $spassLog = Join-Path $GameDir 'bin\spass\spass.log'
    $result = Join-Path $GameDir 'bin\spass\test-result.json'
    Remove-Item $result -Force -ErrorAction SilentlyContinue
    $p = @("tag=$tag", "spots=$($Spots -replace ',', ';')") + $Param
    $runArgs = @{
        GameDir = $GameDir; Instance = $Instance; Mods = @('rvkcompare'); Scenario = 'rvkcompare'; WaitForResult = $true
        Seconds = $Seconds; Param = $p; WindowSize = $WindowSize; Config = $snapConfig
    }
    if ($d3d9) { $runArgs.NoRenderer = $true } else { $runArgs.Mode = $Mode; $runArgs.MaxFps = $MaxFps; if ($Validation) { $runArgs.Validation = $true }; if ($Setting) { $runArgs.Setting = $Setting } }
    Write-Host "==== run $tag ($(if ($d3d9) { 'game D3D9' } else { "Vulkan, $Mode" })) -> $dir"
    $t0 = Get-Date
    & (Join-Path $PSScriptRoot 'run.ps1') @runArgs | Out-Host
    $code = $LASTEXITCODE
    New-Item -ItemType Directory -Force $dir | Out-Null
    $files = @(Get-ChildItem $shots -Filter "${tag}_*.png" -ErrorAction SilentlyContinue)
    foreach ($f in $files) { Move-Item $f.FullName (Join-Path $dir $f.Name) -Force }
    if (Test-Path $spassLog) {
        Copy-Item $spassLog (Join-Path $dir 'spass.log') -Force
        $meta = @(Get-Content $spassLog | Where-Object { $_ -match 'RVKCMP (\{.*\})$' } | ForEach-Object { $Matches[1] })
        Set-Content -Path (Join-Path $dir 'meta.json') -Value ("[`n" + ($meta -join ",`n") + "`n]") -Encoding utf8
    }
    if (Test-Path $result) { Copy-Item $result (Join-Path $dir 'test-result.json') -Force }
    $rvkLog = Join-Path $GameDir 'bin\rvk\rvk.log'
    if (-not $d3d9 -and (Test-Path $rvkLog)) { Copy-Item $rvkLog (Join-Path $dir 'rvk.log') -Force }
    $info = [ordered]@{
        tag = $tag; renderer = $(if ($d3d9) { 'd3d9' } else { 'vulkan' }); exit_code = $code; pictures = $files.Count
        seconds = [int]((Get-Date) - $t0).TotalSeconds; spots = $Spots; params = $p; pinned_config = $pinned
        spots_lua = (Get-FileHash16 $spotsFile); main_lua = (Get-FileHash16 (Join-Path $repo 'mods\rvkcompare\main.lua'))
        spass_dll = (Get-FileHash16 $spassDll)
    }
    if (-not $d3d9) {
        $dll = Join-Path $repo "build\$snapConfig\d3d9.dll"
        $info.d3d9_dll = (Get-FileHash16 $dll); $info.d3d9_dll_time = (Get-Item $dll).LastWriteTime.ToString('s')
        $info.git_head = (& git -C $repo rev-parse --short HEAD); $info.git_dirty_src = [bool](& git -C $repo status --porcelain -- src)
    }
    $info | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $dir 'run.json') -Encoding utf8
    Write-Host ("run {0}: exit {1}, {2} pictures, {3} s" -f $tag, $code, $files.Count, $info.seconds)
    return $files.Count
}

function Get-Identity { return [ordered]@{ spots_lua = (Get-FileHash16 $spotsFile); main_lua = (Get-FileHash16 (Join-Path $repo 'mods\rvkcompare\main.lua')); spass_dll = (Get-FileHash16 $spassDll); thresholds_toml = (Get-FileHash16 $baseThresholds); window = $WindowSize; post = $PostEffect; quality = $Quality; spots = $Spots } }

# ---- runs ---------------------------------------------------------------------------------------------------------------
$refA = Join-Path $out 'd3d9'
$refA2 = Join-Path $out 'd3d9_b'
$thresholds = ''
if ($ReferenceDir) {
    $ReferenceDir = (Resolve-Path $ReferenceDir).Path
    $refA = Join-Path $ReferenceDir 'd3d9'
    if (-not (Test-Path $refA)) { throw "$refA missing" }
    $refA2 = Join-Path $ReferenceDir 'd3d9_b'
    $t = Join-Path $ReferenceDir 'thresholds.json'
    if (Test-Path $t) { $thresholds = $t }
    $idFile = Join-Path $ReferenceDir 'reference.json'
    if (Test-Path $idFile) {
        $stored = Get-Content -Raw $idFile | ConvertFrom-Json
        $now = Get-Identity
        foreach ($k in $now.Keys) {
            if ($k -ne 'spots' -and "$($stored.$k)" -ne "$($now[$k])") { Write-Warning "reference $k differs: stored $($stored.$k), now $($now[$k]) (re-run -Calibrate)" }
        }
    }
    Write-Host "reference: $ReferenceDir$(if ($thresholds) { ' (thresholds + masks)' } elseif (Test-Path $refA2) { ' (baseline run)' })"
} else {
    if ((Invoke-GameRun 'd3d9' $refA $true) -eq 0) { Write-Error 'the D3D9 run produced no pictures'; exit 3 }
    if ($Baseline -or $Calibrate) {
        if ((Invoke-GameRun 'd3d9' $refA2 $true) -eq 0) { Write-Error 'the second D3D9 run produced no pictures'; exit 3 }
    }
    Get-Identity | ConvertTo-Json | Set-Content (Join-Path $out 'reference.json') -Encoding utf8
}

if ($Calibrate) {
    $tf = Join-Path $out 'thresholds.json'
    & $python $tool calibrate $refA $refA2 -o $tf --tag-a d3d9 --tag-b d3d9 --thresholds $baseThresholds
    $code = $LASTEXITCODE
    Write-Host "reference set: $out (use -ReferenceDir $out)"
    exit $code
}

$vk = Join-Path $out 'vk'
if ((Invoke-GameRun 'vk' $vk $false) -eq 0) { Write-Error 'the Vulkan run produced no pictures'; exit 3 }

# ---- 5. the report ------------------------------------------------------------------------------------------------------
$report = Join-Path $out 'report'
$cmpArgs = @('compare', $refA, $vk, '-o', $report, '--tag-a', 'd3d9', '--tag-b', 'vk')
if ($thresholds) { $cmpArgs += @('--thresholds', $thresholds) }
else {
    $cmpArgs += @('--thresholds', $baseThresholds)
    if (Test-Path $refA2) { $cmpArgs += @('--baseline', $refA2) }
}
if ($NoImages) { $cmpArgs += '--no-images' }
& $python $tool @cmpArgs
$code = $LASTEXITCODE
Write-Host '---- summary'
Get-Content (Join-Path $report 'summary.md') | Write-Host
Write-Host "report: $(Join-Path $report 'report.html')"
exit $code

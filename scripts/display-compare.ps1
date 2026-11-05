<#
.SYNOPSIS
    Pictures of the GUI at a configured resolution the monitor does not offer (docs/research/window-mode.md 10): HUD, ESC menu,
    inventory and character screen per case, into out\display\<stamp>\<case>\.

.DESCRIPTION
    Cases (name = config size + window mode + optional fake monitor, RVK_FAKE_MONITOR; a fake monitor never changes a real display mode):
      native      windowed 1600x900                       (the reference: nothing to resolve)
      bl_big      borderless fullscreen, config 2560x1440 (-Fullscreen)
      win_big     windowed, config 2560x1440              (larger than a 1920x1200 monitor: borderless over it)
      bl_fake     borderless, config 2560x1440, fake monitor 1600x900
      odd_fake    windowed, config 1600x900, fake monitor 1366x768 (a size that is no 16:9 / 16:10 mode)
      ex_fake     exclusive request, config 2560x1440, fake monitor 1920x1080
    -Cases a,b picks some. Everything runs in $GameDir (default: the display test copy), always through with-test-lock by the caller.
#>
param(
    [string[]]$Cases = @('native', 'bl_big', 'win_big', 'bl_fake', 'odd_fake', 'ex_fake'),
    [string]$GameDir = 'B:\DevDrive\rvk-work\copies\risen-test-vk-disp',
    [int]$Instance = 9,
    [string]$Config = 'RelWithDebInfo',
    [string]$Mode = 'wrapper',
    [string]$Spots = 'hud_view;menu_esc;gui_inventory;gui_character'
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib\rvk-work.ps1"; $GameDir = Resolve-RvkGameDir $GameDir
$repo = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $repo "out\display\$stamp"
New-Item -ItemType Directory -Force $out | Out-Null
$shots = Join-Path $GameDir 'bin\spass\screenshots'
$table = @{
    native   = @{ size = '1600x900'; full = $false; fake = '';          setting = @() }
    bl_big   = @{ size = '2560x1440'; full = $true; fake = '';          setting = @() }
    win_big  = @{ size = '2560x1440'; full = $false; fake = '';         setting = @() }
    bl_fake  = @{ size = '2560x1440'; full = $true; fake = '1600x900';  setting = @() }
    odd_fake = @{ size = '1600x900'; full = $false; fake = '1366x768';  setting = @() }
    ex_fake  = @{ size = '2560x1440'; full = $true; fake = '1920x1080'; setting = @('vulkan.fullscreen=exclusive') }
}
foreach ($c in $Cases) {
    $t = $table[$c]
    if (-not $t) { throw "unknown case $c" }
    if (Test-Path $shots) { Get-ChildItem $shots -Filter "${c}_*" | Remove-Item -Force }
    $runArgs = @{ Config = $Config; Mode = $Mode; GameDir = $GameDir; Instance = $Instance; Mods = @('rvkcompare'); Scenario = 'rvkcompare'
                  WaitForResult = $true; Seconds = 900; WindowSize = $t.size; AllowFocus = $true
                  Param = @("tag=$c", "spots=$Spots"); Setting = $t.setting
                  RvkFilter = 'window: start|window: resolution|window: display|borderless over|bordered|swapchain|WM_SIZE|exclusive|fake' }
    if ($t.full) { $runArgs.Fullscreen = $true }
    if ($t.fake) { $runArgs.Environment = @("RVK_FAKE_MONITOR=$($t.fake)") }
    & (Join-Path $PSScriptRoot 'run.ps1') @runArgs | Out-Host
    $dir = Join-Path $out $c
    New-Item -ItemType Directory -Force $dir | Out-Null
    Get-ChildItem $shots -Filter "${c}_*.png" -ErrorAction SilentlyContinue | Move-Item -Destination $dir -Force
    Copy-Item (Join-Path $GameDir 'bin\rvk\rvk.log') (Join-Path $dir 'rvk.log') -Force -ErrorAction SilentlyContinue
}
Write-Host "pictures: $out"

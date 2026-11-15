param(
    [Parameter(Mandatory)][string]$Tag,
    [ValidateSet('full', 'wrapper')][string]$Mode = 'full',
    [ValidateSet('vulkan', 'd3d12')][string]$Api = 'vulkan',
    [int]$Seconds = 180,
    [switch]$Cpu,          # -CpuProfile (spike stacks); slows the run a little: separate sessions
    [switch]$Cold,         # bin\rvk\cache removed first
    [string]$Hitch = '25',
    [string[]]$Setting = @(),
    [string[]]$Env = @(),
    [string]$GameDir = 'B:\DevDrive\rvk-work\copies\risen-test-vk',
    [int]$Instance = 0,
    [string]$LockName = 'rvk-test-copy'
)
$repo = Split-Path -Parent $PSScriptRoot
$sc = if ($env:WS26) { $env:WS26 } else { 'B:' + [char]92 + 'DevDrive' + [char]92 + 'rvk-work' + [char]92 + 'scratch' + [char]92 + 'ws26' }  # <tag>_rvk.log / _spass.log / _frames.csv / _load.csv / _run.txt
. "$repo\scripts\lib\rvk-work.ps1"
$GameDir = Resolve-RvkGameDir $GameDir
Set-Location $repo
$block = {
    if ($Cold) {
        $c = Join-Path $GameDir 'bin\rvk\cache'
        if (Test-Path $c) { Remove-Item -Recurse -Force $c }
    }
    # machine load sampler
    $load = Start-Job -ArgumentList "$sc\${Tag}_load.csv" -ScriptBlock {
        param($f)
        'time,cpu,risen,cl,other' | Set-Content $f
        while ($true) {
            $c = (Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'" -ErrorAction SilentlyContinue).PercentProcessorTime
            $r = @(Get-Process -Name 'Risen*' -ErrorAction SilentlyContinue).Count
            $cl = @(Get-Process -Name 'cl', 'link', 'msbuild', 'dxc', 'ninja' -ErrorAction SilentlyContinue).Count
            $cc = @(Get-Process -Name 'clang*', 'cmake', 'python*' -ErrorAction SilentlyContinue).Count
            ('{0},{1:N0},{2},{3},{4}' -f (Get-Date -Format 'HH:mm:ss'), $c, $r, $cl, $cc) | Add-Content $f
            Start-Sleep -Seconds 3
        }
    }
    $envs = @("RVK_HITCH_MS=$Hitch", "RVK_FRAME_CSV=$($sc -replace '\\','/')/${Tag}_frames.csv") + $Env
    $a = @{
        GameDir = $GameDir; Mode = $Mode; Api = $Api; PlayerDefaults = $true; Exe = 'RisenTest.exe'; Vsync = 'off'; Perf = $true
        Environment = $envs; Mods = @('rvk_traverse'); Scenario = 'rvk_traverse'; WaitForResult = $true
        Seconds = $Seconds + 150; Param = @("seconds=$Seconds", 'log=5')
    }
    if ($Instance) { $a.Instance = $Instance }
    if ($Cpu) { $a.CpuProfile = $true }
    if ($Setting.Count) { $a.Setting = $Setting }
    try { & "$repo\scripts\run.ps1" @a *> "$sc\${Tag}_run.txt" } finally { Stop-Job $load; Remove-Job $load -Force }
    Copy-Item (Join-Path $GameDir 'bin\rvk\rvk.log') "$sc\${Tag}_rvk.log" -Force
    Copy-Item (Join-Path $GameDir 'bin\spass\spass.log') "$sc\${Tag}_spass.log" -Force
}
& "$repo\scripts\with-test-lock.ps1" -Name $LockName -Who ws26 -Run $block.GetNewClosure()
"session $Tag done"

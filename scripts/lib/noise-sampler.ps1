param([string]$Csv, [string]$Stop, [string]$Match = 'risen-test-vk-first')
# noise record of a measurement run, every 5 s: total CPU %, this run's own game CPU % (processes started from the copy named $Match), the rest = other agents' load
# (builds, games, tools), and the number of compiler processes
"time,total_cpu,mine,others,other_games,compilers" | Set-Content $Csv
$lp = [Environment]::ProcessorCount
while (-not (Test-Path $Stop)) {
    $procs = Get-CimInstance Win32_PerfFormattedData_PerfProc_Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne '_Total' -and $_.Name -ne 'Idle' }
    $mine = @(Get-CimInstance Win32_Process -Filter "Name like 'Risen%'" -ErrorAction SilentlyContinue | Where-Object { $_.ExecutablePath -like "*$Match*" } | ForEach-Object { $_.ProcessId })
    $m = 0.0; $o = 0.0
    foreach ($p in $procs) { if ($mine -contains [int]$p.IDProcess) { $m += $p.PercentProcessorTime } else { $o += $p.PercentProcessorTime } }
    $games = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'Risen*' -and $mine -notcontains $_.Id }).Count
    $cc = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -in 'cl', 'link', 'clang-cl', 'rvk_shadertool', 'rvk_tests', 'rvk_rhitest', 'rvk_vktest', 'rvk_d3d9test', 'MSBuild', 'ninja' }).Count
    "{0},{1:N0},{2:N0},{3:N0},{4},{5}" -f (Get-Date -Format 'HH:mm:ss'), (($m + $o) / $lp), ($m / $lp), ($o / $lp), $games, $cc | Add-Content $Csv
    Start-Sleep -Seconds 4
}

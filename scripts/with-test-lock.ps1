<#
.SYNOPSIS
    Runs a script block while holding a machine-wide lock on the game test copy (B:\DevDrive\rvk-work\copies\risen-test-vk) and the GPU: several
    agents / worktrees can queue their game runs without two games running at once (a run deploys its d3d9.dll into the shared copy).

.EXAMPLE
    .\scripts\with-test-lock.ps1 -Who 'agent-A' -Run { .\scripts\water-compare.ps1 -SkipEngine -Spots jungle -Setting 'gi.enabled=true' }
    .\scripts\with-test-lock.ps1 -Run { .\scripts\run.ps1 -Mode wrapper -Mods rvkcompare -Scenario rvkcompare -WaitForResult -Seconds 600 }

    The lock is a named mutex (Global\rvk-test-copy), released when the block ends or this process dies; waiting is logged every
    minute; the longest wait is 3 hours. Hold it only for the game run itself (never while building or thinking), keep runs short.
    The script block runs in the caller's working directory (the repo / worktree the script is started from).
#>
param(
    [string]$Name = 'rvk-test-copy',
    [string]$Who = '',
    [Parameter(Mandatory)][scriptblock]$Run
)
$ErrorActionPreference = 'Stop'
$mutex = New-Object System.Threading.Mutex($false, "Global\$Name")
$label = if ($Who) { $Who } else { (Get-Location).Path }
$start = Get-Date
$held = $false
$code = 0
try {
    while (-not $held) {
        try { $held = $mutex.WaitOne([TimeSpan]::FromSeconds(60)) }
        catch [System.Threading.AbandonedMutexException] { $held = $true }  # the previous holder died: the lock is ours
        if (-not $held) {
            $waited = [int]((Get-Date) - $start).TotalMinutes
            Write-Host "with-test-lock: waiting for the test copy ($waited min, $label)"
            if ($waited -ge 180) { throw 'with-test-lock: gave up after 3 hours' }
        }
    }
    Write-Host ('with-test-lock: acquired after {0:N0} s ({1})' -f ((Get-Date) - $start).TotalSeconds, $label)
    & $Run
    if ($null -ne $LASTEXITCODE) { $code = $LASTEXITCODE }
}
finally {
    if ($held) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
Write-Host ('with-test-lock: released after {0:N0} s' -f ((Get-Date) - $start).TotalSeconds)
exit $code

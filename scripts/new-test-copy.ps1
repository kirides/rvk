<#
.SYNOPSIS
    Creates the renderer's own test copy of Risen (default B:\DevDrive\risen-test-vk) from risen-spass's main test copy.

.DESCRIPTION
    Same layout as risen-spass's parallel instances (scripts/test-run.ps1 New-TestCopy): bin is copied (without spass\), data\ini is
    copied, data\common and data\compiled stay junctions into the real install (read-only use!), ConfigUser.xml is seeded.
    The real install is never touched. Delete the directory to drop the copy.
#>
param(
    [string]$GameDir = 'B:\DevDrive\risen-test-vk',
    [string]$Template = 'B:\DevDrive\risen-test'
)
$ErrorActionPreference = 'Stop'
$GameDir = $GameDir.TrimEnd('\')
if ($GameDir -like 'B:\SteamLibrary*') { throw 'refusing to touch the real install' }
# risen-spass's test-run.ps1 only accepts copies named risen-test*
if ($GameDir -notlike 'B:\DevDrive\risen-test*') { throw "test copies must be named B:\DevDrive\risen-test*, not $GameDir" }
if (Test-Path (Join-Path $GameDir 'bin\Risen.exe')) { Write-Host "$GameDir exists already"; return }
if (-not (Test-Path (Join-Path $Template 'bin\Risen.exe'))) { throw "the template copy $Template does not exist" }

Write-Host "creating the test copy $GameDir from $Template"
New-Item -ItemType Directory -Force (Join-Path $GameDir 'data') | Out-Null
& robocopy (Join-Path $Template 'bin') (Join-Path $GameDir 'bin') /E /XD spass rvk /XF d3d9.dll d3d9.pdb /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
Copy-Item (Join-Path $Template 'data\ini') (Join-Path $GameDir 'data\ini') -Recurse -Force
foreach ($j in 'common', 'compiled') {
    $target = @((Get-Item (Join-Path $Template "data\$j") -Force).Target)[0]
    if (-not $target -or $target -notlike '*Risen\data\*') { throw "data\$j of the template is not a junction into a Risen install ($target)" }
    & cmd.exe /c mklink /J "`"$(Join-Path $GameDir "data\$j")`"" "`"$target`"" | Out-Null
}
$cfgUser = Join-Path $Template 'localappdata\Local\Risen\ConfigUser.xml'
if (Test-Path $cfgUser) {
    New-Item -ItemType Directory -Force (Join-Path $GameDir 'localappdata\Local\Risen') | Out-Null
    Copy-Item $cfgUser (Join-Path $GameDir 'localappdata\Local\Risen\ConfigUser.xml')
}
$global:LASTEXITCODE = 0
Write-Host 'done'

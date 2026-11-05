# Dot-source: . "$PSScriptRoot\lib\rvk-work.ps1"
# The ONE place that defines where testing leftovers live. Everything the renderer's tests create outside the repo goes below $RvkWork:
#   $RvkWork\copies\   game test copies (risen-test-vk, risen-test-vk-<name>, ...)
#   $RvkWork\scratch\  logs, scratch scripts, pictures, temp dirs
# Delete $RvkWork to sweep all of it. Override with the environment variable RVK_WORK. Never write scratch to the root of B:\DevDrive.
$RvkWork = if ($env:RVK_WORK) { $env:RVK_WORK.TrimEnd('\') } else { 'B:\DevDrive\rvk-work' }
$RvkCopies = Join-Path $RvkWork 'copies'
$RvkScratch = Join-Path $RvkWork 'scratch'
$RvkOldRoot = 'B:\DevDrive'   # where the copies used to live (before rvk-work)

# Maps a test copy path to its place under $RvkCopies. Accepts the old form (B:\DevDrive\risen-test-vk-x), the new form, or a bare name.
# Backward compatibility: when only the old directory exists (not moved yet), the old one is used with a warning.
function Resolve-RvkGameDir([string]$Dir) {
    $Dir = $Dir.TrimEnd('\')
    $leaf = Split-Path $Dir -Leaf
    $parent = Split-Path $Dir -Parent
    if ($parent -and ($parent -ne $RvkOldRoot) -and ($parent -ne $RvkCopies) -and ($parent -ne 'B:\DevDrive\rvk-work\copies')) { return $Dir }   # some other place: as given
    if ($leaf -notlike 'risen-test-vk*') { return $Dir }
    $new = Join-Path $RvkCopies $leaf
    $old = Join-Path $RvkOldRoot $leaf
    if (-not (Test-Path (Join-Path $new 'bin\Risen.exe')) -and (Test-Path (Join-Path $old 'bin\Risen.exe'))) {
        Write-Warning "test copy $leaf still lives at the old place $old (new place: $new); using the old one"
        return $old
    }
    return $new
}

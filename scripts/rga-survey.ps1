# Radeon GPU Analyzer survey of the renderer's pixel shader modules (frame-ownership.md 14.7): VGPRs / SGPRs / spills / scratch / ISA size per module for gfx1035 (Radeon 680M, RDNA2).
#   .\scripts\rga-survey.ps1 -SpirvDir <copy>\bin\rvk\cache\spirv -Hash 309f55b2 -Out <csv> [-Jobs 6] [-Limit N]
# Inputs are the SPIR-V modules the game wrote into its SPIR-V cache (`<shaderhash>_<tag>_t12_u<ubershader hash>.spv`: the ubershader's, so -Hash picks one text version).
# RGA 2.9.1 is the last release that lists gfx1035 (2.14+ dropped RDNA2): unpack rga-windows-x64-2.9.1.zip (GPUOpen-Tools/radeon_gpu_analyzer releases) into its own directory.
# Offline mode (vk-spv-offline) = the AMD offline compiler, not the installed driver: register numbers are the compiler's, wave32.
param(
    [Parameter(Mandatory)] [string] $SpirvDir,
    [Parameter(Mandatory)] [string] $Out,
    [string] $Hash = '',
    [string] $Rga = 'B:\DevDrive\rvk-work\scratch\rga291\x\rga.exe',
    [string] $Asic = 'gfx1035',
    [int] $Jobs = 6,
    [int] $Limit = 100000,
    [string] $Work = 'B:\DevDrive\rvk-work\scratch\rga-work'
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Rga)) { throw "rga.exe not found: $Rga" }
$files = Get-ChildItem $SpirvDir -Filter '*.spv' | Where-Object { ($Hash -eq '' -or $_.Name -like "*_u$Hash*")  } | Select-Object -First $Limit
New-Item -ItemType Directory -Force $Work | Out-Null
$rows = $files | ForEach-Object -ThrottleLimit $Jobs -Parallel {
    $f = $_
    $w = Join-Path $using:Work ([guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force $w | Out-Null
    try {
        & $using:Rga -s vk-spv-offline -c $using:Asic --frag $f.FullName --isa "$w\isa.txt" -a "$w\st.csv" 2>&1 | Out-Null
        $csv = Get-ChildItem $w -Filter '*_frag.csv' | Select-Object -First 1
        if ($csv) {
            $r = Import-Csv $csv.FullName | Select-Object -First 1
            $tag = if ($f.Name -match '_t12_u[0-9a-f]{8}h?\.spv$') { ($f.Name -replace '^[0-9a-f]{16}_', '' -replace '_t12_u[0-9a-f]{8}h?\.spv$', '') } else { ($f.BaseName -replace '^.*_PS_Default(ZPass)?_','') }
            [pscustomobject]@{ module = $f.Name; shader = $f.Name.Substring(0, 16); tag = $tag; bytes = $f.Length; vgprs = [int]$r.USED_VGPRs; sgprs = [int]$r.USED_SGPRs; vgpr_spills = [int]$r.VGPR_SPILLS; scratch = [int]$r.SCRATCH_MEM; isa_bytes = [int]$r.ISA_SIZE }
        } else {
            [pscustomobject]@{ module = $f.Name; shader = $f.Name.Substring(0, 16); tag = 'FAILED'; bytes = $f.Length; vgprs = -1; sgprs = -1; vgpr_spills = -1; scratch = -1; isa_bytes = -1 }
        }
    } finally { Remove-Item -Recurse -Force $w -ErrorAction SilentlyContinue }
}
$rows | Sort-Object tag, module | Export-Csv -NoTypeInformation $Out
"{0} modules -> {1}" -f @($rows).Count, $Out




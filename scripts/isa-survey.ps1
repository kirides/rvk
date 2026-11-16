# Offline ISA / register survey of every embedded hand-written shader (docs/research/shader-isa.md):
#   1. rvk_shadertool dxil-check embedded-spv <spv dir>  (SPIR-V of every shaders/**.{comp,vert,frag}.hlsl, same text as the renderer / DXIL checks)
#   2. RGA (vk-spv-offline = AMD's offline LLPC) per module and per target: gfx1100 (RDNA3, the RX 7900 XTX) and gfx1035 (RDNA2, Radeon 680M)
#   3. a CSV of VGPRs / SGPRs / LDS / scratch / spills / ISA instruction count + the ISA text per module (game-independent: shaders/ is the repo's own HLSL)
# RGA 2.9.1 (see rga-survey.ps1) cannot translate ray queries (RT shaders: use the driver's own numbers, RVK_ISA_DUMP below).
# The numbers of the installed driver (the real thing: wave size, no RGA offline differences) come from the renderer itself:
#   $env:RVK_ISA_DUMP = '<dir>'  (any rvk run, or rvk_vktest --isa-dir <dir> --shader-stats): statistics + ISA text of every pipeline it builds.
#   .\scripts\isa-survey.ps1 [-Out B:\DevDrive\rvk-work\scratch\ws39] [-Filter gi_] [-Jobs 8]
param(
    [string] $Out = 'B:\DevDrive\rvk-work\scratch\ws39',
    [string] $Filter = '',
    [string[]] $Asics = @('gfx1100', 'gfx1035'),
    [string] $Rga = 'B:\DevDrive\rvk-work\scratch\rga291\x\rga.exe',
    [int] $Jobs = 8,
    [switch] $SkipSpv,
    # a directory of SPIR-V modules instead of the embedded shaders (e.g. `rvk_shadertool dxil-check uber-spv`: the material ubershader's permutations); the outputs get <Tag>
    [string] $SpvDir = '',
    [string] $Tag = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path $Rga)) { throw "rga.exe not found: $Rga" }
$spv = Join-Path $Out 'spv'
if ($SpvDir -ne '') { $spv = $SpvDir; $SkipSpv = $true }
if (-not $SkipSpv) {
    Remove-Item -Recurse -Force $spv -ErrorAction SilentlyContinue
    & "$repo\build\RelWithDebInfo\rvk_shadertool.exe" dxil-check embedded-spv $spv --cache (Join-Path $Out 'dxccache') | Select-Object -Last 3
}
$files = Get-ChildItem $spv -Filter '*.spv' | Where-Object { $Filter -eq '' -or $_.Name -like "*$Filter*" }
$isaDir = Join-Path $Out ('rga-isa' + $(if ($Tag -ne '') { "-$Tag" } else { '' }))
New-Item -ItemType Directory -Force $isaDir | Out-Null
$work = Join-Path $Out 'rga-work'
New-Item -ItemType Directory -Force $work | Out-Null
$rows = foreach ($asic in $Asics) {
    $files | ForEach-Object -ThrottleLimit $Jobs -Parallel {
        $f = $_
        $asic = $using:asic
        function I($v) { $x = 0; if ([int]::TryParse([string]$v, [ref]$x)) { $x } else { 0 } }
        $stage = if ($f.Name -match '__(comp|vert|frag)') { $Matches[1] } else { 'frag' }
        $w = Join-Path $using:work ([guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Force $w | Out-Null
        try {
            & $using:Rga -s vk-spv-offline -c $asic "--$stage" $f.FullName --isa "$w\isa.txt" -a "$w\st.csv" 2>&1 | Out-Null
            $csv = Get-ChildItem $w -Filter '*.csv' | Select-Object -First 1
            $isa = Get-ChildItem $w -Filter '*.txt' | Select-Object -First 1
            if ($csv) {
                $r = Import-Csv $csv.FullName | Select-Object -First 1
                $n = 0
                if ($isa) {
                    Copy-Item $isa.FullName (Join-Path $using:isaDir ("{0}.{1}.isa.txt" -f $f.BaseName, $asic)) -Force
                    $n = @(Select-String -Path $isa.FullName -Pattern '^\s+[sv]_|^\s+(buffer|image|global|ds|scratch|flat|exp|lds)_' ).Count
                }
                [pscustomobject]@{ asic = $asic; shader = $f.BaseName; stage = $stage; vgprs = (I $r.USED_VGPRs); sgprs = (I $r.USED_SGPRs); vgpr_spills = (I $r.VGPR_SPILLS); sgpr_spills = (I $r.SGPR_SPILLS)
                    scratch = (I $r.SCRATCH_MEM); lds = (I $r.USED_LDS_BYTES); isa_instr = $n; status = 'ok' }
            } else {
                [pscustomobject]@{ asic = $asic; shader = $f.BaseName; stage = $stage; vgprs = -1; sgprs = -1; vgpr_spills = -1; sgpr_spills = -1; scratch = -1; lds = -1; isa_instr = -1; status = 'RGA failed' }
            }
        } finally { Remove-Item -Recurse -Force $w -ErrorAction SilentlyContinue }
    }
}
$csvOut = Join-Path $Out ('rga_survey' + $(if ($Tag -ne '') { "_$Tag" } else { '' }) + '.csv')
$rows | Sort-Object shader, asic | Export-Csv -NoTypeInformation $csvOut
"{0} rows -> {1} (ISA text in {2})" -f @($rows).Count, $csvOut, $isaDir

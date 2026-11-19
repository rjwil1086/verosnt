# PROJECT:     Tevera OS
# LICENSE:     GNU General Public License, version 2 or later (GPL-2.0-or-later)
# FILE:        tavera/probes/ntfs/probe-v2/host/Diff-NtfsCaptures.ps1
# PURPOSE:     Byte-diff probe-v2 variant A vs B captures for spec citations
# COPYRIGHT:   Copyright 2026 Ryan Wilson <ryan@taveraos.com>
# PROGRAMMERS: Ryan Wilson <ryan@taveraos.com>

param(
    [string]$OutRoot = ''
)

$ErrorActionPreference = 'Stop'
$Probe = Split-Path $PSScriptRoot -Parent
$SourceRoot = (Resolve-Path (Join-Path $Probe '..\..\..\..')).Path
if (-not $OutRoot) { $OutRoot = Join-Path $SourceRoot 'output\ntfs\probe-v2' }
$A = Join-Path $OutRoot 'A\captures'
$B = Join-Path $OutRoot 'B\captures'
$Report = Join-Path $OutRoot 'diff-report.txt'

function Get-Runs {
    param([byte[]]$Left, [byte[]]$Right)
    $n = [Math]::Min($Left.Length, $Right.Length)
    $runs = New-Object System.Collections.Generic.List[object]
    $i = 0
    while ($i -lt $n) {
        if ($Left[$i] -eq $Right[$i]) { $i++; continue }
        $start = $i
        while ($i -lt $n -and $Left[$i] -ne $Right[$i]) { $i++ }
        $len = $i - $start
        $aHex = ($Left[$start..([Math]::Min($start + 15, $start + $len - 1))] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
        $bHex = ($Right[$start..([Math]::Min($start + 15, $start + $len - 1))] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
        $runs.Add([pscustomobject]@{ Offset = $start; Length = $len; A = $aHex; B = $bHex })
    }
    if ($Left.Length -ne $Right.Length) {
        $runs.Add([pscustomobject]@{
            Offset = $n; Length = [Math]::Abs($Left.Length - $Right.Length)
            A = "len=$($Left.Length)"; B = "len=$($Right.Length)"
        })
    }
    return $runs
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('NTFS-DIFF-REPORT')
$lines.Add("A`t$A")
$lines.Add("B`t$B")

$pairs = @(
    @{ Name = 'quota_q_index_root'; File = 'quota_q_index_root.bin' },
    @{ Name = 'logfile_data'; File = 'logfile_data.bin' },
    @{ Name = 'secretA_data'; File = 'secretA_data.bin'; SameBoot = $true; Other = 'secretB_data.bin' },
    @{ Name = 'secretA_logged_utility'; File = 'secretA_logged_utility.bin'; SameBoot = $true; Other = 'secretB_logged_utility.bin' },
    @{ Name = 'secretA_data_cross'; File = 'secretA_data.bin'; Cross = $true },
    @{ Name = 'secretA_efs_cross'; File = 'secretA_logged_utility.bin'; Cross = $true }
)

foreach ($p in $pairs) {
    if ($p.SameBoot) {
        $leftPath = Join-Path $A $p.File
        $rightPath = Join-Path $A $p.Other
        $tag = "SAMEBOOT-A $($p.File) vs $($p.Other)"
    } elseif ($p.Cross) {
        $leftPath = Join-Path $A $p.File
        $rightPath = Join-Path $B $p.File
        $tag = "CROSS $($p.File)"
    } else {
        $leftPath = Join-Path $A $p.File
        $rightPath = Join-Path $B $p.File
        $tag = "CROSS $($p.File)"
    }
    $lines.Add("PAIR`t$tag")
    if (-not (Test-Path $leftPath) -or -not (Test-Path $rightPath)) {
        $lines.Add('MISSING')
        continue
    }
    $left = [IO.File]::ReadAllBytes($leftPath)
    $right = [IO.File]::ReadAllBytes($rightPath)
    $runs = Get-Runs $left $right
    $lines.Add("SIZES`t$($left.Length)`t$($right.Length)`tDIFFRUNS`t$($runs.Count)")
    $show = [Math]::Min(32, $runs.Count)
    for ($i = 0; $i -lt $show; $i++) {
        $r = $runs[$i]
        $lines.Add("DIFF`t$($p.Name)`toff=$($r.Offset)`tlen=$($r.Length)`tA=$($r.A)`tB=$($r.B)")
    }
    if ($runs.Count -gt $show) {
        $lines.Add("DIFF`t$($p.Name)`t...`tmore=$($runs.Count - $show)")
    }
}

$lines.Add('NTFS-DIFF-REPORT-END')
Set-Content -Path $Report -Value $lines -Encoding ascii
Write-Host "wrote $Report"
Get-Content $Report
